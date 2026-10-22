#include "TestUtil.h"

#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
/** The face of a cube-like mesh whose normal is closest to dir. */
Index faceFacing(const Mesh& m, const glm::dvec3& dir)
{
    Index best = kInvalid;
    double bestDot = -2.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            const double d = glm::dot(m.faceNormal(f), dir);
            if (d > bestDot)
            {
                bestDot = d;
                best = f;
            }
        }
    return best;
}

Index edgeBetween(const Mesh& m, const glm::dvec3& a, const glm::dvec3& b)
{
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            const glm::dvec3 p = m.position(m.edgePoint(e, 0)), q = m.position(m.edgePoint(e, 1));
            if ((glm::distance(p, a) < 1e-6 && glm::distance(q, b) < 1e-6)
                || (glm::distance(p, b) < 1e-6 && glm::distance(q, a) < 1e-6))
                return e;
        }
    return kInvalid;
}

void checkClosed(const Mesh& m)
{
    CHECK(isValid(m));
    CHECK(boundaryEdgeCount(m) == 0);
    CHECK(euler(m) == 2);
}

bool isSymmetric(const Mesh& m, const SymmetryPlane& plane = {})
{
    const SymmetryMap map = SymmetryMap::build(m, plane, 1e-4);
    if (map.unmatchedCount())
        MESSAGE(map.unmatchedCount() << " points without a mirror");
    return map.unmatchedCount() == 0;
}

Index sumHard(const Mesh& m)
{
    std::span<const bool> hard = m.edgeAttrs().get<bool>(attr::kHard);
    Index n = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        n += m.edgeValid(e) && !hard.empty() && hard[e];
    return n;
}
} // namespace

TEST_CASE("ops: delete")
{
    Mesh cube = makeCube();
    deleteFaces(cube, std::vector<Index>{faceFacing(cube, {0, 1, 0})});
    CHECK(isValid(cube));
    CHECK(cube.numFaces() == 5);
    CHECK(boundaryEdgeCount(cube) == 4);

    Mesh c2 = makeCube();
    deleteEdges(c2, std::vector<Index>{0});
    CHECK(isValid(c2));
    CHECK(c2.numFaces() == 4);
    CHECK(c2.numEdges() == 11);

    Mesh c3 = makeCube();
    deletePoints(c3, std::vector<Index>{0});
    CHECK(isValid(c3));
    CHECK(c3.numFaces() == 3);
    CHECK(c3.numPoints() == 7);
}

TEST_CASE("ops: extrude")
{
    Mesh cube = makeCube();
    const Index top = faceFacing(cube, {0, 1, 0});
    ExtrudeOptions eo;
    eo.offset = 50.0;
    ExtrudeResult r = extrudeFaces(cube, std::vector<Index>{top}, eo);
    checkClosed(cube);
    CHECK(r.sides.size() == 4);
    CHECK(cube.numFaces() == 10);
    CHECK(cube.bounds().max.y == doctest::Approx(100.0));
    CHECK(signedVolume(cube) == doctest::Approx(1.5e6));
    // Cube edges were hard, so are the new ones around the extrusion.
    CHECK(sumHard(cube) == 20);

    // Region of two neighbouring faces.
    Mesh grid = makeCube({100, 100, 100, 2, 1, 1});
    std::vector<Index> both;
    for (Index f = 0; f < grid.faceCapacity(); ++f)
        if (grid.faceNormal(f).y > 0.9)
            both.push_back(f);
    REQUIRE(both.size() == 2);
    r = extrudeFaces(grid, both, eo);
    checkClosed(grid);
    CHECK(r.sides.size() == 6);
    CHECK(signedVolume(grid) == doctest::Approx(1.5e6));

    Mesh ind = makeCube({100, 100, 100, 2, 1, 1});
    eo.individual = true;
    r = extrudeFaces(ind, both, eo);
    checkClosed(ind);
    CHECK(r.sides.size() == 8);
}

