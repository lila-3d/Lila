#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "uv/Uv.h"
#include "uv/UvEdit.h"

#include <doctest/doctest.h>

#include <cmath>
#include <set>

using namespace lila;
using geo::Index;

namespace
{
Index edgeBetween(const geo::Mesh& m, const glm::dvec3& a, const glm::dvec3& b)
{
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            const glm::dvec3 p = m.position(m.edgePoint(e, 0)), q = m.position(m.edgePoint(e, 1));
            if ((glm::distance(p, a) < 1e-6 && glm::distance(q, b) < 1e-6) || (glm::distance(p, b) < 1e-6 && glm::distance(q, a) < 1e-6))
                return e;
        }
    return geo::kInvalid;
}

size_t islands(const geo::Mesh& m, uv::IslandSource src)
{
    uv::ChartOptions o;
    o.source = src;
    return uv::islandFaces(m, {}, o).size();
}

bool isSeam(const geo::Mesh& m, Index e)
{
    std::span<const bool> s = m.edgeAttrs().get<bool>(geo::attr::kSeam);
    return e < s.size() && s[e];
}

double uvArea(const geo::Mesh& m, Index f)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    std::vector<glm::dvec2> p;
    m.forEachFaceCorner(f, [&](Index c) { p.push_back(glm::dvec2(uv[c])); });
    double a = 0.0;
    for (size_t i = 0; i < p.size(); ++i)
        a += p[i].x * p[(i + 1) % p.size()].y - p[(i + 1) % p.size()].x * p[i].y;
    return 0.5 * a;
}

/** A 100 cm cube, box projected, its top sewn to the four sides: two islands (top with sides, bottom). */
geo::Mesh sewnCube()
{
    geo::Mesh m = geo::makeCube();
    uv::ProjectOptions po;
    po.type = uv::Projection::Box;
    REQUIRE(uv::project(m, {}, "uv0", po));
    std::vector<Index> top;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.position(m.edgePoint(e, 0)).y > 49 && m.position(m.edgePoint(e, 1)).y > 49)
            top.push_back(e);
    REQUIRE(top.size() == 4);
    for (Index e : top)
        uv::sewUvEdges(m, std::span<const Index>(&e, 1), "uv0", true);
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            REQUIRE(std::abs(uvArea(m, f)) > 1e-4);
    return m;
}

/** Seams among the strip's long edges (the ones along the bevelled edge). */
int longSeams(const geo::Mesh& m, std::span<const Index> faces)
{
    std::set<Index> edges;
    for (Index f : faces)
        m.forEachFaceCorner(f, [&](Index c) {
            if (m.edgeLength(m.cornerEdge(c)) > 50.0 && isSeam(m, m.cornerEdge(c)))
                edges.insert(m.cornerEdge(c));
        });
    return int(edges.size());
}

void checkStripUvs(const geo::Mesh& m, std::span<const Index> faces)
{
    for (Index f : faces)
    {
        CAPTURE(f);
        CHECK(std::abs(uvArea(m, f)) > 1e-7);
    }
}
} // namespace

TEST_CASE("bevel keeps uv islands: strips sewn to both sides, one seam where the edge was one")
{
    for (int segments : {1, 3})
    {
        CAPTURE(segments);
        geo::Mesh base = sewnCube();
        const size_t seamIslands = islands(base, uv::IslandSource::Seams);
        REQUIRE(seamIslands == 2);
        REQUIRE(islands(base, uv::IslandSource::Uvs) == 2);

        geo::BevelOptions o;
        o.segments = segments;
        o.offset = 5.0;
        // A sewn edge (top to front) and a seam (between two sides).
        const Index sewn = edgeBetween(base, {-50, 50, 50}, {50, 50, 50});
        const Index cut = edgeBetween(base, {50, -50, 50}, {50, 50, 50});
        REQUIRE(sewn != geo::kInvalid);
        REQUIRE(cut != geo::kInvalid);
        REQUIRE(!isSeam(base, sewn));
        REQUIRE(isSeam(base, cut));

        {
            geo::Mesh m = base;
            const Index e[1] = {sewn};
            const std::vector<Index> strip = geo::bevelEdges(m, e, o);
            REQUIRE(strip.size() == size_t(segments));
            CHECK(longSeams(m, strip) == 0);
            CHECK(islands(m, uv::IslandSource::Seams) == seamIslands);
            CHECK(islands(m, uv::IslandSource::Uvs) == seamIslands);
            checkStripUvs(m, strip);
        }
        {
            geo::Mesh m = base;
            const Index e[1] = {cut};
            const std::vector<Index> strip = geo::bevelEdges(m, e, o);
            REQUIRE(strip.size() == size_t(segments));
            CHECK(longSeams(m, strip) == 1);
            CHECK(islands(m, uv::IslandSource::Seams) == seamIslands);
            CHECK(islands(m, uv::IslandSource::Uvs) == seamIslands);
            checkStripUvs(m, strip);
        }
        {
            // Every edge, as the auto bevel takes them on a box: corners included.
            geo::Mesh m = base;
            std::vector<Index> all;
            for (Index e = 0; e < m.edgeCapacity(); ++e)
                if (m.edgeValid(e))
                    all.push_back(e);
            const std::vector<Index> made = geo::bevelEdges(m, all, o);
            REQUIRE(!made.empty());
            CHECK(islands(m, uv::IslandSource::Seams) == seamIslands);
            CHECK(islands(m, uv::IslandSource::Uvs) == seamIslands);
            checkStripUvs(m, made);

            geo::Mesh before = base;
            const uv::UnwrapFacesResult r0 = uv::unwrapFaces(before, {}, "uv0");
            const uv::UnwrapFacesResult r1 = uv::unwrapFaces(m, {}, "uv0");
            CHECK(r1.islands == r0.islands);
            CHECK(r1.failed == 0);
            CHECK(islands(m, uv::IslandSource::Uvs) == r0.islands);
            checkStripUvs(m, made);
        }
    }
}

TEST_CASE("inset keeps uv islands and gives the ring faces uv area")
{
    geo::Mesh m = sewnCube();
    const size_t before = islands(m, uv::IslandSource::Seams);
    Index top = geo::kInvalid;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceNormal(f).y > 0.9)
            top = f;
    REQUIRE(top != geo::kInvalid);
    geo::InsetOptions o;
    o.thickness = 10.0;
    const Index sel[1] = {top};
    const geo::ExtrudeResult r = geo::insetFaces(m, sel, o);
    REQUIRE(r.sides.size() == 4);
    checkStripUvs(m, r.sides);
    CHECK(islands(m, uv::IslandSource::Seams) == before);
    CHECK(islands(m, uv::IslandSource::Uvs) == before);
}
