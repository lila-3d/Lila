#include "TestUtil.h"

#include "geo/Intersect.h"
#include "geo/Primitives.h"

#include <glm/gtc/matrix_transform.hpp>
#include <tbb/task_arena.h>

#include <cmath>

using namespace lila::geo;

namespace
{
double length(const IntersectionPolyline& l)
{
    double s = 0.0;
    const size_t n = l.points.size();
    for (size_t k = 0; k + 1 < n; ++k)
        s += glm::distance(l.points[k], l.points[k + 1]);
    if (l.closed && n > 1)
        s += glm::distance(l.points[n - 1], l.points[0]);
    return s;
}

double totalLength(const MeshIntersection& r)
{
    double s = 0.0;
    for (const IntersectionPolyline& l : r.lines)
        s += length(l);
    return s;
}

Mesh ground()
{
    PlaneOptions o;
    o.width = 300.0;
    o.depth = 300.0;
    o.subdivX = 6;
    o.subdivZ = 6;
    return makePlane(o);
}
} // namespace

TEST_CASE("triangleIntersection: crossing, coplanar and apart")
{
    const glm::dvec3 flat[3] = {{-10, 0, -10}, {10, 0, -10}, {0, 0, 10}};
    const glm::dvec3 wall[3] = {{0, -5, -20}, {0, -5, 20}, {0, 5, 0}};
    glm::dvec3 a, b;
    REQUIRE(triangleIntersection(flat, wall, a, b));
    CHECK(a.y == doctest::Approx(0.0));
    CHECK(b.y == doctest::Approx(0.0));
    CHECK(a.x == doctest::Approx(0.0));
    // The wall's cut through y = 0 runs z -10..10; the flat triangle at x = 0 spans z -10..10.
    CHECK(glm::distance(a, b) == doctest::Approx(20.0));

    const glm::dvec3 same[3] = {{-5, 0, -5}, {5, 0, -5}, {0, 0, 5}};
    bool coplanar = false;
    CHECK_FALSE(triangleIntersection(flat, same, a, b, &coplanar));
    CHECK(coplanar);

    const glm::dvec3 above[3] = {{0, 1, -20}, {0, 1, 20}, {0, 5, 0}};
    CHECK_FALSE(triangleIntersection(flat, above, a, b, &coplanar));
    CHECK_FALSE(coplanar);
}

TEST_CASE("intersectMeshes: a cube through a plane gives one closed square")
{
    const MeshBvh plane(ground());
    const MeshBvh cube(makeCube());
    const MeshIntersection r = intersectMeshes(plane, cube, glm::dmat4(1.0));
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].closed);
    CHECK(totalLength(r) == doctest::Approx(400.0));
    CHECK(r.lines[0].facesA.size() == r.lines[0].points.size());
    for (const glm::dvec3& p : r.lines[0].points)
        CHECK(std::abs(p.y) < 1e-9);

    // The other way round (the larger mesh probes) gives the same loop.
    const MeshIntersection back = intersectMeshes(cube, plane, glm::dmat4(1.0));
    REQUIRE(back.lines.size() == 1);
    CHECK(totalLength(back) == doctest::Approx(400.0));
}

TEST_CASE("intersectMeshes: instances placed by transform")
{
    const MeshBvh plane(ground());
    const MeshBvh cube(makeCube());
    const glm::dmat4 up30 = glm::translate(glm::dmat4(1.0), glm::dvec3(0, 30, 0));
    const glm::dmat4 up60 = glm::translate(glm::dmat4(1.0), glm::dvec3(0, 60, 0));
    const glm::dmat4 turned = glm::rotate(glm::dmat4(1.0), glm::radians(45.0), glm::dvec3(0, 1, 0)) * up30;
    CHECK(totalLength(intersectMeshes(plane, cube, up30)) == doctest::Approx(400.0));
    CHECK(intersectMeshes(plane, cube, up60).lines.empty());
    CHECK(totalLength(intersectMeshes(plane, cube, turned)) == doctest::Approx(400.0));
}

