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
