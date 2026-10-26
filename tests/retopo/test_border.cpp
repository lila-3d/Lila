#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Primitives.h"
#include "geo/TestUtil.h"
#include "retopo/Retopo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace lila;
using geo::Index;

namespace
{
/** A border edge of the mesh whose midpoint is nearest p. */
Index borderEdgeNear(const geo::Mesh& m, const glm::dvec3& p)
{
    Index best = geo::kInvalid;
    double bestD = 1e300;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
        {
            double d = glm::length((m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1))) * 0.5 - p);
            if (d < bestD)
            {
                bestD = d;
                best = e;
            }
        }
    return best;
}

/** Consecutive run edges join at the listed points. */
void checkChained(const geo::Mesh& m, const retopo::BorderRun& r)
{
    REQUIRE(r.points.size() == r.edges.size() + (r.closed ? 0 : 1));
    for (size_t i = 0; i < r.edges.size(); ++i)
    {
        Index a = r.points[i], b = r.points[(i + 1) % r.points.size()];
        CHECK(m.findEdge(a, b) == r.edges[i]);
    }
}

const geo::Mesh& outMesh(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return *g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}
} // namespace

TEST_CASE("border walk: whole loop and angle threshold on a grid")
{
    geo::PlaneOptions o;
    o.width = 30.0;
    o.depth = 30.0;
    o.subdivX = 3;
    o.subdivZ = 3;
    geo::Mesh m = geo::makePlane(o);
    // The middle edge of one side; the border is 12 edges with four 90 degree corners.
    Index e = borderEdgeNear(m, {0.0, 0.0, 15.0});
    REQUIRE(e != geo::kInvalid);

    retopo::BorderRun loop = retopo::walkBorder(m, e);
    CHECK(loop.closed);
    CHECK(loop.edges.size() == 12);
    CHECK(loop.edges.front() == e);
    checkChained(m, loop);

    retopo::BorderRun side = retopo::walkBorder(m, e, 45.0);
    CHECK_FALSE(side.closed);
    CHECK(side.edges.size() == 3);
    checkChained(m, side);
    for (Index p : {side.points.front(), side.points.back()})
    {
        glm::dvec3 c = m.position(p);
        CHECK(std::abs(std::abs(c.x) - 15.0) < 1e-9);
        CHECK(std::abs(c.z - 15.0) < 1e-9);
    }

    CHECK(retopo::walkBorder(m, e, 91.0).closed);
    CHECK(retopo::walkBorder(m, e, 89.0).edges.size() == 3);

    // Interior edges have no border run.
    for (Index i = 0; i < m.edgeCapacity(); ++i)
        if (m.edgeValid(i) && !m.isBoundaryEdge(i))
        {
            CHECK(retopo::walkBorder(m, i).edges.empty());
            break;
        }
}

TEST_CASE("border walk: open cylinder rims are smooth loops")
{
    geo::CylinderOptions o;
    o.caps = false;
    o.subdivAxis = 16;
    geo::Mesh m = geo::makeCylinder(o);
    Index e = borderEdgeNear(m, {50.0, 50.0, 0.0});
    REQUIRE(e != geo::kInvalid);
    // 16 sides turn 22.5 degrees each.
    retopo::BorderRun r = retopo::walkBorder(m, e, 30.0);
    CHECK(r.closed);
    CHECK(r.edges.size() == 16);
    checkChained(m, r);
    CHECK(retopo::walkBorder(m, e, 20.0).edges.size() == 1);
}

