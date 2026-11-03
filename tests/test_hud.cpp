#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/HudStats.h"
#include "scene/Isolation.h"
#include "scene/LodGroup.h"
#include "scene/MeshNodes.h"

#include <chrono>
#include <memory>
#include <thread>

using namespace lila;
using namespace lila::scene;
using hud::PolyCount;
using namespace testutil;

namespace
{
/** Two quads sharing an edge, uv0 laid out side by side; seam splits the shared edge's uvs. */
std::shared_ptr<geo::Mesh> sharedTwoQuads(bool seam)
{
    auto m = std::make_shared<geo::Mesh>();
    for (int i = 0; i < 3; ++i)
    {
        m->addPoint({double(i), 0.0, 0.0});
        m->addPoint({double(i), 1.0, 0.0});
    }
    geo::Index a[] = {0, 2, 3, 1}, b[] = {2, 4, 5, 3};
    geo::Index fa = m->addFace(a), fb = m->addFace(b);
    m->cornerAttrs().add("uv0", geo::AttrType::Float2);
    std::span<glm::vec2> uv = m->cornerAttrs().get<glm::vec2>("uv0");
    for (geo::Index f : {fa, fb})
        for (geo::Index c : m->faceCorners(f))
        {
            glm::dvec3 p = m->position(m->cornerPoint(c));
            float shift = seam && f == fb ? 0.5f : 0.0f;
            uv[c] = {float(p.x) + shift, float(p.y)};
        }
    return m;
}

PolyCount cubeCounts() { return {8, 12, 6, 12, 0}; }

PolyCount withoutUvs(PolyCount c)
{
    c.uvs = 0;
    return c;
}
} // namespace

TEST_CASE("hud uv count: one per point per distinct uv")
{
    CHECK(hud::countUvs(*sharedTwoQuads(false), "uv0") == 6);
    CHECK(hud::countUvs(*sharedTwoQuads(true), "uv0") == 8);
    CHECK(hud::countUvs(*sharedTwoQuads(false), "uv1") == 0);
    PolyCount c = hud::countMesh(*sharedTwoQuads(true), "uv0");
    CHECK(c == PolyCount{6, 7, 2, 4, 8});
}

