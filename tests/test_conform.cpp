#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/Conform.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "scene/MeshNodes.h"

#include <algorithm>
#include <cmath>

using namespace lila;
using json = nlohmann::json;
using geo::Index;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json ui(const char* name, json call = json::object()) { return ops::execute(name, call, CallSource::UI); }

const geo::Mesh& meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return *n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

/** Plane point id at column i, row j (rows run toward -Z). */
Index gridId(int sx, int i, int j) { return Index(j * (sx + 1) + i); }

double spreadOf(const std::vector<double>& v)
{
    auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    return *hi - *lo;
}

void applyMoves(geo::Mesh& m, const geo::PointMoves& moves)
{
    for (const geo::PointMove& mv : moves)
        m.setPosition(mv.point, mv.position);
}
} // namespace

TEST_CASE("conform: selections become chains, borders and interiors")
{
    geo::Mesh plane = geo::makePlane({100, 100, 4, 4});
    std::vector<Index> all;
    for (Index f = 0; f < plane.faceCapacity(); ++f)
        all.push_back(f);
    geo::ShapeSelection s = geo::shapeOfFaces(plane, all);
    REQUIRE(s.chains.size() == 1);
    CHECK(s.chains[0].closed);
    CHECK(s.chains[0].points.size() == 16);
    CHECK(s.interior.size() == 9);

    // A row of points is a chain; the corners of one face are that face.
    std::vector<Index> row;
    for (int i = 0; i <= 4; ++i)
        row.push_back(gridId(4, i, 0));
    s = geo::shapeOfPoints(plane, row);
    REQUIRE(s.chains.size() == 1);
    CHECK_FALSE(s.chains[0].closed);
    CHECK(s.chains[0].points.size() == 5);
    s = geo::shapeOfPoints(plane, std::vector<Index>{gridId(4, 0, 0), gridId(4, 1, 0), gridId(4, 1, 1), gridId(4, 0, 1)});
    REQUIRE(s.chains.size() == 1);
    CHECK(s.chains[0].closed);
    CHECK(s.interior.empty());
}

TEST_CASE("conform: circularize a face region keeps the border on one circle and the interior inside")
{
    geo::Mesh plane = geo::makePlane({100, 60, 4, 4});
    std::vector<Index> all;
    for (Index f = 0; f < plane.faceCapacity(); ++f)
        all.push_back(f);
    geo::ShapeSelection s = geo::shapeOfFaces(plane, all);
    geo::PointMoves moves = geo::circularize(plane, s, {});
    CHECK(moves.size() == 16);
    geo::followInterior(plane, s.interior, moves);
    CHECK(moves.size() == 25);
    geo::Mesh out = plane;
    applyMoves(out, moves);

    std::vector<double> radii, gaps;
    const std::vector<Index>& loop = s.chains[0].points;
    for (size_t i = 0; i < loop.size(); ++i)
    {
        glm::dvec3 p = out.position(loop[i]);
        CHECK(p.y == doctest::Approx(0.0).epsilon(1e-9));
        radii.push_back(glm::length(glm::dvec2(p.x, p.z)));
        gaps.push_back(glm::length(out.position(loop[(i + 1) % loop.size()]) - p));
    }
    CHECK(spreadOf(radii) < 1e-6);
    CHECK(spreadOf(gaps) < 1e-6);
    // The rectangle's mean radius, give or take.
    CHECK(radii[0] > 30.0);
    CHECK(radii[0] < 50.0);
    for (Index p : s.interior)
        CHECK(glm::length(glm::dvec2(out.position(p).x, out.position(p).z)) < radii[0]);
    // The centre stays put by symmetry.
    CHECK(glm::length(out.position(gridId(4, 2, 2))) < 1e-5);

    // A fixed radius and a polygon: four sides put every point on a square with its corners on the circle.
    geo::PointMoves squared = geo::circularize(plane, s, {.radius = 40.0, .sides = 4});
    for (const geo::PointMove& mv : squared)
    {
        double r = glm::length(glm::dvec2(mv.position.x, mv.position.z));
        CHECK(r <= 40.0 + 1e-9);
        CHECK(r >= 40.0 / std::sqrt(2.0) - 1e-9);
    }
}

