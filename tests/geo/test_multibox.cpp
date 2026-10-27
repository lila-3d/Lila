#include "TestUtil.h"

#include "geo/MultiBox.h"
#include "geo/Primitives.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace lila::geo;

namespace
{
Mesh prism(const std::vector<glm::dvec2>& poly, double height)
{
    std::vector<glm::dvec3> pts;
    const Index n = Index(poly.size());
    for (double z : {0.0, height})
        for (const glm::dvec2& p : poly)
            pts.emplace_back(p.x, p.y, z);
    std::vector<Index> sizes, idx;
    sizes.push_back(n);
    for (Index i = n; i-- > 0;)
        idx.push_back(i);
    sizes.push_back(n);
    for (Index i = 0; i < n; ++i)
        idx.push_back(n + i);
    for (Index i = 0; i < n; ++i)
    {
        Index j = (i + 1) % n;
        sizes.push_back(4);
        idx.insert(idx.end(), {i, j, n + j, n + i});
    }
    return Mesh::fromPolygons(pts, sizes, idx);
}

Mesh combine(const std::vector<Mesh>& parts)
{
    std::vector<glm::dvec3> pts;
    std::vector<Index> sizes, idx;
    for (const Mesh& m : parts)
    {
        std::vector<Index> remap(m.pointCapacity(), 0);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                remap[p] = Index(pts.size()), pts.push_back(m.position(p));
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
            {
                std::vector<Index> fp = m.facePoints(f);
                sizes.push_back(Index(fp.size()));
                for (Index p : fp)
                    idx.push_back(remap[p]);
            }
    }
    return Mesh::fromPolygons(pts, sizes, idx);
}

void shift(Mesh& m, const glm::dvec3& d)
{
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, m.position(p) + d);
}

double hullVolume(const Mesh& m)
{
    double sum = 0.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            std::vector<Index> fp = m.facePoints(f);
            for (size_t i = 1; i + 1 < fp.size(); ++i)
                sum += glm::dot(m.position(fp[0]), glm::cross(m.position(fp[i]), m.position(fp[i + 1])));
        }
    return std::abs(sum) / 6.0;
}

Mesh lShape()
{
    return prism({{0, 0}, {100, 0}, {100, 20}, {20, 20}, {20, 100}, {0, 100}}, 20.0);
}

bool insideSome(const MultiBoxResult& r, const glm::dvec3& p, double tol)
{
    for (const BoxChunk& c : r.chunks)
    {
        bool in = true;
        for (int i = 0; i < 3; ++i)
            in = in && std::abs(glm::dot(p - c.box.center, c.box.axes[i])) <= c.box.size[i] * 0.5 + tol;
        if (in)
            return true;
    }
    return false;
}
} // namespace

TEST_CASE("multibox: an L prism is two full boxes")
{
    MultiBoxResult r = multiBoxDecompose(lShape());
    REQUIRE(r.chunks.size() == 2);
    for (const BoxChunk& c : r.chunks)
        CHECK(c.fill > 0.95);
    CHECK(r.averageFill > 0.95);
    CHECK(r.coverage > 0.99);
}

TEST_CASE("multibox: a cube is one box")
{
    MultiBoxResult r = multiBoxDecompose(makeCube({100.0, 40.0, 60.0}));
    REQUIRE(r.chunks.size() == 1);
    CHECK(r.chunks[0].fill > 0.95);
    CHECK(r.chunks[0].volume == doctest::Approx(100.0 * 40.0 * 60.0).epsilon(0.02));
}

TEST_CASE("multibox: a rotated L is still two boxes when oriented")
{
    Mesh m = lShape();
    glm::dmat4 xf = glm::rotate(glm::dmat4(1.0), 0.6, glm::normalize(glm::dvec3(0.3, 1.0, 0.5)));
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, glm::dvec3(xf * glm::dvec4(m.position(p), 1.0)));
    MultiBoxResult r = multiBoxDecompose(m);
    CHECK(r.chunks.size() == 2);
    for (const BoxChunk& c : r.chunks)
        CHECK(c.fill > 0.85);
}

TEST_CASE("multibox: max boxes is respected and every vertex is covered")
{
    Mesh m = makeSphere({50.0, 24, 12});
    for (int limit : {1, 2, 5})
    {
        MultiBoxOptions o;
        o.maxBoxes = limit;
        o.minFill = 0.95;
        MultiBoxResult r = multiBoxDecompose(m, o);
        CHECK(int(r.chunks.size()) <= limit);
        CHECK(!r.chunks.empty());
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                CHECK(insideSome(r, m.position(p), 1e-6));
    }
    Mesh l = lShape();
    MultiBoxOptions o;
    o.oriented = false;
    o.padding = 2.0;
    MultiBoxResult r = multiBoxDecompose(l, o);
    for (Index p = 0; p < l.pointCapacity(); ++p)
        if (l.pointValid(p))
            CHECK(insideSome(r, l.position(p), 1e-6));
}