TEST_CASE("hud poly count: scene, selected objects and selected components")
{
    run("newScene");
    Graph& g = Graph::current();
    std::string a = made(run("polyCube")), b = made(run("polyCube")), c = made(run("polyCube"));
    g.evaluate();
    uint64_t cubeUvs = hud::countUvs(*g.node(idOf(a))->as<MeshPtr>(MeshNode::kOutMesh), "uv0");

    run("select", {{"args", {a}}});
    hud::PolyStats s = hud::polyStats(g);
    CHECK(withoutUvs(s.scene) == PolyCount{24, 36, 18, 36, 0});
    CHECK(s.scene.uvs == 3 * cubeUvs);
    CHECK(withoutUvs(s.objects) == cubeCounts());
    CHECK(s.components == PolyCount{});

    SUBCASE("hidden objects leave every column, also when hidden through a parent")
    {
        run("hide", {{"args", {b}}});
        g.evaluate();
        CHECK(hud::polyStats(g).scene.faces == 12);
        std::string grp = made(run("group", {{"args", {a}}}));
        run("select", {{"args", {a}}});
        run("hide", {{"args", {grp}}});
        g.evaluate();
        hud::PolyStats h = hud::polyStats(g);
        CHECK(h.scene.faces == 6);
        CHECK(h.objects == PolyCount{});
    }

    SUBCASE("meshes in a hidden display layer leave the counts")
    {
        std::string layer = run("createDisplayLayer", {{"args", {b}}}).get<std::string>();
        run("displayLayer", {{"args", {layer}}, {"visibility", false}});
        g.evaluate();
        CHECK(hud::polyStats(g).scene.faces == 12);
    }

    SUBCASE("a selected group counts the meshes under it")
    {
        std::string grp = made(run("group", {{"args", {a, b}}}));
        run("select", {{"args", {grp}}});
        CHECK(hud::polyStats(g).objects.faces == 12);
    }

    SUBCASE("components count only the current mode, faces with their triangles")
    {
        run("selectComponents", {{"args", {a}}, {"faces", {0, 1}}});
        run("selectComponents", {{"args", {b}}, {"faces", {2}}, {"add", true}});
        hud::PolyStats f = hud::polyStats(g);
        CHECK(f.components == PolyCount{0, 0, 3, 6, 0});
        run("selectComponents", {{"args", {a}}, {"vertices", {0, 1, 2}}});
        CHECK(hud::polyStats(g).components == PolyCount{3, 0, 0, 0, 0});
        run("selectMode", {{"args", {"object"}}});
        CHECK(hud::polyStats(g).components == PolyCount{});
    }

    SUBCASE("a view's isolate narrows the scene column")
    {
        NodeId keep = idOf(c);
        CHECK(hud::polyStats(g, "uv0", [&](NodeId id) { return id == keep; }).scene.faces == 6);
    }

    SUBCASE("a mesh isolated by components counts only the faces shown, and only the selected components on them")
    {
        NodeId plane = g.createNode(*types::find("mesh"), "TwoQuads")->id();
        g.setValue({plane, MeshNode::kInMesh, -1}, MeshPtr(sharedTwoQuads(true)));
        g.evaluate();
        isolation::Set iso;
        iso.nodes.insert(plane);
        iso.partial[plane] = {{0}, 2};
        iso.touch();
        auto shown = [&](NodeId id) { return iso.shows(id); };
        CHECK(hud::polyStats(g, "uv0", shown, &iso).scene == PolyCount{4, 4, 1, 2, 4});
        CHECK(hud::polyStats(g, "uv0", shown).scene == PolyCount{6, 7, 2, 4, 8});
        run("selectComponents", {{"args", {"TwoQuads"}}, {"vertices", {0, 2, 4}}});
        CHECK(hud::polyStats(g, "uv0", shown, &iso).components == PolyCount{2, 0, 0, 0, 0});
        iso.partial[plane] = {{0, 1}, 2};
        iso.touch();
        CHECK(hud::polyStats(g, "uv0", shown, &iso).components == PolyCount{3, 0, 0, 0, 0});
    }

    SUBCASE("LOD levels a view does not show leave the counts")
    {
        std::string grp = run("lodGroup", {{"args", {a, b, c}}}).get<std::string>();
        g.evaluate();
        lod::View view;
        view.mode = lod::Mode::Level;
        view.level = 1;
        const std::unordered_set<NodeId> hidden = lod::hidden(g, view);
        hud::PolyStats h = hud::polyStats(g, "uv0", [&](NodeId id) { return !hidden.contains(id); });
        CHECK(h.scene.faces == 6);
        CHECK(h.objects.faces == 6);
        view.mode = lod::Mode::All;
        CHECK(lod::hidden(g, view).empty());
    }
}

TEST_CASE("hud poly count: instances count once each; a reshaped snapshot carries its uvs until it settles")
{
    run("newScene");
    Graph& g = Graph::current();
    std::string a = made(run("createNode", {{"args", {"mesh"}}}));
    std::string b = made(run("createNode", {{"args", {"mesh"}}}));
    std::shared_ptr<geo::Mesh> shared = sharedTwoQuads(false);
    for (const std::string& n : {a, b})
        g.setValue({idOf(n), MeshNode::kInMesh, -1}, MeshPtr(shared));
    g.evaluate();
    hud::PolyStats s = hud::polyStats(g);
    CHECK(s.scene == PolyCount{12, 14, 4, 8, 12});
    CHECK_FALSE(s.pending);

    // Same element counts, new uvs: shown provisionally (as during a drag), exact once the snapshot holds still.
    g.setValue({idOf(a), MeshNode::kInMesh, -1}, MeshPtr(sharedTwoQuads(true)));
    g.evaluate();
    s = hud::polyStats(g);
    CHECK(s.pending);
    CHECK(s.scene.uvs == 12);
    CHECK(hud::polyStats(g).pending); // another view asking in the same frame does not count it yet
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    s = hud::polyStats(g);
    CHECK_FALSE(s.pending);
    CHECK(s.scene.uvs == 14);
}
