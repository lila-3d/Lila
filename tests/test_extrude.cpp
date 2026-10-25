#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "scene/Graph.h"
#include "scene/Components.h"
#include "scene/LastOp.h"
#include "scene/MeshNodes.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace lila;
using json = nlohmann::json;
using geo::Index;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json ui(const char* name, json call = json::object()) { return ops::execute(name, call, CallSource::UI); }

/** Faces lying on top of another face: the same corner positions in any order. */
int coincidentFaces(const geo::Mesh& m)
{
    std::set<std::vector<std::array<long long, 3>>> seen;
    int dupes = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<std::array<long long, 3>> key;
        m.forEachFaceCorner(f, [&](Index c) {
            glm::dvec3 p = m.position(m.cornerPoint(c));
            key.push_back({std::llround(p.x * 1e4), std::llround(p.y * 1e4), std::llround(p.z * 1e4)});
        });
        std::sort(key.begin(), key.end());
        if (!seen.insert(key).second)
            ++dupes;
    }
    return dupes;
}

int zeroAreaFaces(const geo::Mesh& m)
{
    int n = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceArea(f) < 1e-9)
            ++n;
    return n;
}

Index faceFacing(const geo::Mesh& m, const glm::dvec3& dir)
{
    Index best = geo::kInvalid;
    double bestDot = -2.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && glm::dot(m.faceNormal(f), dir) > bestDot)
        {
            bestDot = glm::dot(m.faceNormal(f), dir);
            best = f;
        }
    return best;
}

const geo::Mesh& meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return *n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}
} // namespace

TEST_CASE("extrude: thickness pushes the cap out, offset insets it")
{
    geo::Mesh cube = geo::makeCube();
    const Index top = faceFacing(cube, {0, 1, 0});
    geo::ExtrudeOptions o;
    o.offset = 20.0;
    o.inset = 10.0;
    geo::ExtrudeResult r = geo::extrudeFaces(cube, std::vector<Index>{top}, o);
    CHECK(r.sides.size() == 4);
    CHECK(cube.numFaces() == 10);
    CHECK(cube.numPoints() == 12);
    CHECK(cube.faceArea(top) == doctest::Approx(80.0 * 80.0));
    CHECK(cube.bounds().max.y == doctest::Approx(70.0));
    CHECK(coincidentFaces(cube) == 0);
    CHECK(zeroAreaFaces(cube) == 0);
}

TEST_CASE("inset leaves no coincident or zero-area faces")
{
    geo::Mesh cube = geo::makeCube();
    geo::insetFaces(cube, std::vector<Index>{faceFacing(cube, {0, 1, 0})}, {.thickness = 10.0});
    CHECK(cube.numFaces() == 10);
    CHECK(coincidentFaces(cube) == 0);
    CHECK(zeroAreaFaces(cube) == 0);

    // A region of four faces: one inner region plus its rim.
    geo::Mesh grid = geo::makeCube({100, 100, 100, 2, 1, 2});
    std::vector<Index> tops;
    for (Index f = 0; f < grid.faceCapacity(); ++f)
        if (grid.faceValid(f) && grid.faceNormal(f).y > 0.9)
            tops.push_back(f);
    REQUIRE(tops.size() == 4);
    const Index before = grid.numFaces();
    geo::insetFaces(grid, tops, {.thickness = 5.0});
    CHECK(grid.numFaces() == before + 8);
    CHECK(coincidentFaces(grid) == 0);
    CHECK(zeroAreaFaces(grid) == 0);
}