TEST_CASE("ops: inset")
{
    Mesh cube = makeCube();
    const Index top = faceFacing(cube, {0, 1, 0});
    InsetOptions io;
    io.thickness = 10.0;
    ExtrudeResult r = insetFaces(cube, std::vector<Index>{top}, io);
    checkClosed(cube);
    CHECK(cube.numFaces() == 10);
    CHECK(cube.faceArea(top) == doctest::Approx(80.0 * 80.0));
    CHECK(signedVolume(cube) == doctest::Approx(1e6));

    Mesh c2 = makeCube();
    io.depth = -10.0;
    insetFaces(c2, std::vector<Index>{faceFacing(c2, {0, 1, 0})}, io);
    checkClosed(c2);
    // The rim slopes down to the inner face: a frustum comes out.
    CHECK(signedVolume(c2) == doctest::Approx(1e6 - 10.0 / 3.0 * (10000.0 + 6400.0 + 8000.0)));
}

TEST_CASE("ops: bevel")
{
    Mesh cube = makeCube();
    const Index e = edgeBetween(cube, {50, 50, 50}, {50, 50, -50});
    REQUIRE(e != kInvalid);
    BevelOptions bo;
    bo.offset = 10.0;
    std::vector<Index> made = bevelEdges(cube, std::vector<Index>{e}, bo);
    checkClosed(cube);
    CHECK(made.size() == 1);
    CHECK(cube.numFaces() == 7);
    CHECK(signedVolume(cube) == doctest::Approx(1e6 - 0.5 * 10 * 10 * 100));

    Mesh all = makeCube();
    std::vector<Index> edges;
    for (Index i = 0; i < all.edgeCapacity(); ++i)
        edges.push_back(i);
    made = bevelEdges(all, edges, bo);
    checkClosed(all);
    CHECK(all.numFaces() == 26);
    CHECK(made.size() == 20);
    // At a 30 degree smoothing angle a chamfer is faceted everywhere.
    CHECK(sumHard(all) == all.numEdges());

    Mesh round = makeCube();
    bo.segments = 3;
    made = bevelEdges(round, edges, bo);
    checkClosed(round);
    // Odd segment counts close each corner with three 1 x 2 quad grids around a centre triangle.
    CHECK(round.numFaces() == 6 + 12 * 3 + 8 * 7);
    // Rounded edges stay soft between segments, hard against the flat sides at 30 degrees.
    CHECK(signedVolume(round) < 1e6);
    CHECK(signedVolume(round) > 1e6 - 12 * 0.5 * 10 * 10 * 80 - 8 * 1000);

    for (int seg : {2, 4})
    {
        Mesh even = makeCube();
        bo.segments = seg;
        bevelEdges(even, edges, bo);
        checkClosed(even);
        // Even counts make three quad grids per corner.
        CHECK(even.numFaces() == Index(6 + 12 * seg + 8 * 3 * (seg / 2) * (seg / 2)));
        for (Index f = 0; f < even.faceCapacity(); ++f)
            if (even.faceValid(f))
                CHECK(even.faceSize(f) == 4);
        // Every point of the round corner stays near the sphere of radius 10 around (40, 40, 40).
        for (Index p = 0; p < even.pointCapacity(); ++p)
        {
            const glm::dvec3 q = even.position(p);
            if (even.pointValid(p) && q.x > 40.0 && q.y > 40.0 && q.z > 40.0)
                CHECK(glm::distance(q, glm::dvec3(40.0)) == doctest::Approx(10.0).epsilon(0.1));
        }
    }

    Mesh single = makeCube();
    bo.segments = 4;
    made = bevelEdges(single, std::vector<Index>{edgeBetween(single, {50, 50, 50}, {50, 50, -50})}, bo);
    checkClosed(single);
    CHECK(made.size() == 4);
    CHECK(single.numFaces() == 6 + 4);
    // A round profile: every point of the strip lies on a cylinder of radius offset around x=y=40.
    for (Index f : made)
        single.forEachFaceCorner(f, [&](Index c) {
            const glm::dvec3 p = single.position(single.cornerPoint(c));
            CHECK(std::hypot(p.x - 40.0, p.y - 40.0) == doctest::Approx(10.0));
        });
    // Soft between segments and against the sides (22.5 degrees), hard against the end caps.
    CHECK(sumHard(single) == 11 + 2 * 4);

    // Two edges meeting at a corner.
    Mesh two = makeCube();
    std::vector<Index> pair = {edgeBetween(two, {50, 50, 50}, {50, 50, -50}), edgeBetween(two, {50, 50, 50}, {-50, 50, 50})};
    bo.segments = 2;
    made = bevelEdges(two, pair, bo);
    checkClosed(two);

    // Open mesh, edge reaching the border.
    Mesh plane = makePlane({100, 100, 2, 2});
    const Index mid = edgeBetween(plane, {0, 0, 50}, {0, 0, 0});
    REQUIRE(mid != kInvalid);
    bo.segments = 1;
    made = bevelEdges(plane, std::vector<Index>{mid}, bo);
    CHECK(isValid(plane));
}