TEST_CASE("conform: a square loop already on its circle does not move")
{
    geo::Mesh plane = geo::makePlane({100, 100, 1, 1});
    geo::ShapeSelection s = geo::shapeOfFaces(plane, std::vector<Index>{0});
    for (const geo::PointMove& mv : geo::circularize(plane, s, {}))
        CHECK(glm::length(mv.position - plane.position(mv.point)) < 1e-9);
}

TEST_CASE("conform: rectangle, sphere, cylinder, plane and line fits")
{
    // A circle of 16 points squared up: corners land on four of its points, every point on the square's outline.
    geo::Mesh disc = geo::makeDisc({50, 16, 0});
    std::vector<Index> faces;
    for (Index f = 0; f < disc.faceCapacity(); ++f)
        if (disc.faceValid(f))
            faces.push_back(f);
    geo::ShapeSelection s = geo::shapeOfFaces(disc, faces);
    REQUIRE(s.chains.size() == 1);
    geo::PointMoves moves = geo::rectangularize(disc, s, {.spacing = geo::Spacing::Even, .square = true});
    REQUIRE(moves.size() == 16);
    glm::dvec3 c(0.0);
    for (const geo::PointMove& mv : moves)
        c += mv.position / 16.0;
    // The farthest point is a corner, so the square's sides run at 45 degrees to it; along those every point has one coordinate at the half size.
    glm::dvec3 corner = std::max_element(moves.begin(), moves.end(), [&](auto& a, auto& b) {
                            return glm::length(a.position - c) < glm::length(b.position - c);
                        })->position - c;
    const glm::dvec3 d0 = glm::normalize(corner), across = glm::cross(glm::dvec3(0, 1, 0), d0);
    const glm::dvec3 e1 = glm::normalize(d0 + across), e2 = glm::normalize(d0 - across);
    std::vector<double> reach;
    for (const geo::PointMove& mv : moves)
    {
        CHECK(mv.position.y == doctest::Approx(0.0));
        glm::dvec3 d = mv.position - c;
        reach.push_back(std::max(std::abs(glm::dot(d, e1)), std::abs(glm::dot(d, e2))));
    }
    CHECK(spreadOf(reach) < 1e-6);

    // Spherize: a subdivided cube's points all end at one distance from the centre.
    geo::Mesh cube = geo::makeCube({100, 100, 100, 3, 3, 3, false});
    std::vector<Index> pts;
    for (Index p = 0; p < cube.pointCapacity(); ++p)
        if (cube.pointValid(p))
            pts.push_back(p);
    std::vector<double> radii;
    for (const geo::PointMove& mv : geo::spherize(cube, pts))
        radii.push_back(glm::length(mv.position));
    CHECK(radii.size() == pts.size());
    CHECK(spreadOf(radii) < 1e-6);

    // Cylinder: a dented cylinder's side comes back round about its own axis.
    geo::Mesh cyl = geo::makeCylinder({30, 100, 12, 2, 0, false});
    std::vector<Index> side;
    for (Index p = 0; p < cyl.pointCapacity(); ++p)
        if (cyl.pointValid(p))
            side.push_back(p);
    // Opposite points on one ring, so the fitted axis stays on Y.
    for (Index p : {side[3], side[9]})
        cyl.setPosition(p, cyl.position(p) * glm::dvec3(1.2, 1.0, 1.2));
    radii.clear();
    for (const geo::PointMove& mv : geo::cylinderize(cyl, side))
        radii.push_back(glm::length(glm::dvec2(mv.position.x, mv.position.z)));
    CHECK(radii.size() == side.size());
    CHECK(spreadOf(radii) < 1e-6);

    // Plane and line.
    geo::Mesh plane = geo::makePlane({100, 100, 4, 4});
    plane.setPosition(gridId(4, 2, 2), {0, 10, 0});
    std::vector<Index> grid;
    for (Index p = 0; p < plane.pointCapacity(); ++p)
        grid.push_back(p);
    std::vector<double> heights;
    for (const geo::PointMove& mv : geo::flattenToPlane(plane, grid))
        heights.push_back(mv.position.y);
    CHECK(spreadOf(heights) < 1e-9);
    heights.clear();
    for (const geo::PointMove& mv : geo::alignToLine(plane, std::vector<Index>{gridId(4, 0, 0), gridId(4, 4, 4), gridId(4, 2, 2)}))
        heights.push_back(mv.position.x + mv.position.z);
    CHECK(spreadOf(heights) < 1e-6);
}