TEST_CASE("polyInset and polyExtrude through the command and adjust-last-op path")
{
    scene::lastOp::install();
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    ui("polyInset", {{"thickness", 5}});
    CHECK(meshOf(cube).numFaces() == 10);
    CHECK(coincidentFaces(meshOf(cube)) == 0);
    REQUIRE(scene::lastOp::live());
    scene::lastOp::rerun({{"thickness", 10}});
    scene::lastOp::rerun({{"thickness", 12}});
    CHECK(meshOf(cube).numFaces() == 10);
    CHECK(coincidentFaces(meshOf(cube)) == 0);

    run("newScene");
    cube = run("polyCube")[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    ui("polyExtrude", {{"thickness", 20}, {"offset", 10}});
    const geo::Mesh& m = meshOf(cube);
    CHECK(m.numFaces() == 10);
    CHECK(coincidentFaces(m) == 0);
    REQUIRE(scene::lastOp::live());
    scene::lastOp::rerun({{"thickness", 30}});
    CHECK(meshOf(cube).numFaces() == 10);
    glm::dvec3 size = meshOf(cube).bounds().size();
    CHECK(size.x + size.y + size.z == doctest::Approx(330.0));
    run("undo");
    size = meshOf(cube).bounds().size();
    CHECK(size.x + size.y + size.z == doctest::Approx(320.0));
    run("undo");
    CHECK(meshOf(cube).numFaces() == 6);
}

TEST_CASE("adjust-last-op stays reachable through selection changes")
{
    scene::lastOp::install();
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    ui("polyInset", {{"thickness", 5}});
    std::vector<scene::lastOp::Change> changes;
    int listener = scene::lastOp::addListener([&](scene::lastOp::Change c) { changes.push_back(c); });

    ui("selectComponents", {{"args", {cube}}, {"faces", {1}}});
    ui("selectComponents", {{"args", {cube}}, {"faces", {2}}});
    CHECK_FALSE(scene::lastOp::live());
    CHECK(scene::lastOp::reachable());
    CHECK(changes.empty());

    run("undo");
    CHECK(scene::lastOp::reachable());
    run("undo");
    CHECK(scene::lastOp::live());
    REQUIRE(changes.size() == 1);
    CHECK(changes.back() == scene::lastOp::Change::Restored);
    run("redo");
    run("redo");

    scene::lastOp::rerun({{"thickness", 10}});
    CHECK(scene::lastOp::live());
    CHECK(meshOf(cube).numFaces() == 10);
    CHECK(UndoStack::get().redoCount() == 0);
    run("undo");
    CHECK(scene::lastOp::live());
    CHECK(scene::lastOp::current()->call["thickness"] == 5);
    run("undo");
    CHECK(meshOf(cube).numFaces() == 6);
    CHECK_FALSE(scene::lastOp::reachable());
    CHECK(changes.back() == scene::lastOp::Change::Other);

    run("redo");
    CHECK(scene::lastOp::live());
    CHECK(changes.back() == scene::lastOp::Change::Restored);
    ui("selectComponents", {{"args", {cube}}, {"faces", {3}}});
    run("polyCube");
    CHECK_FALSE(scene::lastOp::reachable());
    CHECK(changes.back() == scene::lastOp::Change::Other);
    run("undo");
    CHECK(scene::lastOp::reachable());
    CHECK(changes.back() == scene::lastOp::Change::Restored);
    scene::lastOp::removeListener(listener);
}

TEST_CASE("polyInset on every face of a symmetric cube adds rims without duplicates")
{
    for (bool symmetric : {false, true})
        for (bool individual : {false, true})
        {
            run("newScene");
            std::string cube = run("polyCube", {{"subdivisionsX", 2}, {"subdivisionsY", 2}, {"subdivisionsZ", 2}})[0];
            if (symmetric)
                run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
            const int count = int(meshOf(cube).numFaces());
            for (int f = 0; f < count; ++f)
            {
                CAPTURE(f);
                CAPTURE(symmetric);
                CAPTURE(individual);
                run("selectComponents", {{"args", {cube}}, {"faces", json::array({f, (f + 1) % count})}});
                run("polyInset", {{"thickness", 5}, {"individual", individual}});
                CHECK(coincidentFaces(meshOf(cube)) == 0);
                CHECK(zeroAreaFaces(meshOf(cube)) == 0);
                run("undo");
            }
        }
}

TEST_CASE("polyBevel offset adjusts through adjust-last-op and keeps edge mode")
{
    scene::lastOp::install();
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("selectMode", {{"args", {"edge"}}});
    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    ui("polyBevel", {{"offset", 2}});
    auto spread = [&] {
        double lo = 1e9;
        const geo::Mesh& m = meshOf(cube);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                for (int i = 0; i < 3; ++i)
                    if (std::abs(m.position(p)[i]) > 1e-6 && std::abs(m.position(p)[i]) < 49.9)
                        lo = std::min(lo, std::abs(m.position(p)[i]));
        return lo;
    };
    CHECK(spread() == doctest::Approx(48.0));
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
    REQUIRE(scene::lastOp::live());
    scene::lastOp::rerun({{"offset", 10}});
    CHECK(spread() == doctest::Approx(40.0));
    // Past what the cube allows, the recorded offset stops at the clamp.
    scene::lastOp::rerun({{"offset", 500}});
    const double limit = scene::lastOp::current()->call["offset"].get<double>();
    CHECK(limit < 100.0);
    CHECK(limit > 10.0);
    run("undo");
    CHECK(spread() == doctest::Approx(40.0));
    run("undo");
    run("undo");
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
    CHECK(scene::components::selected(scene::Graph::current().find(cube)->id(), scene::ComponentMode::Edge) == std::vector<uint32_t>{0});
}

TEST_CASE("edits run in a component mode stay in it")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("selectMode", {{"args", {"face"}}});
    run("selectComponentsWhere", {{"args", {cube}}, {"kind", "face"}, {"extreme", "+z"}});
    run("selectComponentsWhere", {{"args", {cube}}, {"kind", "face"}, {"extreme", "-z"}, {"add", true}});
    run("deleteComponents");
    run("selectComponentsWhere", {{"args", {cube}}, {"kind", "edge"}, {"border", true}});
    REQUIRE(scene::components::mode() == scene::ComponentMode::Edge);
    run("polyBridge");
    CHECK(meshOf(cube).numFaces() == 8);
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    run("polyCollapse");
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
}