TEST_CASE("ops: edge loops")
{
    Mesh cube = makeCube();
    std::vector<Index> loop = insertEdgeLoop(cube, 0);
    checkClosed(cube);
    CHECK(loop.size() == 4);
    CHECK(cube.numFaces() == 10);

    Mesh c3 = makeCube();
    EdgeLoopOptions lo;
    lo.count = 3;
    loop = insertEdgeLoop(c3, 0, lo);
    checkClosed(c3);
    CHECK(loop.size() == 12);
    CHECK(c3.numFaces() == 18);

    Mesh plane = makePlane({100, 100, 3, 1});
    lo.count = 1;
    lo.fraction = 0.25;
    loop = insertEdgeLoop(plane, edgeBetween(plane, {-50, 0, 50}, {-50, 0, -50}), lo);
    CHECK(isValid(plane));
    CHECK(loop.size() == 3);
    CHECK(plane.numFaces() == 6);
}

TEST_CASE("ops: merge, collapse, dissolve")
{
    Mesh a = makePlane({100, 100, 1, 1});
    Mesh b = makePlane({100, 100, 1, 1});
    for (glm::dvec3& p : b.positions())
        p.x += 100.0;
    a.append(b);
    CHECK(a.numPoints() == 8);
    std::vector<Index> survivors = mergeByDistance(a, {}, {0.01});
    CHECK(isValid(a));
    CHECK(a.numPoints() == 6);
    CHECK(a.numFaces() == 2);
    CHECK(boundaryEdgeCount(a) == 6);

    Mesh cube = makeCube();
    std::vector<Index> pts = collapseEdges(cube, std::vector<Index>{0});
    CHECK(pts.size() == 1);
    checkClosed(cube);
    CHECK(cube.numPoints() == 7);

    Mesh m2 = makeCube();
    CHECK(mergeToCenter(m2, std::vector<Index>{0, 1}) != kInvalid);
    CHECK(isValid(m2));
    Mesh m3 = makeCube();
    CHECK(mergeToTarget(m3, std::vector<Index>{1}, 0) == 0);
    CHECK(isValid(m3));

    Mesh grid = makePlane({100, 100, 2, 1});
    std::vector<Index> faces = dissolveEdges(grid, std::vector<Index>{edgeBetween(grid, {0, 0, 50}, {0, 0, -50})});
    CHECK(isValid(grid));
    CHECK(grid.numFaces() == 1);
    CHECK(grid.numPoints() == 4);

    Mesh g2 = makePlane({100, 100, 2, 2});
    Index center = kInvalid;
    for (Index p = 0; p < g2.pointCapacity(); ++p)
        if (glm::length(g2.position(p)) < 1e-9)
            center = p;
    faces = dissolvePoints(g2, std::vector<Index>{center});
    CHECK(isValid(g2));
    REQUIRE(faces.size() == 1);
    CHECK(g2.faceSize(faces[0]) == 8);
    CHECK(g2.faceNormal(faces[0]).y == doctest::Approx(1.0));
}

TEST_CASE("ops: bridge")
{
    Mesh tube = makeCylinder({50.0, 300.0, 12, 3, 0, false});
    std::vector<Index> middle;
    for (Index f = 0; f < tube.faceCapacity(); ++f)
        if (std::abs(tube.faceCentroid(f).y) < 1.0)
            middle.push_back(f);
    REQUIRE(middle.size() == 12);
    deleteFaces(tube, middle);
    CHECK(boundaryEdgeCount(tube) == 48);
    std::vector<Index> inner;
    for (Index e = 0; e < tube.edgeCapacity(); ++e)
        if (tube.edgeValid(e) && tube.isBoundaryEdge(e)
            && std::abs(tube.position(tube.edgePoint(e, 0)).y) < 60.0)
            inner.push_back(e);
    REQUIRE(inner.size() == 24);
    BridgeOptions bo;
    bo.divisions = 1;
    std::vector<Index> made = bridgeEdgeLoops(tube, inner, bo);
    CHECK(made.size() == 24);
    CHECK(isValid(tube));
    CHECK(boundaryEdgeCount(tube) == 24);
}

