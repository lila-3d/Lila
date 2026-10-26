#include <doctest/doctest.h>

#include "FakeServer.h"

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "link/Exporters.h"
#include "link/UnrealLink.h"
#include "math/Unreal.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "scene/Transform.h"

#include <filesystem>
#include <fstream>
#include <mutex>

using namespace lila;
using namespace lila::link;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

/** The same registries test_link.cpp sets up; whichever file runs first does it. */
void setup()
{
    if (ops::find("unrealPlaceInstances"))
        return;
    static bool done = [] {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        link::registerCommands();
        return true;
    }();
    (void)done;
}

/** A bolt and two instances, one moved and one turned, plus a plain cube with no instances. */
void buildKit()
{
    run("newScene");
    run("polyCube", {{"name", "Bolt"}});
    run("instance", {{"args", {"Bolt"}}, {"name", "BoltA"}});
    run("instance", {{"args", {"Bolt"}}, {"name", "BoltB"}});
    run("move", {{"args", {"BoltA"}}, {"by", {100, 0, 0}}});
    run("rotate", {{"args", {"BoltB"}}, {"by", {0, 90, 0}}});
    run("polyCube", {{"name", "Crate"}});
}

const json* placementOf(const json& group, const std::string& name)
{
    for (const json& p : group["placements"])
        if (p["name"] == name)
            return &p;
    return nullptr;
}
} // namespace

TEST_CASE("placement plan: one asset per instance root, every member placed in Unreal space")
{
    setup();
    buildKit();
    scene::Graph& g = scene::Graph::current();
    // Selecting one instance brings its whole group.
    json plan = placementPlan(g, {g.find("BoltA")->id(), g.find("Crate")->id()}, PlacementOptions{});
    REQUIRE(plan["groups"].size() == 2);
    const json& bolt = plan["groups"][0];
    CHECK(bolt["source"] == "|Bolt");
    CHECK(bolt["contentPath"] == "/Game/Lila/Instances/SM_Bolt");
    CHECK(bolt["objectPath"] == "/Game/Lila/Instances/SM_Bolt.SM_Bolt");
    CHECK(bolt["mode"] == "actors");
    REQUIRE(bolt["placements"].size() == 3);
    const json* a = placementOf(bolt, "BoltA");
    REQUIRE(a);
    // A dry run gives no link ids, so members go by node id.
    CHECK((*a)["id"] == std::to_string(g.find("BoltA")->id()));
    const math::Vector loc = math::toUnreal(math::Vector(100.0, 0.0, 0.0));
    CHECK((*a)["location"][0].get<double>() == doctest::Approx(loc.x));
    CHECK((*a)["location"][1].get<double>() == doctest::Approx(loc.y));
    CHECK((*a)["location"][2].get<double>() == doctest::Approx(loc.z));
    CHECK((*a)["rotation"][3].get<double>() == doctest::Approx(1.0));
    // A quarter turn about up.
    const json* b = placementOf(bolt, "BoltB");
    REQUIRE(b);
    CHECK(std::abs((*b)["rotation"][3].get<double>()) == doctest::Approx(std::sqrt(0.5)));
    CHECK(plan["groups"][1]["placements"].size() == 1);

    // Above the threshold auto goes to a hierarchical instanced component; an explicit mode wins.
    PlacementOptions o;
    o.threshold = 2;
    o.folder = "/Game/Kit/";
    json hism = placementPlan(g, {g.find("Bolt")->id()}, o);
    CHECK(hism["groups"][0]["mode"] == "hism");
    CHECK(hism["groups"][0]["contentPath"] == "/Game/Kit/SM_Bolt");
    o.mode = "ism";
    CHECK(placementPlan(g, {g.find("Bolt")->id()}, o)["groups"][0]["mode"] == "ism");
    o.mode = "bad";
    CHECK_THROWS_AS(placementPlan(g, {g.find("Bolt")->id()}, o), CommandError);

    CHECK(run("unrealPlaceInstances", {{"args", {"Bolt"}}, {"dryRun", true}})["groups"].size() == 1);
}

