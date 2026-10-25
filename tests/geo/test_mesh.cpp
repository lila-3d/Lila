#include "TestUtil.h"

#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "geo/Topology.h"

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
Mesh quadGrid(int nx, int ny)
{
    Mesh m;
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i)
            m.addPoint({double(i), 0.0, double(-j)});
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
        {
            const Index a = Index(j * (nx + 1) + i);
            m.addFace({a, a + 1, a + 1 + Index(nx + 1), a + Index(nx + 1)});
        }
    return m;
}
} // namespace

TEST_CASE("mesh: build and adjacency")
{
    Mesh m = quadGrid(3, 2);
    CHECK(m.numPoints() == 12);
    CHECK(m.numFaces() == 6);
    CHECK(m.numEdges() == 17);
    CHECK(isValid(m));
    CHECK(boundaryEdgeCount(m) == 10);
    CHECK(m.pointValence(5) == 4);
    CHECK_FALSE(m.isBoundaryPoint(5));
    CHECK(m.isBoundaryPoint(0));
    CHECK(m.pointFaces(5).size() == 4);
    const Index e = m.findEdge(5, 6);
    REQUIRE(e != kInvalid);
    CHECK(m.edgeFaceCount(e) == 2);
    CHECK(m.findEdge(0, 5) == kInvalid);
    CHECK(m.faceNormal(0).y == doctest::Approx(1.0));
}

TEST_CASE("mesh: non-manifold edge")
{
    Mesh m;
    for (int i = 0; i < 5; ++i)
        m.addPoint({double(i), double(i * i), 0.0});
    m.addFace({0, 1, 2});
    m.addFace({1, 0, 3});
    m.addFace({0, 1, 4});
    CHECK(m.edgeFaceCount(m.findEdge(0, 1)) == 3);
    CHECK(isValid(m, kValidateStructure));
    CHECK_FALSE(m.validate(kValidateManifold).empty());
}

TEST_CASE("mesh: remove, compact, attributes follow")
{
    Mesh m = quadGrid(3, 3);
    std::span<float> w = m.pointAttrs().getOrAdd<float>("weight");
    for (Index p = 0; p < m.pointCapacity(); ++p)
        w[p] = float(p);
    std::span<int32_t> id = m.faceAttrs().getOrAdd<int32_t>("id");
    for (Index f = 0; f < m.faceCapacity(); ++f)
        id[f] = int32_t(f);

    m.removeFace(4);
    CHECK(m.numFaces() == 8);
    CHECK(isValid(m));
    m.removePoint(0);
    CHECK(m.numFaces() == 7);
    m.removeAllLoose();
    CHECK(isValid(m));
    MeshRemap remap;
    m.compact(&remap);
    CHECK_FALSE(m.hasDeleted());
    CHECK(isValid(m));
    CHECK(remap.points[0] == kInvalid);
    std::span<float> w2 = m.pointAttrs().get<float>("weight");
    CHECK(w2[remap.points[5]] == 5.0f);
    std::span<int32_t> id2 = m.faceAttrs().get<int32_t>("id");
    CHECK(id2[remap.faces[8]] == 8);
    // Contiguous corners after compact.
    for (Index f = 0; f < m.faceCapacity(); ++f)
        CHECK(m.cornerNext(m.faceCorner(f)) == m.faceCorner(f) + 1);
}

TEST_CASE("mesh: euler operators")
{
    Mesh m = quadGrid(2, 1);
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        uv[c] = glm::vec2(m.position(m.cornerPoint(c)).x, -m.position(m.cornerPoint(c)).z);

    const Index e = m.findEdge(1, 4);
    const Index mid = m.splitEdge(e, 0.25);
    CHECK(isValid(m));
    CHECK(m.faceSize(0) == 5);
    CHECK(m.faceSize(1) == 5);
    CHECK(m.position(mid).z == doctest::Approx(-0.25));
    uv = m.cornerAttrs().get<glm::vec2>("uv0");
    m.forEachPointCorner(mid, [&](Index c) { CHECK(uv[c].y == doctest::Approx(0.25f)); });

    const Index c0 = m.findCorner(0, 0);
    const Index cm = m.findCorner(0, mid);
    const Index nf = m.splitFace(0, c0, cm);
    REQUIRE(nf != kInvalid);
    CHECK(isValid(m));
    CHECK(m.faceSize(0) + m.faceSize(nf) == 7);

    const Index cut = m.lastSplitEdge();
    const Index joined = m.joinFaces(cut);
    REQUIRE(joined != kInvalid);
    CHECK(isValid(m));
    CHECK(m.faceSize(joined) == 5);
    CHECK(m.numFaces() == 2);

    m.flipFace(1);
    CHECK_FALSE(m.validate(kValidateWinding).empty());
    m.flipFace(1);
    CHECK(isValid(m));
}

TEST_CASE("mesh: append and copy")
{
    Mesh a = makeCube();
    Mesh b = a;
    const Index offset = a.append(b);
    CHECK(offset == 8);
    CHECK(a.numFaces() == 12);
    CHECK(isValid(a));
    CHECK(b.numFaces() == 6);
    CHECK(a.bounds().size().x == doctest::Approx(100.0));
}

TEST_CASE("topology: loops, rings, boundaries, components")
{
    Mesh m = quadGrid(4, 3);
    // Interior horizontal edge between rows 1 and 2.
    const Index e = m.findEdge(6, 7);
    CHECK(edgeLoop(m, e).size() == 4);
    CHECK(edgeRing(m, e).size() == 4);
    const std::vector<EdgePath> loops = boundaryLoops(m);
    REQUIRE(loops.size() == 1);
    CHECK(loops[0].closed);
    CHECK(loops[0].edges.size() == 14);

    Mesh cube = makeCube();
    const Index ce = cube.edgeCorner(0) != kInvalid ? 0u : 1u;
    CHECK(edgeLoop(cube, ce).size() == 1); // valence-3 corners stop the loop
    CHECK(edgeRing(cube, ce).size() == 4);

    Mesh two = makeCube();
    two.append(makeSphere());
    std::vector<Index> comp;
    CHECK(faceComponents(two, comp) == 2);
}

