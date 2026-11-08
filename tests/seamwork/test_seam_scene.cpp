#include "TestUtil.h"
#include "extensions/ExtensionManager.h"
#include "geo/ColorSets.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>

using namespace lila;
using namespace testutil;
using json = nlohmann::json;

namespace
{
void load()
{
    if (!ops::registry().find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
    if (!ops::registry().find("settle"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_STAGEHAND_PACKAGE) == "Stagehand");
    if (!ops::registry().find("seamFind"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SEAMWORK_PACKAGE) == "Seamwork");
}

/** A closed box from lo to hi, faces subdivided n times along each side so fields have points to land on. */
std::string box(const char* name, glm::dvec3 lo, glm::dvec3 hi, int n = 1)
{
    json points = json::array(), faces = json::array();
    std::map<std::array<int, 3>, int> ids;
    auto id = [&](int i, int j, int k) {
        auto [it, fresh] = ids.try_emplace({i, j, k}, int(points.size()));
        if (fresh)
            points.push_back({lo.x + (hi.x - lo.x) * i / n, lo.y + (hi.y - lo.y) * j / n, lo.z + (hi.z - lo.z) * k / n});
        return it->second;
    };
    for (int a = 0; a < n; ++a)
        for (int b = 0; b < n; ++b)
        {
            faces.push_back({id(0, a, b), id(0, a, b + 1), id(0, a + 1, b + 1), id(0, a + 1, b)});
            faces.push_back({id(n, a, b), id(n, a + 1, b), id(n, a + 1, b + 1), id(n, a, b + 1)});
            faces.push_back({id(a, 0, b), id(a + 1, 0, b), id(a + 1, 0, b + 1), id(a, 0, b + 1)});
            faces.push_back({id(a, n, b), id(a, n, b + 1), id(a + 1, n, b + 1), id(a + 1, n, b)});
            faces.push_back({id(a, b, 0), id(a, b + 1, 0), id(a + 1, b + 1, 0), id(a + 1, b, 0)});
            faces.push_back({id(a, b, n), id(a + 1, b, n), id(a + 1, b + 1, n), id(a, b + 1, n)});
        }
    return run("createMesh", {{"points", points}, {"faces", faces}, {"name", name}}).get<std::string>();
}

void moveTo(const std::string& node, const glm::dvec3& at)
{
    run("xform", {{"args", {node}}, {"worldSpace", true}, {"translation", json::array({at.x, at.y, at.z})}});
}

const geo::Mesh& evaluatedMesh(const std::string& node)
{
    g().evaluate();
    return *g().find(node)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}
} // namespace

TEST_CASE("seamFind: a wall on a terrain gives a live join, a field and a terrain mask")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 81}, {"name", "Ground"}});
    const std::string terrain = t["mesh"].get<std::string>();
    const std::string wall = box("Wall", {-200, -20, -10}, {200, 280, 10}, 4);
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", terrain}});
    CHECK(r["curves"].get<int>() == 1);
    CHECK(r["length"].get<double>() == doctest::Approx(840.0).epsilon(0.001));
    CHECK(r["terrain"] == "heightfield");
    REQUIRE(r["pairs"].size() == 1);
    CHECK(r["pairs"][0]["a"] == wall);
    CHECK(r["pairs"][0]["b"] == terrain);
    CHECK(r["fieldModifiers"].size() == 1);
    CHECK_FALSE(r["terrainMaskNode"].is_null());

    // The field: red is 1 at the footprint and 0 past the band.
    const geo::Mesh& m = evaluatedMesh(wall);
    const auto sets = geo::colors::sets(m);
    REQUIRE_FALSE(sets.empty());
    const auto colors = geo::colors::values(m, sets.front());
    float lo = 1.0f, hi = 0.0f;
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c))
        {
            lo = std::min(lo, colors[c].r);
            hi = std::max(hi, colors[c].r);
        }
    CHECK(lo == 0.0f);
    CHECK(hi > 0.0f);

    const json info = run("terrainInfo", {{"args", {terrain}}});
    CHECK(info["masks"].contains("contact"));

    const json si = run("seamInfo", {{"args", {r["seam"]}}, {"points", true}});
    REQUIRE(si["curveList"].size() == 1);
    CHECK(si["curveList"][0]["kind"] == "ground");
    CHECK(si["curveList"][0]["angle"][0].get<double>() == doctest::Approx(90.0).epsilon(0.01));

    // Live: move the wall and the join follows.
    moveTo(wall, {0, 100, 0});
    g().evaluate();
    run("seamUpdate");
    const json moved = run("seamInfo", {{"args", {r["seam"]}}});
    CHECK(moved["curves"].get<int>() == 0);
    moveTo(wall, {0, -100, 0});
    const json back = run("seamInfo", {{"args", {r["seam"]}}});
    CHECK(back["length"].get<double>() == doctest::Approx(840.0).epsilon(0.001));
}

