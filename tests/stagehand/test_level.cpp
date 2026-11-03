// Stagehand at level scale: terrain colliders, convex parts, reach, settle caps, instances to objects, the starter
// library and what goes to Unreal.

#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
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

namespace
{
/** A cube broken by a Fracture geometry-nodes modifier into about `pieces` instanced pieces. */
std::string fracturedCube(const char* name, const glm::dvec3& at, int pieces)
{
    const std::string c = cube(name, at, {200, 200, 200});
    const std::string group = std::string(name) + "Break";
    run("gnBuildGroup", {{"args", {json{{"name", group},
                                        {"nodes", json::array({{{"name", "F"}, {"type", "Fracture"}, {"values", {{"pieces", pieces}, {"roughness", 0.0}}}}})},
                                        {"links", json::array({"GroupInput.geometry -> F.mesh", "F.pieces -> GroupOutput.geometry"})}}}}});
    run("gnApply", {{"args", {c, group}}});
    return c;
}

std::filesystem::path scratchFile(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "lila_stagehand_level";
    std::filesystem::create_directories(dir);
    return dir / name;
}
} // namespace

TEST_CASE("stagehand: fracture pieces become objects that settle, undo, and survive save and reopen")
{
    load();
    run("newScene");
    const glm::dvec3 site(1000000, 0, -400000);
    const std::string wall = fracturedCube("Wall", site, 12);
    const json r = run("instancesToObjects", {{"args", {wall}}, {"material", "stone"}});
    const size_t count = r["count"].get<size_t>();
    CHECK(count >= 6);
    CHECK(r["objects"].size() == count);
    CHECK(r["meshes"].get<size_t>() == count); // every piece is its own mesh
    CHECK(run("getAttr", {{"args", {wall + ".visibility"}}}).get<bool>() == false);
    // Each piece sits where its instance was: together they still fill the cube.
    glm::dvec3 lo(1e300), hi(-1e300);
    for (const json& o : r["objects"])
    {
        const json info = run("dressInfo", {{"args", {o}}})[0];
        CHECK(info["role"] == "dynamic");
        CHECK(info["material"] == "stone");
        for (int k = 0; k < 3; ++k)
        {
            lo[k] = std::min(lo[k], info["boundsMin"][k].get<double>());
            hi[k] = std::max(hi[k], info["boundsMax"][k].get<double>());
        }
    }
    CHECK(lo.x == doctest::Approx(site.x - 100.0).epsilon(1e-4));
    CHECK(hi.y == doctest::Approx(200.0).epsilon(0.01));

    // Lifted and settled, the pieces fall in a heap on the ground.
    run("xform", {{"args", {r["group"]}}, {"translation", json::array({0, 300, 0})}});
    const std::vector<std::string> objs = r["objects"].get<std::vector<std::string>>();
    const json s = run("settle", {{"args", objs}});
    CHECK(s["atRest"].get<bool>());
    CHECK(s["deepestPenetration"].get<double>() < 1.0);
    for (const json& m : s["movers"])
        CHECK(m["position"][1].get<double>() < 300.0);

    const auto path = scratchFile("fracture_objects.lila");
    const glm::dvec3 before = worldPos(objs[0]);
    run("saveScene", {{"args", {path.string()}}});
    run("openScene", {{"args", {path.string()}}});
    CHECK(glm::length(worldPos(objs[0]) - before) < 1e-6);
    CHECK(run("dressInfo", {{"args", {objs[0]}}})[0]["material"] == "stone");
    CHECK(run("getAttr", {{"args", {wall + ".visibility"}}}).get<bool>() == false);

    run("newScene");
    const std::string w2 = fracturedCube("Wall", site, 12);
    run("instancesToObjects", {{"args", {w2}}});
    run("undo");
    CHECK(run("getAttr", {{"args", {w2 + ".visibility"}}}).get<bool>() == true);
    CHECK_FALSE(run("objExists", {{"args", {"Wall_Objects"}}}).get<bool>());
}

