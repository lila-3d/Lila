// Stagehand at level scale: terrain colliders, convex parts, reach, settle caps, instances to objects, the starter
// library and what goes to Unreal.

#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

void load()
{
    if (!ops::registry().find("settle"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_STAGEHAND_PACKAGE) == "Stagehand");
}

void loadTerrain()
{
    load();
    if (!ops::registry().find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}

std::string cube(const char* name, const glm::dvec3& at, const glm::dvec3& size)
{
    return run("polyCube", {{"name", name}, {"position", json::array({at.x, at.y, at.z})}, {"width", size.x}, {"height", size.y}, {"depth", size.z},
                            {"baseAtOrigin", true}, {"generator", false}})[0]
        .get<std::string>();
}

glm::dvec3 worldPos(const std::string& node)
{
    const json t = run("xform", {{"args", {node}}, {"query", true}, {"worldSpace", true}, {"translation", true}});
    return {t[0].get<double>(), t[1].get<double>(), t[2].get<double>()};
}

void moveTo(const std::string& node, const glm::dvec3& at)
{
    run("xform", {{"args", {node}}, {"worldSpace", true}, {"translation", json::array({at.x, at.y, at.z})}});
}

/** An arch 200 wide, 240 tall and 60 deep from three closed boxes: two legs 40 wide and a lintel, its opening 120 wide and 200 high. */
std::string arch(const char* name)
{
    json points = json::array(), faces = json::array();
    auto box = [&](glm::dvec3 lo, glm::dvec3 hi) {
        const int base = int(points.size());
        for (int k = 0; k < 8; ++k)
            points.push_back({k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
        for (auto q : {std::array{0, 2, 3, 1}, std::array{4, 5, 7, 6}, std::array{0, 1, 5, 4}, std::array{2, 6, 7, 3}, std::array{0, 4, 6, 2},
                       std::array{1, 3, 7, 5}})
            faces.push_back({base + q[0], base + q[1], base + q[2], base + q[3]});
    };
    box({-100, 0, -30}, {-60, 200, 30});
    box({60, 0, -30}, {100, 200, 30});
    box({-100, 200, -30}, {100, 240, 30});
    return run("createMesh", {{"points", points}, {"faces", faces}, {"name", name}}).get<std::string>();
}
} // namespace

TEST_CASE("stagehand: an arch collides as convex parts and lands around a post instead of on it")
{
    load();
    run("newScene");
    const std::string post = cube("Post", {0, 0, 0}, {40, 150, 40});
    const std::string a = arch("Arch");
    moveTo(a, {0, 40, 0});
    run("physicsBody", {{"args", {a}}, {"material", "stone"}, {"role", "dynamic"}});
    const json info = run("dressInfo", {{"args", {a}}});
    CHECK(info[0]["shape"] == "compound");
    CHECK(info[0]["parts"].get<int64_t>() >= 3);
    const json r = run("settle", {{"args", {a}}});
    CHECK(r["atRest"].get<bool>());
    CHECK(worldPos(a).y < 5.0);
    CHECK(worldPos(post).y == doctest::Approx(0.0));

    // As one hull it would sit on the post.
    run("undo");
    run("physicsBody", {{"args", {a}}, {"shape", "hull"}});
    moveTo(a, {0, 160, 0});
    run("settle", {{"args", {a}}});
    CHECK(worldPos(a).y > 140.0);
}

TEST_CASE("stagehand: settle kilometres out takes only the colliders near each mover, and reports a cap")
{
    load();
    run("newScene");
    const glm::dvec3 west(-250000, 0, 120000), east(250000, 0, 120000);
    const std::string t1 = cube("TableWest", west, {120, 75, 80});
    const std::string t2 = cube("TableEast", east, {120, 75, 80});
    cube("Between", {0, 0, 120000}, {500, 500, 500});
    const std::string c1 = cube("CrateWest", west + glm::dvec3(0, 140, 0), {30, 30, 30});
    const std::string c2 = cube("CrateEast", east + glm::dvec3(10, 160, 0), {30, 30, 30});
    const json r = run("settle", {{"args", {c1, c2}}});
    CHECK(r["atRest"].get<bool>());
    CHECK_FALSE(r["capped"].get<bool>());
    CHECK(r["colliders"].get<int64_t>() == 2);
    CHECK(worldPos(c1).y == doctest::Approx(75.0).epsilon(0.01));
    CHECK(worldPos(c2).y == doctest::Approx(75.0).epsilon(0.01));
    CHECK(std::abs(worldPos(c1).x - west.x) < 1.0);
    CHECK(r["deepestPenetration"].get<double>() < 0.5);
    CHECK(r["movers"][0]["restsOn"][0] == t1);
    CHECK(r["movers"][1]["restsOn"][0] == t2);

    const std::string high = cube("High", west + glm::dvec3(300, 20000, 0), {30, 30, 30});
    const json capped = run("settle", {{"args", {high}}, {"time", 0.25}});
    CHECK(capped["capped"].get<bool>());
    CHECK_FALSE(capped["atRest"].get<bool>());
}

TEST_CASE("stagehand: a terrain is a heightfield collider and things settle onto it far from the origin")
{
    loadTerrain();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({400000, 400000})}, {"resolution", 257},
                                         {"center", json::array({300000, 1500, -200000})}, {"name", "Plain"}});
    const std::string terrain = t["mesh"].get<std::string>();
    run("physicsBody", {{"args", {terrain}}, {"role", "static"}});
    CHECK(run("dressInfo", {{"args", {terrain}}})[0]["shape"] == "heightfield");
    const std::string stone = cube("Stone", {310000, 2500, -190000}, {200, 120, 150});
    run("physicsBody", {{"args", {stone}}, {"material", "stone"}});
    const json r = run("settle", {{"args", {stone}}});
    CHECK(r["atRest"].get<bool>());
    CHECK(worldPos(stone).y == doctest::Approx(1500.0).epsilon(0.001));
    CHECK(r["movers"][0]["restsOn"][0] == terrain);
}