TEST_CASE("seamFind: tags leave objects out and seamCheck reports floating and open joins")
{
    load();
    run("newScene");
    const std::string a = box("BlockA", {0, 0, 0}, {100, 100, 100});
    const std::string b = box("BlockB", {50, 25, 25}, {150, 75, 75});
    const std::string floater = box("Floater", {500, 30, 0}, {550, 80, 50});
    const std::string floor = box("Floor", {-1000, -20, -1000}, {1000, 0, 1000});
    run("seamTag", {{"args", {floor}}, {"ground", true}});
    json r = run("seamFind", {{"args", {a, b, floater, floor}}, {"field", "none"}, {"terrainMask", false}});
    // A-B crossing, A on the floor (near contact at 0 gap is a touch, not counted), B above it.
    bool meshPair = false;
    for (const json& p : r["pairs"])
        meshPair |= p["kind"] == "mesh" && p["a"] == a && p["b"] == b;
    CHECK(meshPair);
    const json check = run("seamCheck", {{"args", {r["seam"]}}});
    bool floating = false, open = false;
    for (const json& i : check["issues"])
    {
        floating |= i["kind"] == "floating" && i["node"] == floater && std::abs(i["gap"].get<double>() - 30.0) < 0.01;
        open |= i["kind"] == "open";
    }
    CHECK(floating);
    CHECK(open);

    run("seamTag", {{"args", {b}}, {"ignore", true}});
    r = run("seamFind", {{"args", {a, b}}, {"field", "none"}, {"name", "second"}});
    CHECK(r["ignored"].size() == 1);
    CHECK(r["curves"].get<int>() == 0);
}

TEST_CASE("seamScatter: rubble along a wall settles clean")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 81}, {"name", "Ground"}});
    const std::string wall = box("Wall", {-200, -20, -10}, {200, 280, 10});
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", t["mesh"]}, {"field", "none"}});
    const json s = run("seamScatter", {{"args", {r["seam"]}}, {"preset", "rubble"}, {"density", 1.0}, {"clump", 2}, {"settle", true}});
    CHECK(s["instances"].get<int>() > 10);
    CHECK(s["farthestFromContact"].get<double>() < 40.0 + 15.0 + 10.0);
    CHECK(s["dressCheck"]["ok"].get<bool>());
    // The scatter covers the contact now.
    const json c = run("seamCheck", {{"args", {r["seam"]}}});
    CHECK(c["ok"].get<bool>());
    // Rules on it: nothing left inside a box over half the wall.
    const std::string zone = box("Zone", {-300, -100, -200}, {0, 100, 200});
    const json p = run("scatterProximity", {{"args", {s["group"]}}, {"rules", {{{"kind", "cullInside"}, {"target", zone}}}}});
    CHECK(p["removed"].size() > 0);
    CHECK(p["kept"].get<int>() > 0);
}