namespace
{
bool loopClosed(const Mesh& m, const std::vector<Index>& loop)
{
    if (loop.size() < 3)
        return false;
    const Index a = loop.front(), b = loop.back();
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            if (m.edgePoint(a, i) == m.edgePoint(b, j))
                return true;
    return false;
}

bool horizontal(const Mesh& m, Index e)
{
    return std::abs(m.position(m.edgePoint(e, 0)).y - m.position(m.edgePoint(e, 1)).y) < 1e-6;
}

bool onSide(const Mesh& m, Index e, double radius)
{
    for (int i = 0; i < 2; ++i)
    {
        const glm::dvec3 q = m.position(m.edgePoint(e, i));
        if (std::abs(std::hypot(q.x, q.z) - radius) > 1e-6)
            return false;
    }
    return true;
}
} // namespace

TEST_CASE("topology: edge loops run through valence-4 points and stop at poles")
{
    SUBCASE("cylinder with n-gon caps: rims and rings close, verticals stop at the rims")
    {
        Mesh m = makeCylinder({.subdivAxis = 12, .subdivHeight = 3});
        for (Index e = 0; e < m.edgeCapacity(); ++e)
        {
            if (!m.edgeValid(e))
                continue;
            const std::vector<Index> loop = edgeLoop(m, e);
            if (horizontal(m, e))
            {
                CHECK(loop.size() == 12);
                CHECK(loopClosed(m, loop));
            }
            else
                CHECK(loop.size() == 3);
        }
    }
    SUBCASE("cylinder with fan caps")
    {
        Mesh m = makeCylinder({.subdivAxis = 12, .subdivHeight = 2, .subdivCaps = 1});
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e) && horizontal(m, e) && onSide(m, e, 50.0))
                CHECK(edgeLoop(m, e).size() == 12);
    }
    SUBCASE("torus: every loop closes")
    {
        Mesh m = makeTorus({.subdivAxis = 12, .subdivHeight = 8});
        for (Index e = 0; e < m.edgeCapacity(); ++e)
        {
            if (!m.edgeValid(e))
                continue;
            const std::vector<Index> loop = edgeLoop(m, e);
            CHECK((loop.size() == 12 || loop.size() == 8));
            CHECK(loopClosed(m, loop));
        }
    }
    SUBCASE("quad sphere: loops close or end on its valence-3 poles")
    {
        Mesh m = makeQuadSphere({.subdiv = 4});
        for (Index e = 0; e < m.edgeCapacity(); ++e)
        {
            if (!m.edgeValid(e))
                continue;
            const std::vector<Index> loop = edgeLoop(m, e);
            if (loopClosed(m, loop))
                CHECK(loop.size() == 16);
            else
            {
                // Each open end sits on a pole.
                Index endA = m.edgePoint(loop.front(), 0), endB = m.edgePoint(loop.back(), 0);
                if (loop.size() > 1)
                {
                    const Index f1 = loop[1], b1 = loop[loop.size() - 2];
                    endA = m.edgePoint(f1, 0) == endA || m.edgePoint(f1, 1) == endA ? m.edgePoint(loop.front(), 1) : endA;
                    endB = m.edgePoint(b1, 0) == endB || m.edgePoint(b1, 1) == endB ? m.edgePoint(loop.back(), 1) : endB;
                }
                CHECK(m.pointValence(endA) == 3);
                CHECK(m.pointValence(endB) == 3);
            }
        }
    }
    SUBCASE("UV sphere: meridians run pole to pole, parallels close")
    {
        Mesh m = makeSphere({.subdivAxis = 12, .subdivHeight = 8});
        for (Index e = 0; e < m.edgeCapacity(); ++e)
        {
            if (!m.edgeValid(e))
                continue;
            const std::vector<Index> loop = edgeLoop(m, e);
            if (horizontal(m, e))
            {
                CHECK(loop.size() == 12);
                CHECK(loopClosed(m, loop));
            }
            else
                CHECK(loop.size() == 8);
        }
    }
    SUBCASE("grid with mixed winding and its border")
    {
        Mesh m = quadGrid(4, 3);
        m.flipFace(1);
        m.flipFace(4);
        m.flipFace(6);
        CHECK(edgeLoop(m, m.findEdge(6, 7)).size() == 4);
        CHECK(edgeLoop(m, m.findEdge(1, 6)).size() == 3);
        const std::vector<Index> border = edgeLoop(m, m.findEdge(0, 1));
        CHECK(border.size() == 14);
        CHECK(loopClosed(m, border));
    }
    SUBCASE("border runs on past a pole sitting on it")
    {
        // Point 2 on the bottom border gets a second interior edge.
        Mesh m = quadGrid(4, 2);
        Index c2 = kInvalid, c6 = kInvalid;
        m.forEachFaceCorner(1, [&](Index c) {
            c2 = m.cornerPoint(c) == 2 ? c : c2;
            c6 = m.cornerPoint(c) == 6 ? c : c6;
        });
        m.splitFace(1, c2, c6);
        CHECK(m.pointValence(2) == 4);
        const std::vector<Index> border = edgeLoop(m, m.findEdge(0, 1));
        CHECK(border.size() == 12);
        CHECK(loopClosed(m, border));
    }
}