TEST_CASE("border walk: a loop cut by one sharp corner opens there")
{
    // A 12-gon with one point pulled out into a spike: every turn is about 30 degrees except at the spike.
    geo::Mesh m;
    std::vector<Index> pts;
    for (int k = 0; k < 12; ++k)
    {
        double a = k * 3.14159265358979323846 / 6.0, r = k == 0 ? 30.0 : 10.0;
        pts.push_back(m.addPoint({r * std::cos(a), r * std::sin(a), 0.0}));
    }
    m.addFace(pts);
    Index e = m.findEdge(pts[3], pts[4]);
    retopo::BorderRun r = retopo::walkBorder(m, e, 45.0);
    CHECK_FALSE(r.closed);
    CHECK(r.edges.size() == 12);
    CHECK(r.points.front() == pts[0]);
    CHECK(r.points.back() == pts[0]);
    checkChained(m, r);
    CHECK(retopo::walkBorder(m, e, 170.0).closed);
}

TEST_CASE("commands: retopoDrawAddFace faces= extrudes a strip in one undo step")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    auto P = [](double x, double y) { return json::array({x, y, 0.0}); };
    ops::execute("retopoDrawAddFace", {{"mesh", "strip"}, {"create", true}, {"points", {P(0, 0), P(10, 0), P(10, 10), P(0, 10)}}});
    ops::execute("retopoDrawAddFace", {{"mesh", "strip"}, {"points", {P(10, 0), P(20, 0), P(20, 10), P(10, 10)}}, {"weld", 0.5}});
    // The bottom border, extruded down; the repeated first face is skipped.
    json faces = {{P(0, 0), P(10, 0), P(10, -5), P(0, -5)},
                  {P(10, 0), P(20, 0), P(20, -5), P(10, -5)},
                  {P(0, 0), P(10, 0), P(10, -5), P(0, -5)}};
    ops::execute("retopoDrawAddFace", {{"mesh", "strip"}, {"faces", faces}, {"weld", 0.5}});
    {
        const geo::Mesh& m = outMesh("strip");
        CHECK(m.numFaces() == 4);
        CHECK(m.numPoints() == 9);
        CHECK(geo::test::isValid(m));
    }
    UndoStack::get().undo();
    CHECK(outMesh("strip").numFaces() == 2);
    CHECK_THROWS(ops::execute("retopoDrawAddFace", {{"mesh", "strip"}, {"faces", {{P(0, 0), P(10, 0), P(10, 10), P(0, 10)}}}, {"weld", 0.5}}));
    scene::Graph::current().clear();
    UndoStack::get().clear();
}

TEST_CASE("commands: retopo draw follows radial symmetry")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    auto P = [](double x, double y, double z) { return json::array({x, y, z}); };
    ops::execute("retopoDrawAddFace", {{"mesh", "rim"}, {"create", true}, {"points", {P(20, 0, 1), P(30, 0, 1), P(30, 10, 1), P(20, 10, 1)}}});
    ops::execute("setSymmetry", {{"args", {"rim"}}, {"axis", "radial"}, {"count", 6}, {"radialAxis", "y"}});
    ops::execute("retopoDrawAddFace", {{"mesh", "rim"}, {"points", {P(20, 0, 5), P(30, 0, 5), P(30, 10, 5), P(20, 10, 5)}}});
    {
        // The first face was drawn before symmetry: alone; the second comes with its five turned copies.
        const geo::Mesh& m = outMesh("rim");
        CHECK(m.numFaces() == 7);
        CHECK(geo::test::isValid(m));
    }
    json move = {{"mesh", "rim"}, {"points", {4}}, {"positions", {P(21, 1, 5)}}};
    ops::execute("retopoDrawMove", move);
    const geo::Mesh m = outMesh("rim");
    const auto spec = scene::symmetrySpec(*scene::Graph::current().find("rim"));
    REQUIRE(spec);
    geo::SymmetryMap map = geo::SymmetryMap::buildFor(m, *spec, 1e-3);
    Index followers = 0;
    for (int g = 1; g < map.order(); ++g)
        if (const Index q = map.image(g, 4); q != geo::kInvalid && q != 4)
        {
            CHECK(glm::length(m.position(q) - map.transform(g, m.position(4))) < 1e-9);
            ++followers;
        }
    CHECK(followers == 5);
    scene::Graph::current().clear();
    UndoStack::get().clear();
}