TEST_CASE("ops: poke, spin, fill, flip, conform, triangulate, quadrangulate")
{
    Mesh cube = makeCube();
    std::vector<Index> made = pokeFaces(cube, std::vector<Index>{faceFacing(cube, {0, 1, 0})});
    CHECK(made.size() == 4);
    checkClosed(cube);

    Mesh grid = makePlane({100, 100, 2, 2});
    const Index mid = edgeBetween(grid, {0, 0, 0}, {0, 0, 50});
    std::vector<Index> spun = spinEdges(grid, std::vector<Index>{mid});
    REQUIRE(spun.size() == 1);
    CHECK(isValid(grid));
    CHECK(grid.numFaces() == 4);

    Mesh open = makeCube();
    deleteFaces(open, std::vector<Index>{2});
    made = fillHoles(open, {});
    CHECK(made.size() == 1);
    checkClosed(open);
    CHECK(signedVolume(open) == doctest::Approx(1e6));

    Mesh sphere = makeSphere();
    flipNormals(sphere, std::vector<Index>{3, 50, 51, 52, 200});
    CHECK_FALSE(sphere.validate(kValidateWinding).empty());
    conformNormals(sphere);
    checkClosed(sphere);
    CHECK(signedVolume(sphere) > 0.0);
    std::vector<Index> all;
    for (Index f = 0; f < sphere.faceCapacity(); ++f)
        all.push_back(f);
    flipNormals(sphere, all);
    conformNormals(sphere);
    CHECK(signedVolume(sphere) > 0.0);

    Mesh tri = makeCube();
    all.clear();
    for (Index f = 0; f < tri.faceCapacity(); ++f)
        all.push_back(f);
    made = triangulateFaces(tri, all);
    CHECK(made.size() == 12);
    checkClosed(tri);
    CHECK(sumHard(tri) == 12);
    made = quadrangulateFaces(tri, made);
    CHECK(made.size() == 6);
    checkClosed(tri);
    tri.compact();
    CHECK(uvsDontOverlap(tri));
}

TEST_CASE("ops: subdivide")
{
    Mesh cube = makeCube();
    Mesh s = subdivide(cube);
    checkClosed(s);
    CHECK(s.numFaces() == 24);
    CHECK(s.numPoints() == 26);
    CHECK(signedVolume(s) < 0.9e6);
    CHECK(uvsDontOverlap(s));
    Mesh s2 = subdivide(cube, {2});
    checkClosed(s2);
    CHECK(s2.numFaces() == 96);

    std::span<float> crease = cube.edgeAttrs().getOrAdd<float>(attr::kCrease);
    std::fill(crease.begin(), crease.end(), 2.0f);
    Mesh sharp = subdivide(cube, {2});
    CHECK(sharp.bounds().size().x == doctest::Approx(100.0));
    CHECK(signedVolume(sharp) == doctest::Approx(1e6));

    Mesh plane = makePlane();
    Mesh sp = subdivide(plane);
    CHECK(isValid(sp));
    CHECK(sp.bounds().size().x == doctest::Approx(100.0));
}

TEST_CASE("ops: mirror, separate, combine")
{
    Mesh sphere = makeSphere({50.0, 16, 8});
    const Index full = sphere.numPoints();
    std::vector<Index> left;
    for (Index f = 0; f < sphere.faceCapacity(); ++f)
        if (sphere.faceCentroid(f).x < 0.0)
            left.push_back(f);
    deleteFaces(sphere, left);
    std::vector<Index> made = mirrorMesh(sphere);
    sphere.compact();
    checkClosed(sphere);
    CHECK(sphere.numPoints() == full);
    CHECK(signedVolume(sphere) > 0.0);
    CHECK(isSymmetric(sphere));

    Mesh two = makeCube();
    Mesh other = makeTorus();
    two.append(other);
    std::vector<Mesh> parts = separate(two);
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].numFaces() == 6);
    CHECK(parts[1].numFaces() == other.numFaces());
    CHECK(isValid(parts[1]));
    const Mesh* list[2] = {&parts[0], &parts[1]};
    Mesh back = combine(list);
    CHECK(back.numFaces() == two.numFaces());
}