TEST_CASE("unrealPlaceInstances exports each source once, imports it and places its members")
{
    setup();
    buildKit();
    fs::path dir = fs::temp_directory_path() / "lila_link_placements";
    fs::remove_all(dir);
    PrefStore::get().set("unreal", "/cacheDir", dir.generic_string());

    std::mutex mx;
    std::vector<std::vector<scene::NodeId>> exported;
    registerExporter("fbx:instanceSource", [&](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        std::scoped_lock lock(mx);
        exported.push_back(nodes);
        return true;
    });
    FakeServer server([](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements"}}});
        if (m == "importAsset")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"].get<std::string>() + ".Imported"}},
                                           {"warnings", json::array()},
                                           {"errors", json::array()}});
        if (m == "placeInstances")
            return FakeServer::reply(req, {{"spawned", req["params"]["placements"].size()}, {"updated", 0}, {"removed", 0}});
        if (m == "removePlacements")
            return FakeServer::reply(req, {{"removed", 3}});
        return std::nullopt;
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    scene::Graph& g = scene::Graph::current();
    json done = run("unrealPlaceInstances", {{"args", {"BoltB"}}, {"wait", true}, {"folder", "/Game/Kit"}});
    REQUIRE(done["groups"].size() == 1);
    CHECK(done["groups"][0]["placed"]["spawned"] == 3);
    {
        std::scoped_lock lock(mx);
        REQUIRE(exported.size() == 1);
        CHECK(exported[0] == std::vector<scene::NodeId>{g.find("Bolt")->id()});
    }
    json import, place;
    for (const json& r : server.requests())
    {
        if (r["method"] == "importAsset")
            import = r["params"];
        if (r["method"] == "placeInstances")
            place = r["params"];
    }
    REQUIRE(import.is_object());
    CHECK(import["contentPath"] == "/Game/Kit/SM_Bolt");
    CHECK(import["kind"] == "static");
    REQUIRE(place.is_object());
    const std::string key = std::get<std::string>(g.find("Bolt")->value(scene::Transform::kLinkId));
    REQUIRE(key.size() == 16);
    CHECK(place["group"] == key);
    CHECK(place["label"] == "|Bolt");
    // The placements reference the asset the import reported.
    CHECK(place["mesh"] == "/Game/Kit/SM_Bolt.Imported");
    CHECK(place["mode"] == "actors");
    CHECK(place["placements"].size() == 3);

    CHECK(run("unrealRemovePlacements", {{"args", {"Bolt"}}})["removed"] == 3);
    json removal;
    for (const json& r : server.requests())
        if (r["method"] == "removePlacements")
            removal = r["params"];
    CHECK(removal["groups"] == json::array({key}));

    unregisterExporter("fbx:instanceSource");
    Link::get().disconnect();
    fs::remove_all(dir);
}

TEST_CASE("placement plan: a live array's copies are placements of their source's asset")
{
    setup();
    run("newScene");
    run("polyCube", {{"name", "Post"}});
    run("array", {{"args", {"Post"}}, {"count", 4}, {"offset", {200, 0, 0}}, {"name", "Fence"}});
    scene::Graph& g = scene::Graph::current();
    json plan = placementPlan(g, {g.find("Fence")->id()}, PlacementOptions{});
    REQUIRE(plan["groups"].size() == 1);
    const json& post = plan["groups"][0];
    CHECK(post["source"] == "|Fence|Post");
    REQUIRE(post["placements"].size() == 4);
    const json* last = placementOf(post, "Post_3");
    REQUIRE(last);
    const std::string key = std::to_string(g.find("Post")->id()) + ":3";
    CHECK((*last)["id"] == key);
    const math::Vector loc = math::toUnreal(math::Vector(600.0, 0.0, 0.0));
    CHECK((*last)["location"][0].get<double>() == doctest::Approx(loc.x));
    CHECK((*last)["location"][1].get<double>() == doctest::Approx(loc.y));

    // The copies count toward the threshold.
    PlacementOptions o;
    o.threshold = 3;
    CHECK(placementPlan(g, {g.find("Fence")->id()}, o)["groups"][0]["mode"] == "hism");
}

TEST_CASE("placement keys: link ids survive renames, and copies get their own")
{
    setup();
    buildKit();
    scene::Graph& g = scene::Graph::current();
    PlacementOptions o;
    o.assignIds = true;
    json first = placementPlan(g, {g.find("Bolt")->id()}, o);
    const std::string key = first["groups"][0]["key"];
    const std::string boltA = std::get<std::string>(g.find("BoltA")->value(scene::Transform::kLinkId));
    REQUIRE(key.size() == 16);
    REQUIRE(boltA.size() == 16);
    CHECK(boltA != key);

    run("rename", {{"args", {"Bolt", "Rivet"}}});
    json renamed = placementPlan(g, {g.find("Rivet")->id()}, o);
    CHECK(renamed["groups"][0]["key"] == key);
    CHECK(renamed["groups"][0]["source"] == "|Rivet");

    // Duplicates start without one; a copy that arrives carrying one (pasted, imported twice) gets a new one.
    run("duplicate", {{"args", {"BoltA"}}, {"name", "BoltC"}});
    CHECK(std::get<std::string>(g.find("BoltC")->value(scene::Transform::kLinkId)).empty());
    g.setValue({g.find("BoltC")->id(), scene::Transform::kLinkId, -1}, boltA);
    placementPlan(g, {g.find("BoltC")->id()}, o);
    CHECK(std::get<std::string>(g.find("BoltA")->value(scene::Transform::kLinkId)) == boltA);
    const std::string boltC = std::get<std::string>(g.find("BoltC")->value(scene::Transform::kLinkId));
    CHECK(boltC.size() == 16);
    CHECK(boltC != boltA);
}