TEST_CASE("multibox: hull chunks and determinism")
{
    Mesh m = makeSphere({50.0, 24, 12});
    MultiBoxOptions o;
    o.shape = "hull";
    MultiBoxResult a = multiBoxDecompose(m, o), b = multiBoxDecompose(m, o);
    REQUIRE(a.chunks.size() == b.chunks.size());
    size_t hulls = 0;
    for (size_t i = 0; i < a.chunks.size(); ++i)
    {
        hulls += a.chunks[i].hull;
        CHECK(a.chunks[i].hull == (a.chunks[i].hullMesh.numFaces() > 0));
        CHECK(a.chunks[i].box.center == b.chunks[i].box.center);
        CHECK(a.chunks[i].box.size == b.chunks[i].box.size);
        CHECK(a.chunks[i].fill == b.chunks[i].fill);
    }
    CHECK(hulls > 0);
    CHECK(a.coverage == b.coverage);
}

TEST_CASE("multibox: overlapping closed shells and open meshes are filled")
{
    Mesh a = makeCube({60.0, 60.0, 60.0});
    Mesh b = makeCube({60.0, 60.0, 60.0});
    for (Index p = 0; p < b.pointCapacity(); ++p)
        if (b.pointValid(p))
            b.setPosition(p, b.position(p) + glm::dvec3(30.0, 0.0, 0.0));
    std::vector<glm::dvec3> pts;
    std::vector<Index> sizes, idx;
    for (const Mesh* m : {&a, &b})
    {
        Index base = Index(pts.size());
        std::vector<Index> remap(m->pointCapacity(), 0);
        for (Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                remap[p] = Index(pts.size()), pts.push_back(m->position(p));
        (void)base;
        for (Index f = 0; f < m->faceCapacity(); ++f)
            if (m->faceValid(f))
            {
                std::vector<Index> fp = m->facePoints(f);
                sizes.push_back(Index(fp.size()));
                for (Index p : fp)
                    idx.push_back(remap[p]);
            }
    }
    Mesh both = Mesh::fromPolygons(pts, sizes, idx);
    MultiBoxResult r = multiBoxDecompose(both);
    REQUIRE(r.chunks.size() == 1);
    CHECK(r.chunks[0].fill > 0.95);
}

TEST_CASE("multibox: a wedge base with a thin bar on its ridge is a hull and a box")
{
    Mesh base = prism({{-50, 0}, {50, 0}, {20, 30}, {-20, 30}}, 200.0);
    Mesh bar = prism({{-4, 30}, {4, 30}, {4, 38}, {-4, 38}}, 180.0);
    shift(bar, {0, 0, 10});
    Mesh ship = combine({base, bar});
    MultiBoxOptions o;
    o.maxBoxes = 2;
    o.shape = "auto";
    MultiBoxResult r = multiBoxDecompose(ship, o);
    REQUIRE(r.chunks.size() == 2);
    const BoxChunk *wedge = &r.chunks[0], *thin = &r.chunks[1];
    if (wedge->box.size.x * wedge->box.size.z < thin->box.size.x * thin->box.size.z)
        std::swap(wedge, thin);
    CHECK(thin->fill > 0.9);
    CHECK(thin->box.size.y < 15.0);
    CHECK(wedge->hull);
    CHECK(hullVolume(wedge->hullMesh) == doctest::Approx(2100.0 * 200.0).epsilon(0.1));
    // The two shapes meet without one swallowing the other.
    double lo = wedge->box.center.y - wedge->box.size.y * 0.5, hi = wedge->box.center.y + wedge->box.size.y * 0.5;
    CHECK(thin->box.center.y - thin->box.size.y * 0.5 > hi - 6.0);
    CHECK(lo < 3.0);
}

TEST_CASE("multibox: a tapered flat base with a thin bar thinner than a voxel")
{
    std::vector<glm::dvec3> corners = {{-60, 0, -150}, {60, 0, -150}, {30, 0, 150}, {-30, 0, 150},
                                       {-40, 25, -130}, {40, 25, -130}, {15, 25, 130}, {-15, 25, 130}};
    Mesh base = convexHullMesh(corners, 32);
    Mesh bar = makeCube({5.0, 5.0, 260.0});
    shift(bar, {0, 27.5, 0});
    MultiBoxOptions o;
    o.maxBoxes = 2;
    o.shape = "auto";
    MultiBoxResult r = multiBoxDecompose(combine({base, bar}), o);
    REQUIRE(r.chunks.size() == 2);
    const BoxChunk *wedge = &r.chunks[0], *thin = &r.chunks[1];
    if (wedge->box.size.x * wedge->box.size.y < thin->box.size.x * thin->box.size.y)
        std::swap(wedge, thin);
    CHECK(thin->fill > 0.9);
    CHECK(thin->box.size.x < 15.0);
    CHECK(thin->box.size.y < 15.0);
    CHECK(wedge->hull);
    CHECK(hullVolume(wedge->hullMesh) == doctest::Approx(hullVolume(base)).epsilon(0.1));
    CHECK(thin->box.center.y - thin->box.size.y * 0.5 > 20.0);
}