TEST_CASE("ops: mirror directions cut at the plane")
{
    auto shiftedCube = [] {
        Mesh m = makeCube();
        for (glm::dvec3& p : m.positions())
            p.x += 20.0;
        return m;
    };
    auto run = [&](MirrorDirection dir) {
        Mesh m = shiftedCube();
        MirrorOptions o;
        o.direction = dir;
        mirrorMesh(m, o);
        m.compact();
        return m;
    };

    Mesh pos = run(MirrorDirection::Positive);
    checkClosed(pos);
    CHECK(pos.numPoints() == 12);
    CHECK(pos.numFaces() == 10);
    CHECK(isSymmetric(pos));
    CHECK(pos.bounds().min.x == doctest::Approx(-70.0));
    CHECK(pos.bounds().max.x == doctest::Approx(70.0));
    CHECK(signedVolume(pos) == doctest::Approx(1.4e6));

    Mesh neg = run(MirrorDirection::Negative);
    checkClosed(neg);
    CHECK(neg.numPoints() == 12);
    CHECK(neg.numFaces() == 10);
    CHECK(isSymmetric(neg));
    CHECK(neg.bounds().max.x == doctest::Approx(30.0));
    CHECK(signedVolume(neg) == doctest::Approx(6e5));

    Mesh both = run(MirrorDirection::Both);
    CHECK(both.numPoints() == 16);
    CHECK(both.numFaces() == 12);
    CHECK(both.bounds().min.x == doctest::Approx(-70.0));

    // Other axes, and a plane given in another space: world x = 0 is object x = -20 here.
    Mesh y = shiftedCube();
    MirrorOptions oy;
    oy.plane = axisPlane(1);
    oy.direction = MirrorDirection::Negative;
    mirrorMesh(y, oy);
    y.compact();
    checkClosed(y);
    CHECK(y.numFaces() == 10);
    CHECK(y.bounds().max.y == doctest::Approx(50.0));
    Mesh w = makeCube();
    MirrorOptions ow;
    ow.space = glm::translate(glm::dmat4(1.0), glm::dvec3(20.0, 0.0, 0.0));
    ow.direction = MirrorDirection::Positive;
    mirrorMesh(w, ow);
    w.compact();
    checkClosed(w);
    CHECK(w.bounds().min.x == doctest::Approx(-90.0));
    CHECK(w.bounds().max.x == doctest::Approx(50.0));
    CHECK(isSymmetric(w, {glm::dvec3(1.0, 0.0, 0.0), -20.0}));
}

TEST_CASE("ops: mirror cut splits crossing faces and interpolates uvs")
{
    Mesh quad = makePlane();
    for (glm::dvec3& p : quad.positions())
        p.x += 10.0;
    MirrorOptions o;
    mirrorMesh(quad, o);
    quad.compact();
    CHECK(isValid(quad));
    CHECK(quad.numFaces() == 2);
    CHECK(quad.numPoints() == 8);
    o.direction = MirrorDirection::Positive;
    quad = makePlane();
    for (glm::dvec3& p : quad.positions())
        p.x += 10.0;
    mirrorMesh(quad, o);
    quad.compact();
    CHECK(isValid(quad));
    CHECK(quad.numFaces() == 2);
    CHECK(quad.numPoints() == 6);
    for (Index f = 0; f < quad.faceCapacity(); ++f)
        CHECK(quad.faceSize(f) == 4);
    std::span<const glm::vec2> uv = quad.cornerAttrs().get<glm::vec2>(attr::uvName(0));
    REQUIRE(!uv.empty());
    // The cut runs 40% of the way across the 100 wide plane.
    for (Index c = 0; c < quad.cornerCapacity(); ++c)
        if (quad.cornerValid(c) && std::abs(quad.position(quad.cornerPoint(c)).x) < 1e-9)
            CHECK(std::min(std::abs(uv[c].x - 0.4f), std::abs(uv[c].x - 0.6f)) < 1e-5f);

    // A C shape whose two prongs reach across the plane: one face crossing it four times.
    auto cShape = [] {
        Mesh m;
        const double xy[8][2] = {{-5, -10}, {10, -10}, {10, -6}, {-2, -6}, {-2, 6}, {10, 6}, {10, 10}, {-5, 10}};
        std::vector<Index> pts;
        for (const auto& p : xy)
            pts.push_back(m.addPoint({p[0], p[1], 0.0}));
        m.addFace(pts);
        return m;
    };
    auto area = [](const Mesh& m) {
        double a = 0.0;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                a += m.faceArea(f);
        return a;
    };
    Mesh prongs = cShape();
    mirrorMesh(prongs, o);
    prongs.compact();
    CHECK(isValid(prongs));
    CHECK(prongs.numFaces() == 4);
    CHECK(prongs.numPoints() == 12);
    CHECK(area(prongs) == doctest::Approx(160.0));
    CHECK(isSymmetric(prongs));

    Mesh back = cShape();
    o.direction = MirrorDirection::Negative;
    mirrorMesh(back, o);
    back.compact();
    CHECK(isValid(back));
    CHECK(back.numFaces() == 2);
    CHECK(area(back) == doctest::Approx(152.0));
    CHECK(isSymmetric(back));
}

