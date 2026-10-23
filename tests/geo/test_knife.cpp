#include "TestUtil.h"

#include "geo/Knife.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
/** The edge whose midpoint is nearest p. */
Index edgeAt(const Mesh& m, const glm::dvec3& p)
{
    Index best = kInvalid;
    double bestD = 1e30;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            const double d = glm::distance((m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1))) * 0.5, p);
            if (d < bestD)
            {
                bestD = d;
                best = e;
            }
        }
    return best;
}

Index faceAt(const Mesh& m, const glm::dvec3& p)
{
    Index best = kInvalid;
    double bestD = 1e30;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && glm::distance(m.faceCentroid(f), p) < bestD)
        {
            bestD = glm::distance(m.faceCentroid(f), p);
            best = f;
        }
    return best;
}

KnifeOptions topView()
{
    KnifeOptions o;
    o.viewDir = {0, -1, 0};
    return o;
}

bool hasPointAt(const Mesh& m, const glm::dvec3& p)
{
    for (Index i = 0; i < m.pointCapacity(); ++i)
        if (m.pointValid(i) && glm::distance(m.position(i), p) < 1e-6)
            return true;
    return false;
}
} // namespace

TEST_CASE("knife: one face, edge to edge")
{
    Mesh m = makePlane({100, 100, 1, 1});
    const KnifePoint pts[] = {KnifePoint::edge(edgeAt(m, {-50, 0, 0}), 0.5), KnifePoint::edge(edgeAt(m, {50, 0, 0}), 0.5)};
    std::vector<Index> edges = knifeCut(m, pts, topView());
    CHECK(edges.size() == 1);
    CHECK(isValid(m));
    CHECK(m.numFaces() == 2);
    CHECK(m.numPoints() == 6);
    CHECK(boundaryEdgeCount(m) == 6);
    CHECK(hasPointAt(m, {-50, 0, 0}));
    CHECK(hasPointAt(m, {50, 0, 0}));
}

TEST_CASE("knife: across several faces splits every crossed edge")
{
    Mesh m = makePlane({100, 100, 3, 1});
    const KnifePoint pts[] = {KnifePoint::edge(edgeAt(m, {-50, 0, 0}), 0.5), KnifePoint::edge(edgeAt(m, {50, 0, 0}), 0.5)};
    std::vector<Index> edges = knifeCut(m, pts, topView());
    CHECK(edges.size() == 3);
    CHECK(isValid(m));
    CHECK(m.numFaces() == 6);
    CHECK(m.numPoints() == 12);
    // Every face is still a quad after a straight cut through quads.
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            CHECK(m.faceSize(f) == 4);
}

TEST_CASE("knife: vertex to vertex and a diagonal cut")
{
    Mesh m = makePlane({100, 100, 2, 2});
    Index a = kInvalid, b = kInvalid;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (glm::distance(m.position(p), {-50, 0, -50}) < 1e-6)
            a = p;
        if (glm::distance(m.position(p), {50, 0, 50}) < 1e-6)
            b = p;
    }
    REQUIRE(a != kInvalid);
    REQUIRE(b != kInvalid);
    const KnifePoint pts[] = {KnifePoint::point(a), KnifePoint::point(b)};
    std::vector<Index> edges = knifeCut(m, pts, topView());
    // Through the centre vertex: two diagonals, no new points.
    CHECK(edges.size() == 2);
    CHECK(isValid(m));
    CHECK(m.numPoints() == 9);
    CHECK(m.numFaces() == 6);
}

TEST_CASE("knife: points inside a face become a polyline through it")
{
    Mesh m = makePlane({100, 100, 1, 1});
    const KnifePoint pts[] = {KnifePoint::edge(edgeAt(m, {-50, 0, 0}), 0.5), KnifePoint::face(0, {-10, 0, 20}),
                              KnifePoint::face(0, {10, 0, -20}), KnifePoint::edge(edgeAt(m, {50, 0, 0}), 0.5)};
    std::vector<Index> edges = knifeCut(m, pts, topView());
    CHECK(edges.size() == 3);
    CHECK(isValid(m));
    CHECK(m.numFaces() == 2);
    CHECK(m.numPoints() == 8);
    CHECK(hasPointAt(m, {-10, 0, 20}));
    CHECK(hasPointAt(m, {10, 0, -20}));
}

TEST_CASE("knife: a path that starts inside a face cuts only from the first border it reaches")
{
    Mesh m = makePlane({90, 30, 3, 1});
    const Index start = faceAt(m, {-30, 0, 0});
    const KnifePoint pts[] = {KnifePoint::face(start, {-30, 0, 0}), KnifePoint::edge(edgeAt(m, {45, 0, 0}), 0.5)};
    knifeCut(m, pts, topView());
    CHECK(isValid(m));
    CHECK(m.numFaces() == 5);
    CHECK_FALSE(hasPointAt(m, {-30, 0, 0}));
}