TEST_CASE("intersectMeshes: a face lying on the plane is coplanar, not a seam")
{
    CubeOptions o;
    o.baseAtOrigin = true;
    const MeshBvh plane(ground());
    const MeshBvh sitting(makeCube(o));
    const MeshIntersection r = intersectMeshes(plane, sitting, glm::dmat4(1.0));
    CHECK(r.coplanarPairs > 0);
}

TEST_CASE("intersectObjects: pairs across objects only, in world space")
{
    const MeshBvh plane(ground());
    const MeshBvh cube(makeCube());
    const glm::dmat4 farAway = glm::translate(glm::dmat4(1.0), glm::dvec3(1000, 0, 0));
    std::vector<IntersectionInput> in = {
        {&plane, farAway, 0},
        {&cube, farAway, 1},
        {&cube, glm::translate(farAway, glm::dvec3(0, 20, 0)), 1}, // same object as the first cube
    };
    const std::vector<ObjectIntersection> r = intersectObjects(in);
    REQUIRE(r.size() == 2);
    CHECK(r[0].a == 0);
    CHECK(r[0].b == 1);
    CHECK(r[1].a == 0);
    CHECK(r[1].b == 2);
    for (const glm::dvec3& p : r[0].result.lines[0].points)
        CHECK(p.x > 900.0);
}

TEST_CASE("intersectHeightfield: flat, sloped, holes")
{
    const MeshBvh cube(makeCube());
    Heightfield flat = makeHeightfield(31, 31, 10.0, glm::dvec2(-150.0), 0.0f);
    const MeshIntersection r = intersectHeightfield(cube, glm::dmat4(1.0), flat);
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].closed);
    CHECK(totalLength(r) == doctest::Approx(400.0));

    Heightfield slope = flat;
    for (int z = 0; z < slope.nz; ++z)
        for (int x = 0; x < slope.nx; ++x)
            slope.heights[slope.index(x, z)] = float(0.2 * slope.samplePos(x, z).x);
    const MeshIntersection s = intersectHeightfield(cube, glm::dmat4(1.0), slope);
    REQUIRE(s.lines.size() == 1);
    CHECK(totalLength(s) == doctest::Approx(200.0 + 2.0 * std::sqrt(100.0 * 100.0 + 20.0 * 20.0)));

    // The cube moved off the grid crosses nothing.
    CHECK(intersectHeightfield(cube, glm::translate(glm::dmat4(1.0), glm::dvec3(500, 0, 0)), flat).lines.empty());

    Heightfield open = flat;
    std::fill(open.mask("holes").begin(), open.mask("holes").end(), 1.0f);
    CHECK(intersectHeightfield(cube, glm::dmat4(1.0), open).lines.empty());
}

TEST_CASE("intersection is deterministic across thread counts")
{
    PlaneOptions po;
    po.width = 300.0;
    po.depth = 300.0;
    po.subdivX = 60;
    po.subdivZ = 60;
    const MeshBvh plane(makePlane(po));
    CubeOptions co;
    co.subdivX = co.subdivY = co.subdivZ = 8;
    const MeshBvh cube(makeCube(co));
    const glm::dmat4 m = glm::rotate(glm::dmat4(1.0), 0.3, glm::normalize(glm::dvec3(1, 2, 3)));
    MeshIntersection one;
    tbb::task_arena(1).execute([&] { one = intersectMeshes(plane, cube, m); });
    const MeshIntersection many = intersectMeshes(plane, cube, m);
    REQUIRE(one.lines.size() == many.lines.size());
    for (size_t i = 0; i < one.lines.size(); ++i)
    {
        REQUIRE(one.lines[i].points.size() == many.lines[i].points.size());
        for (size_t k = 0; k < one.lines[i].points.size(); ++k)
            CHECK(one.lines[i].points[k] == many.lines[i].points[k]);
    }
    REQUIRE(one.lines.size() == 1);
    CHECK(one.lines[0].closed);
}