TEST_CASE("symmetry: maps")
{
    Mesh sphere = makeSphere({50.0, 16, 8});
    SymmetryMap map = SymmetryMap::build(sphere);
    CHECK(map.unmatchedCount() == 0);
    Index onPlane = 0;
    for (Index p = 0; p < sphere.pointCapacity(); ++p)
        onPlane += map.onPlane(p);
    CHECK(onPlane == 2 * 7 + 2);

    // Topological match from an edge on the plane.
    Index seed = kInvalid;
    for (Index e = 0; e < sphere.edgeCapacity() && seed == kInvalid; ++e)
        if (map.onPlane(sphere.edgePoint(e, 0)) && map.onPlane(sphere.edgePoint(e, 1)))
            seed = e;
    REQUIRE(seed != kInvalid);
    SymmetryMap topo = SymmetryMap::buildTopological(sphere, seed);
    REQUIRE_FALSE(topo.empty());
    for (Index p = 0; p < sphere.pointCapacity(); ++p)
        CHECK(topo.mirrorPoint(p) == map.mirrorPoint(p));

    const Index f = 5;
    const Index mf = map.mirrorFace(sphere, f);
    REQUIRE(mf != kInvalid);
    CHECK(sphere.faceCentroid(mf).x == doctest::Approx(-sphere.faceCentroid(f).x));
}

TEST_CASE("symmetry: ops stay symmetric")
{
    // Even subdivisions put a column of points on the plane.
    const CubeOptions co{100, 100, 100, 4, 4, 4};
    Mesh m = makeCube(co);
    SymmetryMap map = SymmetryMap::build(m);
    REQUIRE(map.unmatchedCount() == 0);
    std::vector<Index> sel;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        const glm::dvec3 c = m.faceCentroid(f);
        if (m.faceNormal(f).y > 0.9 && c.x > 0.0 && c.x < 25.0 && c.z > 0.0 && c.z < 25.0)
            sel.push_back(f);
    }
    REQUIRE(sel.size() == 1);
    ExtrudeOptions eo;
    eo.offset = 20.0;
    eo.symmetry = &map;
    ExtrudeResult r = extrudeFaces(m, sel, eo);
    CHECK(r.faces.size() == 2);
    checkClosed(m);
    CHECK(isSymmetric(m));

    // Bevel an edge touching the plane.
    m.compact();
    map = SymmetryMap::build(m);
    REQUIRE(map.unmatchedCount() == 0);
    Index e = kInvalid;
    for (Index i = 0; i < m.edgeCapacity() && e == kInvalid; ++i)
    {
        const glm::dvec3 a = m.position(m.edgePoint(i, 0)), b = m.position(m.edgePoint(i, 1));
        if (std::abs(a.y + 50.0) < 1e-6 && std::abs(b.y + 50.0) < 1e-6 && std::abs(a.z - b.z) < 1e-6
            && std::min(a.x, b.x) > -1e-6 && std::min(a.x, b.x) < 1e-6 && std::abs(a.z) < 30.0 && a.z > 0)
            e = i;
    }
    REQUIRE(e != kInvalid);
    BevelOptions bo;
    bo.offset = 5.0;
    bo.segments = 2;
    bo.symmetry = &map;
    bevelEdges(m, std::vector<Index>{e}, bo);
    checkClosed(m);
    CHECK(isSymmetric(m));

    // Inset across the plane.
    m.compact();
    map = SymmetryMap::build(m);
    sel.clear();
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        const glm::dvec3 c = m.faceCentroid(f);
        if (m.faceNormal(f).z > 0.9 && c.x > 0.0 && c.x < 25.0 && c.y > 0.0 && c.y < 25.0)
            sel.push_back(f);
    }
    REQUIRE(sel.size() == 1);
    InsetOptions io;
    io.thickness = 3.0;
    io.depth = -5.0;
    io.symmetry = &map;
    insetFaces(m, sel, io);
    checkClosed(m);
    CHECK(isSymmetric(m));
}