TEST_CASE("conform: conformToMesh lands a ring one to one on a cylinder's rim")
{
    // The plane's border has 8 points; the open cylinder's rims have 8 too.
    geo::Mesh plane = geo::makePlane({60, 60, 2, 2});
    for (Index p = 0; p < plane.pointCapacity(); ++p)
        plane.setPosition(p, plane.position(p) + glm::dvec3(2, 49, 3));
    geo::Mesh cyl = geo::makeCylinder({25, 100, 8, 1, 0, false});
    std::vector<Index> faces = {0, 1, 2, 3};
    geo::ShapeSelection s = geo::shapeOfFaces(plane, faces);
    geo::PointMoves moves = geo::conformToMesh(plane, s, cyl, {});
    REQUIRE(moves.size() == 8);
    std::vector<Index> used;
    for (const geo::PointMove& mv : moves)
    {
        double best = 1e9;
        Index at = geo::kInvalid;
        for (Index p = 0; p < cyl.pointCapacity(); ++p)
            if (double d = glm::length(cyl.position(p) - mv.position); d < best)
                best = d, at = p;
        CHECK(best < 1e-9);
        CHECK(cyl.position(at).y == doctest::Approx(50.0));
        used.push_back(at);
    }
    std::sort(used.begin(), used.end());
    CHECK(std::unique(used.begin(), used.end()) == used.end());
    geo::followInterior(plane, s.interior, moves);
    CHECK(moves.size() == 9);

    // Surface mode: every point onto the sphere.
    geo::Mesh sphere = geo::makeSphere({50, 32, 32});
    geo::PointMoves onto = geo::conformToMesh(plane, s, sphere, {.mode = geo::ConformMode::Surface});
    CHECK(onto.size() == 9);
    for (const geo::PointMove& mv : onto)
    {
        CHECK(glm::length(mv.position) <= 50.0 + 1e-6);
        CHECK(glm::length(mv.position) > 49.0);
    }
}