TEST_CASE("polyTargetWeld: vertex into target, halfway, edges, and across symmetry")
{
    auto hasPoint = [](const geo::Mesh& m, const glm::dvec3& p) {
        for (Index i = 0; i < m.pointCapacity(); ++i)
            if (m.pointValid(i) && glm::distance(m.position(i), p) < 1e-6)
                return true;
        return false;
    };
    run("newScene");
    std::string cube = run("polyCube")[0];
    const geo::Mesh before = meshOf(cube);
    const Index a = before.edgePoint(0, 0), b = before.edgePoint(0, 1);
    run("polyTargetWeld", {{"mesh", cube}, {"source", a}, {"target", b}});
    CHECK(meshOf(cube).numPoints() == 7);
    CHECK(hasPoint(meshOf(cube), before.position(b)));
    CHECK_FALSE(hasPoint(meshOf(cube), before.position(a)));
    run("undo");
    CHECK(meshOf(cube).numPoints() == 8);

    run("polyTargetWeld", {{"mesh", cube}, {"source", a}, {"target", b}, {"center", true}});
    CHECK(hasPoint(meshOf(cube), (before.position(a) + before.position(b)) * 0.5));
    run("undo");

    Index apart = geo::kInvalid;
    for (Index e = 1; e < before.edgeCapacity() && apart == geo::kInvalid; ++e)
        if (before.edgeValid(e) && before.edgePoint(e, 0) != a && before.edgePoint(e, 0) != b && before.edgePoint(e, 1) != a &&
            before.edgePoint(e, 1) != b)
            apart = e;
    REQUIRE(apart != geo::kInvalid);
    run("polyTargetWeld", {{"mesh", cube}, {"source", 0}, {"target", apart}, {"edge", true}});
    CHECK(meshOf(cube).numPoints() == 6);
    run("undo");

    // With X symmetry, welding a +X vertex to its neighbour welds the -X pair too; welding across the plane meets on it.
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    Index px = geo::kInvalid, nx = geo::kInvalid, py = geo::kInvalid;
    for (Index i = 0; i < before.pointCapacity(); ++i)
    {
        const glm::dvec3 p = before.position(i);
        if (p.x > 0 && p.y > 0 && p.z > 0)
            px = i;
        if (p.x < 0 && p.y > 0 && p.z > 0)
            nx = i;
        if (p.x > 0 && p.y < 0 && p.z > 0)
            py = i;
    }
    run("polyTargetWeld", {{"mesh", cube}, {"source", px}, {"target", py}});
    CHECK(meshOf(cube).numPoints() == 6);
    run("undo");
    run("polyTargetWeld", {{"mesh", cube}, {"source", px}, {"target", nx}});
    CHECK(meshOf(cube).numPoints() == 7);
    CHECK(hasPoint(meshOf(cube), glm::dvec3(0.0, before.position(px).y, before.position(px).z)));
}