TEST_CASE("ops: more coverage")
{
    // Closed loop around a cylinder's side.
    Mesh cyl = makeCylinder({50.0, 100.0, 16, 2});
    Index ring = kInvalid;
    for (Index e = 0; e < cyl.edgeCapacity() && ring == kInvalid; ++e)
        if (std::abs(cyl.position(cyl.edgePoint(e, 0)).y) < 1e-9 && std::abs(cyl.position(cyl.edgePoint(e, 1)).y) < 1e-9)
            ring = e;
    REQUIRE(ring != kInvalid);
    CHECK(edgeLoop(cyl, ring).size() == 16);

    // Extruding a whole open plane walls in its border.
    Mesh plane = makePlane({100, 100, 2, 2});
    std::vector<Index> all;
    for (Index f = 0; f < plane.faceCapacity(); ++f)
        all.push_back(f);
    ExtrudeOptions eo;
    eo.offset = 10.0;
    ExtrudeResult r = extrudeFaces(plane, all, eo);
    CHECK(isValid(plane));
    CHECK(r.sides.size() == 8);
    CHECK(boundaryEdgeCount(plane) == 8);
    CHECK(plane.bounds().max.y == doctest::Approx(10.0));

    // Individual inset keeps faces apart.
    Mesh grid = makePlane({100, 100, 2, 1});
    InsetOptions io;
    io.thickness = 5.0;
    io.individual = true;
    r = insetFaces(grid, std::vector<Index>{0, 1}, io);
    CHECK(isValid(grid));
    CHECK(r.sides.size() == 8);
    CHECK(grid.faceArea(0) == doctest::Approx(40.0 * 90.0));

    // Merge with symmetry welds both sides.
    Mesh a = makePlane({100, 100, 2, 2});
    Mesh b = a;
    // A gap under the merge distance; coincident points would make mirror matches ambiguous.
    for (glm::dvec3& p : b.positions())
        p.z -= 100.005;
    a.append(b);
    SymmetryMap map = SymmetryMap::build(a);
    REQUIRE(map.unmatchedCount() == 0);
    std::vector<Index> seam;
    for (Index p = 0; p < a.pointCapacity(); ++p)
        if (std::abs(a.position(p).z + 50.0) < 0.01 && a.position(p).x > 1.0)
            seam.push_back(p);
    REQUIRE(seam.size() == 2);
    CHECK(map.expandPoints(seam).size() == 4);
    MergeOptions mo;
    mo.distance = 0.01;
    mo.symmetry = &map;
    mergeByDistance(a, seam, mo);
    CHECK(isValid(a));
    CHECK(a.numPoints() == 18 - 2);
}

TEST_CASE("perf: ops on large meshes")
{
    Mesh m = makePlane({1000.0, 1000.0, 1000, 1000});
    std::vector<Index> some;
    for (Index f = 0; f < m.faceCapacity(); f += 100)
        some.push_back(f);
    auto t0 = std::chrono::steady_clock::now();
    ExtrudeOptions eo;
    eo.offset = 1.0;
    eo.individual = true;
    extrudeFaces(m, some, eo);
    const double tExtrude = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(m.numFaces() == 1000000 + 4 * 10000);

    Mesh q = makePlane({1000.0, 1000.0, 500, 500});
    t0 = std::chrono::steady_clock::now();
    Mesh s = subdivide(q);
    const double tSub = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(s.numFaces() == 1000000);

    Mesh b = makePlane({1000.0, 1000.0, 300, 300});
    std::vector<Index> edges;
    for (Index e = 0; e < b.edgeCapacity(); e += 7)
        if (!b.isBoundaryEdge(e))
            edges.push_back(e);
    t0 = std::chrono::steady_clock::now();
    BevelOptions bo;
    bo.offset = 0.5;
    bevelEdges(b, edges, bo);
    const double tBevel = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(isValid(b));
    MESSAGE("10k individual extrudes on 1M quads " << tExtrude << "s, subdivide 250k -> 1M quads " << tSub
                                                   << "s, bevel " << edges.size() << " edges " << tBevel << "s");
}
