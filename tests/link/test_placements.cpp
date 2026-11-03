#include <doctest/doctest.h>

#include "FakeServer.h"

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "io/SceneFile.h"
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
    if (!ops::find("unrealPlaceInstances"))
    {
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
    if (!ops::find("saveScene"))
        io::registerCommands();
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

TEST_CASE("placement plan: lights become light actors and lighting channels Unreal's three")
{
    setup();
    run("newScene");
    run("polyCube", {{"name", "Crate"}});
    run("polyCube", {{"name", "Floor"}});
    run("light", {{"type", "spot"}, {"name", "Lamp"}, {"position", {0, 200, 0}}, {"skipSelect", true}});
    run("light", {{"type", "dome"}, {"name", "Sky"}, {"skipSelect", true}});
    run("lightingChannels", {{"args", {"Lamp", "Crate"}}, {"set", {"interior"}}});
    scene::Graph& g = scene::Graph::current();
    const std::vector<scene::NodeId> roots = {g.find("Crate")->id(), g.find("Floor")->id(), g.find("Lamp")->id(), g.find("Sky")->id()};

    json plan = placementPlan(g, roots, PlacementOptions{});
    REQUIRE(plan["lights"].size() == 1);
    const json& lamp = plan["lights"][0];
    CHECK(lamp["kind"] == "spot");
    CHECK(lamp["lightingChannels"] == json::array({false, true, false}));
    // Unturned, a spot shines down Lila's -Z; forward is that direction in Unreal space.
    const math::Vector down = math::toUnreal(math::Vector(0.0, 0.0, -1.0));
    CHECK(lamp["forward"][0].get<double>() == doctest::Approx(down.x));
    CHECK(lamp["forward"][1].get<double>() == doctest::Approx(down.y));
    CHECK(lamp["forward"][2].get<double>() == doctest::Approx(down.z));
    const math::Vector at = math::toUnreal(math::Vector(0.0, 200.0, 0.0));
    CHECK(lamp["location"][0].get<double>() == doctest::Approx(at.x));
    CHECK(lamp["location"][2].get<double>() == doctest::Approx(at.z));
    CHECK(plan["skipped"].size() == 1);

    REQUIRE(plan["groups"].size() == 2);
    CHECK(plan["groups"][0]["placements"][0]["lightingChannels"] == json::array({false, true, false}));
    CHECK(plan["groups"][1]["placements"][0]["lightingChannels"] == json::array({true, false, false}));
    const json& report = plan["lightingChannels"];
    CHECK(report["channels"] == json::array({"interior"}));
    CHECK(report["lights"] == json::array({"Lamp"}));
    CHECK(report["objects"] == json::array({"Crate"}));
    CHECK(report["unreal"] == json::array({"default", "interior", ""}));
    CHECK(report["written"] == true);
    CHECK_FALSE(report.contains("note"));

    // More than three: the fourth and later are dropped and named, and what is left in none goes on channel 0.
    for (const char* c : {"a", "b", "c"})
        run("lightingChannel", {{"args", {c}}, {"create", true}});
    run("lightingChannels", {{"args", {"Floor"}}, {"set", {"a", "b"}}});
    run("lightingChannels", {{"args", {"Lamp"}}, {"set", {"c"}}});
    plan = placementPlan(g, roots, PlacementOptions{});
    CHECK(plan["lightingChannels"]["unreal"] == json::array({"default", "interior", "a"}));
    CHECK(plan["lightingChannels"]["dropped"] == json::array({"b", "c"}));
    CHECK(plan["lightingChannels"]["note"].get<std::string>().find("b, c dropped") != std::string::npos);
    CHECK(plan["lights"][0]["lightingChannels"] == json::array({true, false, false}));

    PlacementOptions off;
    off.lightingChannels = false;
    plan = placementPlan(g, roots, off);
    CHECK_FALSE(plan["groups"][0]["placements"][0].contains("lightingChannels"));
    CHECK_FALSE(plan["lights"][0].contains("lightingChannels"));
    CHECK(plan["lightingChannels"]["written"] == false);
    CHECK(plan["lightingChannels"]["objects"].size() == 2);
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
    // Static level meshes collide by default, without Nanite unless asked.
    CHECK(import["options"]["collision"] == "complexAsSimple");
    CHECK(import["options"]["nanite"] == false);
    REQUIRE(place.is_object());
    const std::string key = std::get<std::string>(g.find("Bolt")->value(scene::Transform::kLinkId));
    REQUIRE(key.size() == 16);
    CHECK(place["group"] == key);
    CHECK(place["label"] == "|Bolt");
    // The placements reference the asset the import reported.
    CHECK(place["mesh"] == "/Game/Kit/SM_Bolt.Imported");
    CHECK(place["mode"] == "actors");
    CHECK(place["collision"] == "complexAsSimple");
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

TEST_CASE("placement assets: names unique across same-named sources, saved and kept after reopening")
{
    setup();
    run("newScene");
    run("group", {{"empty", true}, {"name", "North"}});
    run("group", {{"empty", true}, {"name", "South"}});
    run("polyCube", {{"name", "Wall"}});
    run("parent", {{"args", {"Wall", "North"}}});
    run("polyCube", {{"name", "Gate"}});
    scene::Graph& g = scene::Graph::current();
    PlacementOptions o;
    o.assignIds = true;
    json first = placementPlan(g, {g.find("Wall")->id(), g.find("Gate")->id()}, o);
    REQUIRE(first["groups"].size() == 2);
    CHECK(first["groups"][0]["contentPath"] == "/Game/Lila/Instances/SM_Wall");
    CHECK(first["groups"][1]["contentPath"] == "/Game/Lila/Instances/SM_Gate");

    // A second Wall elsewhere gets its own asset; the first keeps its name.
    run("polyCube", {{"name", "Wall"}});
    run("parent", {{"args", {"|Wall", "South"}}});
    const scene::NodeId south = g.find("|South|Wall")->id();
    json both = placementPlan(g, {g.find("|North|Wall")->id(), south}, o);
    REQUIRE(both["groups"].size() == 2);
    CHECK(both["groups"][0]["contentPath"] == "/Game/Lila/Instances/SM_Wall");
    const std::string key = both["groups"][1]["key"];
    const std::string second = both["groups"][1]["contentPath"];
    CHECK(second == "/Game/Lila/Instances/SM_Wall_" + key.substr(0, 8));
    CHECK(both["groups"][1]["objectPath"] == second + "." + second.substr(second.rfind('/') + 1));

    const fs::path path = fs::temp_directory_path() / "lila_link_asset_names.lila";
    run("saveScene", {{"args", {path.generic_string()}}});
    run("newScene");
    run("openScene", {{"args", {path.generic_string()}}});
    json again = placementPlan(g, {g.find("|North|Wall")->id(), g.find("|South|Wall")->id()}, o);
    CHECK(again["groups"][0]["contentPath"] == "/Game/Lila/Instances/SM_Wall");
    CHECK(again["groups"][1]["contentPath"] == second);
    // Renaming the source keeps the asset it was sent as.
    run("rename", {{"args", {"|South|Wall", "Rampart"}}});
    CHECK(placementPlan(g, {g.find("Rampart")->id()}, o)["groups"][0]["contentPath"] == second);
    fs::remove(path);
}

TEST_CASE("placement collision: options checked, UCX_ proxies go with their source and are never placed")
{
    setup();
    run("newScene");
    run("polyCube", {{"name", "Arch"}});
    run("polyCube", {{"name", "UCX_Arch_00"}});
    run("parent", {{"args", {"UCX_Arch_00", "Arch"}}});
    scene::Graph& g = scene::Graph::current();
    PlacementOptions o;
    json plan = placementPlan(g, {g.find("Arch")->id()}, o);
    REQUIRE(plan["groups"].size() == 1);
    CHECK(plan["groups"][0]["source"] == "|Arch");
    CHECK(plan["groups"][0]["collision"] == "complexAsSimple");
    CHECK(plan["groups"][0]["nanite"] == false);
    o.collision = "ucx";
    o.nanite = true;
    plan = placementPlan(g, {g.find("Arch")->id()}, o);
    CHECK(plan["groups"][0]["collision"] == "ucx");
    CHECK(plan["groups"][0]["nanite"] == true);
    o.collision = "sphere";
    CHECK_THROWS_AS(placementPlan(g, {g.find("Arch")->id()}, o), CommandError);
    CHECK(run("unrealPlaceInstances", {{"args", {"Arch"}}, {"dryRun", true}, {"collision", "box"}})["groups"][0]["collision"] == "box");

    // unrealImport passes collision through as an import option.
    CHECK(importParams("C:/x.fbx", {{"contentPath", "/Game/A"}, {"collision", "convex"}})["options"]["collision"] == "convex");
}

TEST_CASE("verification commands: unrealCall, unrealListPlacements, unrealAssetInfo, unrealSaveAssets return errors as data")
{
    setup();
    buildKit();
    FakeServer server([](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements"}}});
        if (m == "ping")
            return FakeServer::reply(req, "pong");
        if (m == "listPlacements")
            return FakeServer::reply(req, json::array({{{"actor", "Bolt"}, {"group", "g1"}, {"node", "n1"}},
                                                       {{"actor", "Lila_Crate"}, {"group", "other"}, {"instances", 40}}}));
        if (m == "assetInfo")
            return FakeServer::reply(req, {{"path", req["params"]["path"]}, {"class", "StaticMesh"}, {"collision", {{"simple", 1}}}});
        if (m == "saveAssets")
            return FakeServer::reply(req, {{"saved", false}, {"errors", {"Saving the level failed"}}});
        return json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"error", {{"code", -32601}, {"message", "no method " + m}}}};
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    json r = run("unrealCall", {{"args", {"ping"}}});
    CHECK(r["ok"] == true);
    CHECK(r["result"] == "pong");
    r = run("unrealCall", {{"args", {"noSuchMethod"}}, {"params", {{"x", 1}}}});
    CHECK(r["ok"] == false);
    CHECK(r["code"] == -32601);
    CHECK(r["error"].get<std::string>().find("noSuchMethod") != std::string::npos);

    // Every placement, or only the groups of the given meshes (their keys sent along).
    r = run("unrealListPlacements");
    REQUIRE(r["ok"] == true);
    CHECK(r["placements"].size() == 2);
    r = run("unrealListPlacements", {{"args", {"Crate"}}});
    REQUIRE(r["ok"] == true);
    CHECK(r["placements"].empty());
    json listed;
    for (const json& q : server.requests())
        if (q["method"] == "listPlacements")
            listed = q["params"];
    CHECK(listed["groups"] == json::array({std::to_string(scene::Graph::current().find("Crate")->id())}));

    r = run("unrealAssetInfo", {{"args", {"/Game/Lila/Instances/SM_Bolt"}}});
    CHECK(r["ok"] == true);
    CHECK(r["collision"]["simple"] == 1);
    r = run("unrealSaveAssets", {{"level", true}});
    CHECK(r["ok"] == false);
    CHECK(r["errors"].size() == 1);
    json saved;
    for (const json& q : server.requests())
        if (q["method"] == "saveAssets")
            saved = q["params"];
    CHECK(saved["level"] == true);
    CHECK_THROWS_AS(run("unrealSaveAssets"), CommandError);
    Link::get().disconnect();
}

TEST_CASE("placement timeouts scale with the count; a km-wide grid of thousands plans as one hism group")
{
    setup();
    CHECK(placementTimeoutMs(0) == 30000);
    CHECK(placementTimeoutMs(10000) == 230000);
    CHECK(placementTimeoutMs(100000000) == 1800000);

    run("newScene");
    run("polyCube", {{"name", "Stone"}, {"width", 300.0}, {"height", 300.0}, {"depth", 300.0}});
    // 60 x 60 stones 100 m apart: 5.9 km across.
    run("array", {{"args", {"Stone"}}, {"mode", "grid"}, {"count", 60}, {"countY", 1}, {"countZ", 60}, {"spacing", {10000, 0, 10000}}, {"name", "Field"}});
    scene::Graph& g = scene::Graph::current();
    json plan = placementPlan(g, {g.find("Field")->id()}, PlacementOptions{});
    REQUIRE(plan["groups"].size() == 1);
    const json& grp = plan["groups"][0];
    CHECK(grp["mode"] == "hism");
    REQUIRE(grp["placements"].size() == 3600);
    double far = 0.0;
    for (const json& p : grp["placements"])
        far = std::max(far, std::hypot(p["location"][0].get<double>(), p["location"][1].get<double>()));
    CHECK(far == doctest::Approx(std::hypot(590000.0, 590000.0)));
}
