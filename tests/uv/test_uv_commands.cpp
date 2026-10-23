#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "uv/Chart.h"
#include "uv/Uv.h"
#include "uv/UvCommands.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace lila;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
} // namespace

TEST_CASE("uv commands on a cube")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("select", {{"args", {cube}}});

    json r = run("uvAuto");
    CHECK(r["ok"] == true);
    CHECK(r["islands"].get<int>() <= 6);
    json info = run("uvLayout");
    CHECK(info["overlappingFaces"] == 0);
    CHECK(info["foldedFaces"] == 0);
    CHECK(info["islands"].get<int>() == r["islands"].get<int>());
    run("undo");
    run("redo");

    run("uvProject", {{"type", "box"}});
    CHECK(run("uvLayout")["islands"] == 6);
    run("uvPack", {{"resolution", 512}, {"padding", 8}});
    CHECK(run("uvLayout")["overlappingFaces"] == 0);
    run("uvOrient");
    run("uvStraighten");

    double td = run("uvTexelDensity", {{"resolution", 1024}}).get<double>();
    CHECK(td > 0.0);
    run("uvTexelDensity", {{"resolution", 1024}, {"density", 2.0}});
    CHECK(run("uvTexelDensity", {{"resolution", 1024}}).get<double>() == doctest::Approx(2.0).epsilon(1e-3));

    json lm = run("uvLightmap", {{"resolution", 64}});
    CHECK(lm["ok"] == true);
    CHECK(run("uvLayout", {{"uvSet", "uv1"}})["overlappingFaces"] == 0);
    CHECK(run("uvSetCurrent") == "uv0");

    // Component edits: cut then sew one edge of a face, move one face's uvs.
    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    CHECK(run("uvCutEdges")["cut"] == 1);
    CHECK(run("uvSewEdges")["sewn"].get<int>() <= 1);
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    CHECK(run("uvTransform", {{"translate", {0.1, 0.0}}, {"rotate", 15.0}})["corners"].get<int>() >= 4);
    run("uvUnwrap");
    run("uvAutoSeams");
}

TEST_CASE("uvPack packs every selected mesh into one layout and stacks")
{
    run("newScene");
    std::string a = run("polyCube")[0], b = run("polyCube")[0];
    run("select", {{"args", {a, b}}});
    run("uvProject", {{"type", "box"}});
    json r = run("uvPack", {{"resolution", 512}, {"padding", 4}, {"effort", 2}});
    CHECK(r["ok"] == true);
    CHECK(r["density"].get<double>() > 30.0);
    CHECK(r["stacked"] == 0);
    CHECK(run("uvStacks")[a].empty());

    json s = run("uvPack", {{"stack", true}, {"rotateStep", 0}, {"angles", {0, 90}}, {"layout", "fill"}, {"keep", "none"}});
    CHECK(s["stacked"] == 11);
    json stacks = run("uvStacks", {{"select", true}});
    CHECK(stacks[a].size() == 1);
    CHECK(stacks[a][0].size() == 6);

    run("select", {{"args", {a, b}}});
    json g = run("uvPack", {{"layout", "groupTiles"}, {"groupBy", "object"}, {"scaleMode", "keep"}, {"region", {0.0, 0.0, 0.5, 1.0}}});
    CHECK(g["tiles"] == 2);
    run("undo");
    CHECK_THROWS(run("uvPack", {{"scaleMode", "lock"}, {"tilesU", 1}, {"padding", 600}}));
}

TEST_CASE("uvPack perObject gives each mesh a layout of its own")
{
    run("newScene");
    std::string a = run("polyCube")[0], b = run("polyCube")[0];
    run("select", {{"args", {a, b}}});
    run("uvProject", {{"type", "box"}});
    double shared = run("uvPack", {{"resolution", 512}, {"padding", 4}})["texelsPerUnit"].get<double>();
    json r = run("uvPack", {{"resolution", 512}, {"padding", 4}, {"perObject", true}});
    REQUIRE(r["meshes"].size() == 2);
    CHECK(r["meshes"][0]["mesh"] == a);
    for (const json& m : r["meshes"])
        CHECK(m["texelsPerUnit"].get<double>() > shared * 1.2);
    run("undo");
}

namespace
{
std::string made(const json& r) { return (r.is_array() ? r[0] : r).get<std::string>(); }
scene::NodeId idOf(const std::string& name) { return scene::Graph::current().find(name)->id(); }
std::vector<scene::NodeId> sorted(std::vector<scene::NodeId> v)
{
    std::sort(v.begin(), v.end());
    return v;
}
} // namespace