TEST_CASE("stagehand: scattered copies of one mesh become scene instances of it")
{
    load();
    run("newScene");
    const std::string ground = cube("Lot", {0, 0, 0}, {1000, 10, 1000});
    const std::string rock = cube("Rock", {0, 0, 3000}, {20, 15, 25});
    run("gnBuildGroup",
        {{"args", {json{{"name", "Strew"},
                        {"nodes", json::array({{{"name", "Info"}, {"type", "ObjectInfo"}, {"props", {{"object", rock}, {"transformSpace", "original"}}}},
                                               {{"name", "Pts"}, {"type", "DistributePointsOnFaces"}, {"values", {{"density", 0.2}, {"seed", 3}}}},
                                               {{"name", "Inst"}, {"type", "InstanceOnPoints"}}})},
                        {"links", json::array({"GroupInput.geometry -> Pts.mesh", "Pts.points -> Inst.points", "Info.geometry -> Inst.instance",
                                               "Inst.instances -> GroupOutput.geometry"})}}}}});
    run("gnApply", {{"args", {ground, "Strew"}}});
    const json r = run("instancesToObjects", {{"args", {ground}}, {"name", "Strewn"}});
    CHECK(r["meshes"].get<int64_t>() == 1);
    CHECK(r["count"].get<int64_t>() >= 10);
    CHECK(run("selectInstances", {{"args", {r["objects"][0]}}}).size() == r["objects"].size());

    const json u = run("instancesToObjects", {{"args", {ground}}, {"mode", "unique"}, {"hide", false}});
    CHECK(u["count"] == r["count"]);
    CHECK(run("selectInstances", {{"args", {u["objects"][0]}}}).size() == 1);
}

TEST_CASE("library: the starter props are built on first use, found by words and tags, and placed with their physics")
{
    load();
    run("newScene");
    const json built = run("libraryStarter");
    const std::vector<std::string> ids = built["assets"].get<std::vector<std::string>>();
    CHECK(ids.size() >= 30);
    for (const char* id : {"Industrial/Crate", "Industrial/Barrel", "Industrial/Pallet", "Industrial/Beam", "Industrial/IBeam", "Industrial/PipeSegment",
                           "Industrial/Panel", "Industrial/CableSpool", "Industrial/ScaffoldFrame", "Debris/StoneChunkS1", "Debris/StoneChunkXL3",
                           "Debris/RubblePile", "Debris/BrokenSlab", "Debris/Rebar", "Facility/Locker", "Facility/Console", "Facility/SupplyCrate",
                           "Facility/CableTray"})
        CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    CHECK(std::filesystem::exists(std::filesystem::path(built["dir"].get<std::string>()) / "Industrial" / "Barrel" / "asset.json"));
    // Built once: asking again finds them current.
    CHECK_FALSE(run("libraryStarter")["built"].get<bool>());

    const json rebar = run("librarySearch", {{"args", {"rebar"}}, {"tier", "starter"}});
    REQUIRE(rebar.size() >= 2);
    CHECK(rebar[0]["collection"] == "Debris");
    // Small debris by tag and size: only the S and M stone chunks.
    const json small = run("librarySearch", {{"tags", json::array({"debris", "stone"})}, {"maxSize", 50.0}, {"tier", "starter"}});
    CHECK(small.size() == 6);
    for (const json& a : small)
        CHECK(a["physics"]["material"] == "stone");
    const json hangar = run("librarySearch", {{"args", {"starfighter"}}, {"tier", "starter"}});
    CHECK(hangar.size() >= 2);

    const std::string floor = cube("Floor", {0, 0, 0}, {2000, 10, 2000});
    const json crates = run("place", {{"args", {"Industrial/Crate"}}, {"on", floor}, {"count", 3}, {"spacing", json::array({100, 0, 0})}});
    REQUIRE(crates.size() == 3);
    const std::string crateMesh = crates[0]["meshes"][0].get<std::string>();
    const json info = run("dressInfo", {{"args", {crateMesh}}})[0];
    CHECK(info["role"] == "dynamic");
    CHECK(info["material"] == "wood");
    CHECK(info["shape"] == "box");
    CHECK(info["boundsMax"][1].get<double>() - info["boundsMin"][1].get<double>() == doctest::Approx(80.0).epsilon(0.01));
    CHECK(info["boundsMin"][1].get<double>() == doctest::Approx(10.0).epsilon(0.01));

    // An I-beam and a scaffold frame collide as their parts.
    const json beam = run("place", {{"args", {"Industrial/IBeam"}}, {"at", json::array({0, 300, 400})}, {"snap", false}});
    const json beamInfo = run("dressInfo", {{"args", {beam[0]["meshes"][0]}}})[0];
    CHECK(beamInfo["shape"] == "compound");
    CHECK(beamInfo["parts"].get<int64_t>() >= 3);
    CHECK(beamInfo["material"] == "metal");
    const json r = run("settle", {{"args", {beam[0]["meshes"][0]}}});
    CHECK(r["atRest"].get<bool>());
    CHECK(r["movers"][0]["restsOn"][0] == floor);

    const auto path = std::filesystem::temp_directory_path() / "lila_stagehand_level" / "starter_props.lila";
    std::filesystem::create_directories(path.parent_path());
    run("saveScene", {{"args", {path.string()}}});
    run("openScene", {{"args", {path.string()}}});
    CHECK(run("dressInfo", {{"args", {crateMesh}}})[0]["material"] == "wood");
    CHECK(run("dressInfo", {{"args", {beam[0]["meshes"][0]}}})[0]["shape"] == "compound");
}