TEST_CASE("conform commands: straighten, circularize, spherize, shape and object, undoable and adjustable")
{
    scene::lastOp::install();
    run("newScene");
    std::string plane = run("polyPlane", {{"subdivisionsX", 4}, {"subdivisionsZ", 2}})[0];
    // Bend the first and last rows, then straighten both chains at once.
    run("selectComponents", {{"args", {plane}}, {"vertices", {gridId(4, 2, 0), gridId(4, 1, 2)}}});
    run("moveComponents", {{"by", {0, 10, 0}}});
    std::vector<Index> rows;
    for (int i = 0; i <= 4; ++i)
        rows.push_back(gridId(4, i, 0)), rows.push_back(gridId(4, i, 2));
    run("selectComponents", {{"args", {plane}}, {"vertices", rows}});
    ui("polyStraighten");
    for (Index p : rows)
        CHECK(meshOf(plane).position(p).y == doctest::Approx(0.0));
    CHECK(meshOf(plane).position(gridId(4, 2, 0)).x == doctest::Approx(0.0));
    REQUIRE(scene::lastOp::live());
    scene::lastOp::rerun({{"strength", 0.5}});
    CHECK(meshOf(plane).position(gridId(4, 2, 0)).y == doctest::Approx(5.0));
    run("undo");
    CHECK(meshOf(plane).position(gridId(4, 2, 0)).y == doctest::Approx(10.0));

    // Circularize a face region, then square it, then adjust the radius live.
    run("newScene");
    plane = run("polyPlane", {{"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    std::vector<int> all;
    for (int f = 0; f < 16; ++f)
        all.push_back(f);
    run("selectComponents", {{"args", {plane}}, {"faces", all}});
    ui("polyCircularize");
    auto radiusOf = [&](Index p) { return glm::length(glm::dvec2(meshOf(plane).position(p).x, meshOf(plane).position(p).z)); };
    CHECK(radiusOf(gridId(4, 0, 0)) == doctest::Approx(radiusOf(gridId(4, 2, 0))));
    REQUIRE(scene::lastOp::live());
    scene::lastOp::rerun({{"radius", 30.0}});
    CHECK(radiusOf(gridId(4, 0, 0)) == doctest::Approx(30.0));
    CHECK(radiusOf(gridId(4, 3, 4)) == doctest::Approx(30.0));
    CHECK(radiusOf(gridId(4, 1, 1)) < 30.0);
    ui("polyConformShape", {{"shape", "square"}});
    run("undo");
    CHECK(radiusOf(gridId(4, 3, 4)) == doctest::Approx(30.0));

    run("newScene");
    std::string cube = run("polyCube", {{"subdivisionsX", 2}, {"subdivisionsY", 2}, {"subdivisionsZ", 2}})[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("convertSelection", {{"args", {"vertex"}}});
    run("selectAllComponents");
    ui("polySpherize");
    std::vector<double> radii;
    const geo::Mesh& m = meshOf(cube);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            radii.push_back(glm::length(m.position(p)));
    CHECK(spreadOf(radii) < 1e-6);

    // Conform to object: the other selected mesh is the target.
    run("newScene");
    std::string cyl = run("polyCylinder", {{"radius", 25}, {"height", 100}, {"subdivisionsAxis", 8}, {"caps", false}})[0];
    plane = run("polyPlane", {{"width", 60}, {"depth", 60}, {"subdivisionsX", 2}, {"subdivisionsZ", 2}})[0];
    run("move", {{"args", {plane}}, {"to", {0, 48, 0}}});
    run("selectComponents", {{"args", {plane}}, {"faces", {0, 1, 2, 3}}});
    ui("polyConformToObject", {{"target", cyl}});
    json pts = run("meshData", {{"args", {plane}}, {"worldSpace", true}})["points"];
    int onRim = 0;
    for (const json& p : pts)
        if (std::abs(p[1].get<double>() - 50.0) < 1e-6 && std::abs(std::hypot(p[0].get<double>(), p[2].get<double>()) - 25.0) < 1e-6)
            ++onRim;
    CHECK(onRim == 8);
    CHECK_THROWS_AS(run("polyConformToObject", {{"target", plane}}), CommandError);
}

TEST_CASE("conform commands follow symmetry")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("setSymmetry", {{"args", {plane}}, {"axis", "x"}});
    // The +X half's middle 2x2 block of faces.
    run("selectComponents", {{"args", {plane}}, {"faces", {6, 7, 10, 11}}});
    run("polyCircularize", {{"radius", 20.0}});
    const geo::Mesh& m = meshOf(plane);
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        glm::dvec3 a = m.position(p);
        double best = 1e9;
        for (Index q = 0; q < m.pointCapacity(); ++q)
            if (m.pointValid(q))
                best = std::min(best, glm::length(m.position(q) - glm::dvec3(-a.x, a.y, a.z)));
        CHECK(best < 1e-6);
    }
}