TEST_CASE("uv islands of seed faces")
{
    run("newScene");
    std::string cube = made(run("polyCube"));
    run("select", {{"args", {cube}}});
    scene::Graph& g = scene::Graph::current();
    auto mesh = [&] {
        g.evaluate();
        return g.find(cube)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    };
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;

    run("uvProject", {{"type", "box"}});
    const geo::Index seed[] = {0};
    CHECK(uv::islandsOfFaces(*mesh(), seed, co) == std::vector<geo::Index>{0});
    const geo::Index two[] = {0, 3};
    CHECK(uv::islandsOfFaces(*mesh(), two, co) == std::vector<geo::Index>{0, 3});
    CHECK(uv::islandsOfFaces(*mesh(), {}, co).empty());

    // One shared uv per point joins every face into a single island.
    run("uvProject", {{"type", "planar"}});
    CHECK(uv::islandsOfFaces(*mesh(), seed, co).size() == 6);
}

TEST_CASE("uv editor meshes follow the selection and its hierarchy")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube")), c = made(run("polyCube"));
    std::string grp = made(run("group", {{"args", {a, b}}}));
    scene::Graph& g = scene::Graph::current();

    run("select", {{"args", {grp}}});
    CHECK(sorted(uv::shownMeshes()) == sorted({idOf(a), idOf(b)}));
    const scene::NodeId roots[] = {idOf(grp), idOf(a)};
    CHECK(uv::meshesUnder(g, roots).size() == 2);

    // Whole-mesh uv tools reach the meshes under a selected group.
    json r = run("uvProject", {{"type", "box"}});
    CHECK(r.contains(a));
    CHECK(r.contains(b));

    // A component selection left on a mesh does not keep it shown once something else is selected.
    run("select", {{"args", {a}}});
    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {a}}, {"faces", {0, 1}}});
    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {c}}});
    CHECK(uv::shownMeshes() == std::vector<scene::NodeId>{idOf(c)});
    run("selectMode", {{"args", {"vertex"}}});
    CHECK(uv::shownMeshes() == std::vector<scene::NodeId>{idOf(c)});

    // With no objects selected, meshes with components of the current kind show.
    run("select", {{"clear", true}});
    run("selectComponents", {{"args", {b}}, {"vertices", {0}}});
    CHECK(uv::shownMeshes() == std::vector<scene::NodeId>{idOf(b)});
    run("selectMode", {{"args", {"object"}}});
    CHECK(uv::shownMeshes().empty());
}

TEST_CASE("uv editor meshes leave out children when includeChildren is off")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube"));
    run("parent", {{"args", {b, a}}});
    run("select", {{"args", {a}}});
    CHECK(uv::shownMeshes().size() == 2);
    uv::setIncludeChildren(false);
    CHECK(uv::shownMeshes() == std::vector<scene::NodeId>{idOf(a)});
    // One mesh projected: the result is its frame rather than results keyed by mesh.
    json r = run("uvProject", {{"type", "box"}});
    CHECK(r.contains("type"));
    CHECK(!r.contains(b));
    uv::setIncludeChildren(true);
}

TEST_CASE("uvTransform perIsland turns each island about its own centre")
{
    run("newScene");
    std::string cube = made(run("polyCube"));
    run("select", {{"args", {cube}}});
    run("uvProject", {{"type", "box"}});
    run("uvPack", {{"resolution", 512}, {"padding", 4}});
    auto centres = [&] {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        const geo::Mesh& m = *g.find(cube)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        uv::ChartOptions co;
        co.source = uv::IslandSource::Uvs;
        std::span<const glm::vec2> uvs = m.cornerAttrs().get<glm::vec2>("uv0");
        std::vector<glm::dvec2> out;
        for (const auto& island : uv::islandFaces(m, {}, co))
        {
            glm::dvec2 lo(1e30), hi(-1e30);
            for (geo::Index c : uv::uvCornersOfFaces(m, "uv0", island))
            {
                lo = glm::min(lo, glm::dvec2(uvs[c]));
                hi = glm::max(hi, glm::dvec2(uvs[c]));
            }
            out.push_back((lo + hi) * 0.5);
        }
        return out;
    };
    std::vector<glm::dvec2> before = centres();
    REQUIRE(before.size() == 6);
    CHECK(run("uvTransform", {{"rotate", 90.0}, {"perIsland", true}})["corners"].get<int>() > 0);
    run("uvTransform", {{"scale", {-1.0, 1.0}}, {"perIsland", true}});
    std::vector<glm::dvec2> after = centres();
    REQUIRE(after.size() == 6);
    for (size_t i = 0; i < 6; ++i)
        CHECK(glm::distance(before[i], after[i]) < 1e-4);
}