TEST_CASE("knife: closed path around a cube and edge loop plan")
{
    Mesh cube = makeCube();
    const Index top = edgeAt(cube, {0, 50, 50});
    std::vector<Index> loop = knifeApply(cube, knifeLoopPlan(cube, top, 0.5));
    CHECK(loop.size() == 4);
    CHECK(isValid(cube));
    CHECK(boundaryEdgeCount(cube) == 0);
    CHECK(euler(cube) == 2);
    CHECK(cube.numFaces() == 10);

    Mesh c2 = makeCube();
    const KnifePoint pts[] = {KnifePoint::edge(edgeAt(c2, {0, 50, 50}), 0.5), KnifePoint::edge(edgeAt(c2, {0, 50, -50}), 0.5),
                              KnifePoint::edge(edgeAt(c2, {0, -50, -50}), 0.5), KnifePoint::edge(edgeAt(c2, {0, -50, 50}), 0.5)};
    KnifeOptions o;
    o.closed = true;
    std::vector<Index> ring = knifeCut(c2, pts, o);
    CHECK(ring.size() == 4);
    CHECK(isValid(c2));
    CHECK(euler(c2) == 2);
    CHECK(c2.numFaces() == 10);
    CHECK(c2.numPoints() == 12);
}

TEST_CASE("knife: symmetry cuts the mirror side too and keeps on-plane cuts single")
{
    Mesh m = makePlane({100, 100, 4, 1});
    SymmetryMap sym = SymmetryMap::build(m, axisPlane(0));
    REQUIRE(sym.unmatchedCount() == 0);
    KnifeOptions o = topView();
    o.symmetry = &sym;
    const KnifePoint side[] = {KnifePoint::edge(edgeAt(m, {37.5, 0, 50}), 0.3), KnifePoint::edge(edgeAt(m, {37.5, 0, -50}), 0.3)};
    knifeCut(m, side, o);
    CHECK(isValid(m));
    CHECK(m.numFaces() == 6);
    CHECK(m.numPoints() == 14);
    CHECK(SymmetryMap::build(m, axisPlane(0)).unmatchedCount() == 0);

    Mesh c = makePlane({100, 100, 4, 1});
    SymmetryMap csym = SymmetryMap::build(c, axisPlane(0));
    o.symmetry = &csym;
    const KnifePoint across[] = {KnifePoint::edge(edgeAt(c, {-50, 0, 0}), 0.5), KnifePoint::edge(edgeAt(c, {50, 0, 0}), 0.5)};
    knifeCut(c, across, o);
    CHECK(isValid(c));
    CHECK(c.numFaces() == 8);
    CHECK(c.numPoints() == 15);
}

TEST_CASE("knife: through cuts hidden faces too")
{
    auto stacked = [] {
        Mesh m = makePlane({100, 100, 1, 1});
        Mesh lower = makePlane({100, 100, 1, 1});
        for (glm::dvec3& p : lower.positions())
            p.y -= 10.0;
        m.append(lower);
        return m;
    };
    Mesh m = stacked();
    const KnifePoint pts[] = {KnifePoint::edge(edgeAt(m, {-50, 0, 0}), 0.5), KnifePoint::edge(edgeAt(m, {50, 0, 0}), 0.5)};
    knifeCut(m, pts, topView());
    CHECK(m.numFaces() == 3);

    Mesh t = stacked();
    KnifeOptions o = topView();
    o.through = true;
    knifeCut(t, pts, o);
    CHECK(isValid(t));
    CHECK(t.numFaces() == 4);
    CHECK(hasPointAt(t, {-50, -10, 0}));

    // Perspective from above gives the same cut on the top plane.
    Mesh p = stacked();
    KnifeOptions persp;
    persp.eye = glm::dvec3(0, 500, 0);
    knifeCut(p, pts, persp);
    CHECK(isValid(p));
    CHECK(p.numFaces() == 3);
}

TEST_CASE("knife: attributes follow and nothing happens without a path")
{
    Mesh m = makePlane({100, 100, 1, 1});
    KnifePlan none = knifePlan(m, std::vector<KnifePoint>{KnifePoint::face(0, {0, 0, 0})}, topView());
    CHECK(none.segments.empty());
    CHECK(knifeApply(m, none).empty());
    CHECK(m.numFaces() == 1);

    Mesh u = makePlane({100, 100, 1, 1});
    REQUIRE(u.cornerAttrs().has("uv0"));
    const KnifePoint pts[] = {KnifePoint::edge(edgeAt(u, {-50, 0, 0}), 0.5), KnifePoint::face(0, {0, 0, 0}),
                              KnifePoint::edge(edgeAt(u, {50, 0, 0}), 0.5)};
    knifeCut(u, pts, topView());
    CHECK(isValid(u));
    CHECK(uvsDontOverlap(u));
}

TEST_CASE("knife: perspective walk over a curved surface stays on the near side")
{
    Mesh m = makeSphere();
    const Index before = m.numFaces();
    const glm::dvec3 eye(0, 0, 500);
    const Index fa = faceAt(m, {-35, 5, 35}), fb = faceAt(m, {35, 5, 35});
    const KnifePoint pts[] = {KnifePoint::face(fa, m.faceCentroid(fa)), KnifePoint::face(fb, m.faceCentroid(fb))};
    KnifeOptions o;
    o.eye = eye;
    KnifePlan plan = knifePlan(m, pts, o);
    CHECK(plan.failed.empty());
    for (const KnifeSegment& s : plan.segments)
        CHECK(m.faceCentroid(s.face).z > 0.0);
    knifeApply(m, plan);
    CHECK(isValid(m));
    CHECK(euler(m) == 2);
    CHECK(m.numFaces() > before);
}
