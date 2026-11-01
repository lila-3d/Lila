#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "uv/LiveSession.h"
#include "uv/Uv.h"

#include <doctest/doctest.h>

#include <thread>

using namespace lila;
using geo::Index;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

scene::MeshPtr meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

std::vector<glm::vec2> uvsOf(const std::string& name)
{
    auto uv = meshOf(name)->cornerAttrs().get<glm::vec2>("uv0");
    return {uv.begin(), uv.end()};
}

/** Edges of a plane on the line x = 0. */
std::vector<Index> middleLine(const geo::Mesh& m)
{
    std::vector<Index> out;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
            if (std::abs(a.x) < 1e-6 && std::abs(b.x) < 1e-6)
                out.push_back(e);
        }
    return out;
}

std::string plane()
{
    run("newScene");
    std::string p = run("polyPlane", {{"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("select", {{"args", {p}}});
    run("uvProject", {{"type", "planar"}, {"axis", "y"}});
    return p;
}
} // namespace

TEST_CASE("uvLiveRelax, uvPin and uvUnpin")
{
    run("newScene");
    std::string s = run("polySphere")[0];
    run("select", {{"args", {s}}});
    run("uvAuto");
    json r = run("uvLiveRelax", {{"iterations", 100}, {"balance", 0.5}});
    CHECK(r["islands"].get<int>() > 0);
    CHECK(r["folded"] == 0);
    run("undo");
    run("redo");

    // Pinned uvs do not move when the island relaxes, even from scratch.
    run("selectComponents", {{"args", {s}}, {"faces", {0, 1, 2}}});
    CHECK(run("uvPin")["corners"].get<int>() >= 9);
    std::vector<glm::vec2> before = uvsOf(s);
    auto pin = meshOf(s)->cornerAttrs().get<bool>("uvPin");
    run("select", {{"args", {s}}});
    run("uvLiveRelax", {{"flatten", true}});
    std::vector<glm::vec2> after = uvsOf(s);
    size_t checked = 0;
    for (size_t c = 0; c < pin.size(); ++c)
        if (pin[c])
        {
            CHECK(glm::length(after[c] - before[c]) < 1e-5f);
            ++checked;
        }
    CHECK(checked >= 9);
    run("uvUnpin", {{"all", true}});
    auto cleared = meshOf(s)->cornerAttrs().get<bool>("uvPin");
    CHECK(std::none_of(cleared.begin(), cleared.end(), [](bool b) { return b; }));
}

TEST_CASE("uvCut and uvSew re-chart the islands")
{
    std::string p = plane();
    std::vector<Index> line = middleLine(*meshOf(p));
    REQUIRE(line.size() == 4);
    json j = json::array();
    for (Index e : line)
        j.push_back(e);

    // A full cut across the plane: two islands (a flat sheet does not move apart, but the seam splits it).
    json cut = run("uvCut", {{"args", {p}}, {"edges", j}, {"relax", false}});
    CHECK(cut["cut"] == 4);
    CHECK(cut["islands"] == 2);
    CHECK(uv::liveIslands(*meshOf(p), {}, "uv0").size() == 2);

    // Move one half away, then Move and Sew brings it back and welds it.
    std::vector<glm::vec2> flat = uvsOf(p);
    std::vector<Index> left;
    {
        auto m = meshOf(p);
        for (Index f = 0; f < m->faceCapacity(); ++f)
            if (m->faceValid(f) && m->faceCentroid(f).x < 0.0)
                left.push_back(f);
    }
    {
        // Straight onto the mesh: uvTransform would drag the other side's coincident uvs along.
        scene::Graph& g = scene::Graph::current();
        geo::Mesh m = *meshOf(p);
        std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
        for (Index f : left)
            m.forEachFaceCorner(f, [&](Index c) { uv[c] = glm::vec2(-uv[c].y, uv[c].x) + glm::vec2(0.0f, 0.7f); });
        UndoChunk chunk("move half");
        g.setValue({g.find(p)->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    }
    CHECK(run("uvLayout")["islands"] == 2);
    json sew = run("uvSew", {{"args", {p}}, {"edges", j}});
    CHECK(sew["sewn"] == 4);
    CHECK(sew["islands"] == 1);
    CHECK(sew["folded"] == 0);
    CHECK(run("uvLayout")["islands"] == 1);
    CHECK(run("uvLayout")["overlappingFaces"] == 0);
    CHECK(uv::liveIslands(*meshOf(p), {}, "uv0").size() == 1);
    // Undo walks back through sew and move.
    run("undo");
    CHECK(run("uvLayout")["islands"] == 2);
    run("undo");
    CHECK(uvsOf(p) == flat);
}

TEST_CASE("a live stroke is one undo step and its preview records nothing")
{
    std::string p = plane();
    scene::NodeId node = scene::Graph::current().find(p)->id();
    std::vector<glm::vec2> start = uvsOf(p);
    UndoStack& u = UndoStack::get();
    std::string labelBefore = u.undoLabel();
    uint64_t serial = u.serial();

    uv::live::Session* s = uv::live::get(node);
    REQUIRE(s);
    REQUIRE(s->islandCount() == 1);
    s->begin("UV Live Drag");
    s->setStroking(true);
    // Drag the corner wedge of corner 0 across several frames.
    auto [island, vertex] = s->wedge(0);
    REQUIRE(island == 0);
    glm::dvec2 from = s->topology(0).uv[vertex];
    for (int i = 1; i <= 8; ++i)
    {
        glm::dvec2 to = from + glm::dvec2(0.04 * i, 0.03 * i);
        s->wake({island}, [vertex, to](uv::LiveIsland& is) { is.setTarget(vertex, to, 1.0); });
        s->solver().waitIdle(2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
        s->tick();
        CHECK(s->open());
    }
    // The preview reached the scene, without an undo entry.
    CHECK(uvsOf(p) != start);
    CHECK(u.serial() == serial);
    s->wake({island}, [](uv::LiveIsland& is) { is.clearTargets(); });
    s->finish();
    CHECK(!s->open());
    CHECK(u.serial() != serial);
    CHECK(u.undoLabel() == "UV Live Drag");
    std::vector<glm::vec2> moved = uvsOf(p);
    CHECK(moved != start);
    run("undo");
    CHECK(uvsOf(p) == start);
    CHECK(u.undoLabel() == labelBefore);
    run("redo");
    CHECK(uvsOf(p) == moved);

    // Another command commits an open transaction first, so it lands as its own step.
    s = uv::live::get(node);
    REQUIRE(s);
    s->begin("UV Live Brush");
    s->setStroking(true);
    s->wake({0}, [](uv::LiveIsland& is) {
        uv::LiveDab d;
        d.brush = uv::LiveBrush::Pin;
        d.center = is.chart().uv[0];
        d.radius = 1e-3;
        is.applyDab(d);
    });
    s->solver().waitIdle(2000);
    run("select", {{"args", {p}}});
    CHECK(!s->open());
    CHECK(u.undoLabel() == "UV Live Brush");
    auto pin = meshOf(p)->cornerAttrs().get<bool>("uvPin");
    REQUIRE(!pin.empty());
    CHECK(pin[0]);
    run("undo");
    CHECK(u.undoLabel() == "UV Live Drag");
}

TEST_CASE("uvAlignToEdge and uvOrient turn the whole island, not just the selected part")
{
    std::string p = plane();
    run("uvTransform", {{"rotate", 30.0}, {"perIsland", true}});
    Index edge = middleLine(*meshOf(p)).front();
    run("selectComponents", {{"args", {p}}, {"edges", {edge}}});
    CHECK(run("uvAlignToEdge")["islands"] == 1);

    run("select", {{"args", {p}}});
    CHECK(run("uvLayout")["islands"] == 1);
    scene::MeshPtr mesh = meshOf(p);
    const geo::Mesh& after = *mesh;
    auto uv = after.cornerAttrs().get<glm::vec2>("uv0");
    // The selected edge lies on an axis.
    glm::vec2 d(0.0f);
    after.forEachEdgeCorner(edge, [&](Index c) { d = uv[after.cornerNext(c)] - uv[c]; });
    CHECK(std::min(std::abs(d.x), std::abs(d.y)) < 1e-4f);

    run("uvTransform", {{"rotate", 30.0}, {"perIsland", true}});
    run("selectComponents", {{"args", {p}}, {"faces", {0}}});
    CHECK(run("uvOrient")["islands"] == 1);
    run("select", {{"args", {p}}});
    CHECK(run("uvLayout")["islands"] == 1);
}
