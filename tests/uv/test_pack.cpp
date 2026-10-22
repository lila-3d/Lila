#include "geo/Mesh.h"
#include "uv/Uv.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <span>

using namespace lila;
using geo::Index;

namespace
{
double seconds(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

struct Rng
{
    uint32_t seed;
    double operator()()
    {
        seed = seed * 1664525u + 1013904223u;
        return double(seed >> 8) / double(1u << 24);
    }
};

/** Polygons in the xy plane, uvs equal to xy; every island lifted to its own z so the islands stay apart in 3D. */
struct SoupBuilder
{
    std::vector<glm::dvec3> pts;
    std::vector<Index> sizes, idx;
    double z = 0.0;

    Index point(glm::dvec2 p) { pts.emplace_back(p.x, p.y, z); return Index(pts.size() - 1); }
    void face(std::initializer_list<Index> f)
    {
        sizes.push_back(Index(f.size()));
        idx.insert(idx.end(), f);
    }
    void next() { z += 3.0; }

    void rect(glm::dvec2 o, double w, double h)
    {
        Index a = point(o), b = point(o + glm::dvec2(w, 0)), c = point(o + glm::dvec2(w, h)), d = point(o + glm::dvec2(0, h));
        face({a, b, c, d});
        next();
    }
    /** Annulus (or an arc of one) as a quad strip: a single island with a hole when closed. */
    void ring(glm::dvec2 c, double r0, double r1, int segs, double sweep = glm::two_pi<double>())
    {
        bool closed = sweep >= glm::two_pi<double>() - 1e-9;
        int n = closed ? segs : segs + 1;
        Index base = Index(pts.size());
        for (int i = 0; i < n; ++i)
        {
            double a = sweep * i / segs;
            glm::dvec2 d(std::cos(a), std::sin(a));
            point(c + d * r0);
            point(c + d * r1);
        }
        for (int i = 0; i < segs; ++i)
        {
            Index i0 = base + Index(2 * i), i1 = base + Index(2 * ((i + 1) % n));
            face({i0, i1, i1 + 1, i0 + 1});
        }
        next();
    }
    void disc(glm::dvec2 c, double r, int segs)
    {
        Index centre = point(c), base = Index(pts.size());
        for (int i = 0; i < segs; ++i)
        {
            double a = glm::two_pi<double>() * i / segs;
            point(c + glm::dvec2(std::cos(a), std::sin(a)) * r);
        }
        for (int i = 0; i < segs; ++i)
            face({centre, base + Index(i), base + Index((i + 1) % segs)});
        next();
    }
    /** L made of three quads. */
    void ell(glm::dvec2 o, double w, double h, double t)
    {
        Index a = point(o), b = point(o + glm::dvec2(t, 0)), c = point(o + glm::dvec2(w, 0)), d = point(o + glm::dvec2(w, t)),
              e = point(o + glm::dvec2(t, t)), f = point(o + glm::dvec2(t, h)), g = point(o + glm::dvec2(0, h)), k = point(o + glm::dvec2(0, t));
        face({a, b, e, k});
        face({b, c, d, e});
        face({k, e, f, g});
        next();
    }
    geo::Mesh build()
    {
        geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
        std::span<glm::vec2> uv = uv::uvLayer(m, "uv0");
        for (Index f = 0; f < m.faceCapacity(); ++f)
            m.forEachFaceCorner(f, [&](Index c) {
                glm::dvec3 p = m.position(m.cornerPoint(c));
                uv[c] = glm::vec2(float(p.x), float(p.y));
            });
        return m;
    }
};

/** Assorted island shapes like a real asset's: boxes, long strips, Ls, rings, arcs and discs, at many sizes. */
geo::Mesh shapeSoup(int n, uint32_t seed = 777)
{
    Rng rnd{seed};
    SoupBuilder b;
    for (int i = 0; i < n; ++i)
    {
        double s = 2.0 + 28.0 * rnd() * rnd();
        glm::dvec2 o(rnd() * 400.0, rnd() * 400.0);
        switch (i % 7)
        {
        case 0: b.rect(o, s, s * (0.3 + rnd())); break;
        case 1: b.rect(o, s * 2.0, s * (0.08 + 0.15 * rnd())); break;
        case 2: b.ell(o, s, s * (0.6 + rnd()), s * 0.25); break;
        case 3: b.ring(o, s * 0.5, s * 0.62, 24); break;
        case 4: b.ring(o, s * 0.4, s * 0.55, 16, glm::pi<double>() * (0.8 + rnd())); break;
        case 5: b.disc(o, s * 0.5, 12); break;
        default: b.rect(o, s * 0.7, s * 0.7); break;
        }
    }
    return b.build();
}

size_t overlaps(const geo::Mesh& m, std::string_view set = "uv0")
{
    std::vector<uint8_t> flags;
    return uv::overlappingFaces(m, set, flags);
}

std::vector<uv::Chart> islands(const geo::Mesh& m, std::string_view set = "uv0")
{
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    co.uvSet = set;
    return uv::buildCharts(m, {}, co);
}

double segDist(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b)
{
    glm::dvec2 d = b - a;
    double t = glm::dot(d, d) > 0.0 ? std::clamp(glm::dot(p - a, d) / glm::dot(d, d), 0.0, 1.0) : 0.0;
    return glm::length(p - (a + t * d));
}

/** Smallest distance between the boundaries of different islands, in texels of a texture of the given size. */
double gapTexels(const geo::Mesh& m, double resolution, std::string_view set = "uv0")
{
    std::vector<uv::Chart> charts = islands(m, set);
    std::vector<std::vector<std::pair<glm::dvec2, glm::dvec2>>> segs(charts.size());
    std::vector<glm::dvec2> lo(charts.size()), hi(charts.size());
    for (size_t i = 0; i < charts.size(); ++i)
    {
        lo[i] = charts[i].uvMin() * resolution;
        hi[i] = charts[i].uvMax() * resolution;
        for (const auto& loop : charts[i].boundaryLoops())
            for (size_t k = 0; k < loop.size(); ++k)
                segs[i].emplace_back(charts[i].uv[loop[k]] * resolution, charts[i].uv[loop[(k + 1) % loop.size()]] * resolution);
    }
    double best = 1e9;
    for (size_t i = 0; i < segs.size(); ++i)
        for (size_t j = i + 1; j < segs.size(); ++j)
        {
            glm::dvec2 sep = glm::max(lo[j] - hi[i], lo[i] - hi[j]);
            if (std::max(sep.x, sep.y) >= best)
                continue;
            for (auto& [a, b] : segs[i])
                for (auto& [c, d] : segs[j])
                    best = std::min({best, segDist(a, c, d), segDist(b, c, d), segDist(c, a, b), segDist(d, a, b)});
        }
    return best;
}

/** Every corner inside [lo, hi] (tile uvs), within eps. */
bool inside(const geo::Mesh& m, glm::dvec2 lo, glm::dvec2 hi, std::string_view set = "uv0")
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(set);
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            bool ok = true;
            m.forEachFaceCorner(f, [&](Index c) {
                ok &= uv[c].x >= lo.x - 1e-5 && uv[c].y >= lo.y - 1e-5 && uv[c].x <= hi.x + 1e-5 && uv[c].y <= hi.y + 1e-5;
            });
            if (!ok)
                return false;
        }
    return true;
}

std::vector<glm::vec2> uvsOf(const geo::Mesh& m, std::string_view set = "uv0")
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(set);
    return {uv.begin(), uv.end()};
}
} // namespace

TEST_CASE("pack: no overlaps, padding held in pixels, inside the margin")
{
    geo::Mesh m = shapeSoup(140);
    uv::PackOptions po;
    po.resolution = 1024;
    po.padding = 4.0;
    auto t0 = std::chrono::steady_clock::now();
    uv::PackResult r = uv::packIslands(m, {}, "uv0", po);
    MESSAGE("140 mixed islands at 1024: " << seconds(t0) * 1000.0 << " ms, density " << r.coverage * 100.0 << "%, " << r.attempts << " layouts");
    REQUIRE(r.ok);
    CHECK(overlaps(m) == 0);
    double gap = gapTexels(m, po.resolution);
    MESSAGE("smallest gap " << gap << " px");
    CHECK(gap >= po.padding - 1e-3);
    double margin = po.padding * 0.5 / po.resolution;
    CHECK(inside(m, glm::dvec2(margin), glm::dvec2(1.0 - margin)));
    CHECK(r.coverage > 0.6);

    SUBCASE("padding at a mip level and a coarse search grid still hold in pixels")
    {
        geo::Mesh n = shapeSoup(60, 99);
        uv::PackOptions pm;
        pm.resolution = 2048;
        pm.padding = 2.0;
        pm.paddingMips = 2;
        pm.workingResolution = 300;
        REQUIRE(uv::packIslands(n, {}, "uv0", pm).ok);
        CHECK(overlaps(n) == 0);
        CHECK(gapTexels(n, pm.resolution) >= 8.0 - 1e-3);
    }
}

TEST_CASE("pack: small islands nest in a ring's hole")
{
    SoupBuilder b;
    b.ring({0, 0}, 40.0, 50.0, 32);
    for (int i = 0; i < 60; ++i)
        b.rect({200.0 + 10.0 * (i % 8), 10.0 * (i / 8)}, 6.0, 6.0);
    geo::Mesh m = b.build();
    uv::PackOptions po;
    po.resolution = 1024;
    po.padding = 2.0;
    uv::PackResult r = uv::packIslands(m, {}, "uv0", po);
    REQUIRE(r.ok);
    CHECK(overlaps(m) == 0);
    std::vector<uv::Chart> charts = islands(m);
    // The ring is the island with the most vertices; its hole is a circle about its centre.
    auto ring = std::max_element(charts.begin(), charts.end(), [](const uv::Chart& a, const uv::Chart& c) { return a.uv.size() < c.uv.size(); });
    glm::dvec2 centre = (ring->uvMin() + ring->uvMax()) * 0.5;
    double inner = (ring->uvMax().x - ring->uvMin().x) * 0.5 * 0.8;
    int nested = 0;
    for (const uv::Chart& ch : charts)
        if (&ch != &*ring && glm::length(ch.uvCentroid() - centre) < inner)
            ++nested;
    MESSAGE(nested << " of 60 islands inside the ring, density " << r.coverage * 100.0 << "%");
    CHECK(nested >= 10);
}

TEST_CASE("pack: rotation helps long islands")
{
    SoupBuilder b;
    Rng rnd{4242};
    for (int i = 0; i < 80; ++i)
    {
        double len = 20.0 + 40.0 * rnd(), w = 2.0 + 2.0 * rnd();
        if (i % 2)
            b.rect({0.0, 0.0}, len, w);
        else
            b.rect({0.0, 0.0}, w, len);
    }
    geo::Mesh a = b.build(), c = a;
    uv::PackOptions po;
    po.resolution = 1024;
    po.padding = 2.0;
    po.preOrient = false;
    po.rotateStep = 0;
    uv::PackResult fixedR = uv::packIslands(a, {}, "uv0", po);
    po.rotateStep = 90;
    uv::PackResult rotR = uv::packIslands(c, {}, "uv0", po);
    MESSAGE("long islands: density " << fixedR.coverage * 100.0 << "% unrotated, " << rotR.coverage * 100.0 << "% with quarter turns");
    REQUIRE(fixedR.ok);
    REQUIRE(rotR.ok);
    CHECK(rotR.coverage > fixedR.coverage);
    CHECK(overlaps(c) == 0);

    SUBCASE("free rotation and flipping stay overlap free")
    {
        geo::Mesh d = shapeSoup(50, 5);
        uv::PackOptions pf;
        pf.freeRotation = true;
        pf.flip = true;
        uv::PackResult r = uv::packIslands(d, {}, "uv0", pf);
        MESSAGE("free rotation + flip: density " << r.coverage * 100.0 << "%");
        REQUIRE(r.ok);
        CHECK(overlaps(d) == 0);
        CHECK(gapTexels(d, pf.resolution) >= pf.padding - 1e-3);
    }
}

TEST_CASE("pack: identical and mirrored islands stack")
{
    SoupBuilder b;
    Rng rnd{17};
    std::vector<std::array<double, 3>> shapes;
    for (int i = 0; i < 10; ++i)
        shapes.push_back({10.0 + 20.0 * rnd(), 10.0 + 20.0 * rnd(), 3.0 + 3.0 * rnd()});
    for (int copy = 0; copy < 3; ++copy)
        for (const auto& s : shapes)
            b.ell({100.0 * copy, 0.0}, s[0], s[1], s[2]);
    geo::Mesh plain = b.build(), m = plain;
    // Third copy: uvs mirrored and turned, as the mirrored half of a model comes out.
    std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    for (Index f = 0; f < m.faceCapacity(); ++f)
        m.forEachFaceCorner(f, [&](Index c) {
            if (uv[c].x >= 199.0f)
            {
                glm::dvec2 p(-(uv[c].x - 200.0), uv[c].y);
                double a = 0.6;
                uv[c] = glm::vec2(float(p.x * std::cos(a) - p.y * std::sin(a)), float(p.x * std::sin(a) + p.y * std::cos(a)));
            }
        });
    geo::Mesh noMirror = m;
    uv::PackOptions po;
    po.stack = true;
    uv::PackResult r = uv::packIslands(m, {}, "uv0", po);
    REQUIRE(r.ok);
    CHECK(r.stacked == 20);
    auto stacks = uv::stackedIslands(m, "uv0", 0.002);
    CHECK(stacks.size() == 10);
    for (const auto& s : stacks)
        CHECK(s.size() == 3);

    uv::PackOptions pn = po;
    pn.stackMirrored = false;
    uv::PackResult rn = uv::packIslands(noMirror, {}, "uv0", pn);
    REQUIRE(rn.ok);
    CHECK(rn.stacked == 10);
    CHECK(uv::stackedIslands(noMirror, "uv0").size() == 10);

    uv::PackResult rp = uv::packIslands(plain, {}, "uv0", {});
    CHECK(rp.stacked == 0);
    CHECK(uv::stackedIslands(plain, "uv0").empty());
}

TEST_CASE("pack: fixed islands stay put and the rest go around them")
{
    geo::Mesh m = shapeSoup(80, 31);
    uv::PackOptions po;
    po.resolution = 1024;
    po.padding = 3.0;
    REQUIRE(uv::packIslands(m, {}, "uv0", po).ok);
    std::vector<uv::Chart> charts = islands(m);
    std::vector<Index> fixedFaces;
    for (size_t i = 0; i < charts.size(); i += 4)
        fixedFaces.insert(fixedFaces.end(), charts[i].faces.begin(), charts[i].faces.end());
    std::vector<glm::vec2> before = uvsOf(m);
    uv::PackTarget t;
    t.mesh = &m;
    t.fixedFaces = fixedFaces;
    uv::PackOptions pf = po;
    pf.rotateStep = 45;
    uv::PackResult r = uv::packMeshes(std::span<uv::PackTarget>(&t, 1), "uv0", pf);
    REQUIRE(r.ok);
    std::vector<glm::vec2> after = uvsOf(m);
    bool fixedKept = true, othersMoved = false;
    std::set<Index> fixedSet(fixedFaces.begin(), fixedFaces.end());
    for (Index f = 0; f < m.faceCapacity(); ++f)
        m.forEachFaceCorner(f, [&](Index c) {
            if (fixedSet.count(f))
                fixedKept &= before[c] == after[c];
            else
                othersMoved |= before[c] != after[c];
        });
    CHECK(fixedKept);
    CHECK(othersMoved);
    CHECK(overlaps(m) == 0);
    CHECK(gapTexels(m, po.resolution) >= po.padding - 1e-3);
}

TEST_CASE("pack: udim tiles by group, keep tile, region and locked scale")
{
    geo::Mesh m = shapeSoup(60, 8);
    std::vector<uv::Chart> charts = islands(m);
    {
        std::span<int32_t> group = m.faceAttrs().getOrAdd<int32_t>("uvGroup");
        for (size_t i = 0; i < charts.size(); ++i)
            for (Index f : charts[i].faces)
                group[f] = int32_t(i % 3);
    }
    std::span<const int32_t> group = m.faceAttrs().get<int32_t>("uvGroup");
    uv::PackOptions po;
    po.layout = uv::PackLayout::GroupTiles;
    po.groupBy = uv::PackGroupBy::Attribute;
    po.tilesU = 2;
    uv::PackResult r = uv::packIslands(m, {}, "uv0", po);
    REQUIRE(r.ok);
    CHECK(r.tilesUsed == 3);
    CHECK(overlaps(m) == 0);
    auto tileOf = [](const uv::Chart& ch) { return glm::ivec2(glm::floor(ch.uvCentroid())); };
    bool rule = true;
    for (const uv::Chart& ch : islands(m))
    {
        int g = group[ch.faces.front()];
        rule &= tileOf(ch) == glm::ivec2(g % 2, g / 2);
    }
    CHECK(rule);

    SUBCASE("keep tile leaves every island in its tile")
    {
        uv::PackOptions pk;
        pk.layout = uv::PackLayout::KeepTile;
        pk.rotateStep = 0;
        REQUIRE(uv::packIslands(m, {}, "uv0", pk).ok);
        bool kept = true;
        for (const uv::Chart& ch : islands(m))
        {
            int g = group[ch.faces.front()];
            kept &= tileOf(ch) == glm::ivec2(g % 2, g / 2);
        }
        CHECK(kept);
        CHECK(overlaps(m) == 0);
    }

    SUBCASE("a sub-rectangle region")
    {
        uv::PackOptions pr;
        pr.regionMin = {0.5, 0.25};
        pr.regionMax = {1.0, 0.75};
        REQUIRE(uv::packIslands(m, {}, "uv0", pr).ok);
        CHECK(inside(m, {0.5, 0.25}, {1.0, 0.75}));
        CHECK(overlaps(m) == 0);
    }

    SUBCASE("group regions split the tile")
    {
        uv::PackOptions pg;
        pg.layout = uv::PackLayout::GroupRegions;
        pg.groupBy = uv::PackGroupBy::Attribute;
        REQUIRE(uv::packIslands(m, {}, "uv0", pg).ok);
        CHECK(inside(m, {0.0, 0.0}, {1.0, 1.0}));
        CHECK(overlaps(m) == 0);
        CHECK(gapTexels(m, pg.resolution) >= pg.padding - 1e-3);
    }

    SUBCASE("locked scale keeps island sizes and spills into more tiles")
    {
        geo::Mesh n = shapeSoup(40, 3);
        for (glm::vec2& p : n.cornerAttrs().get<glm::vec2>("uv0"))
            p *= 0.02f;
        std::vector<double> areas;
        for (const uv::Chart& ch : islands(n))
            areas.push_back(std::abs(ch.areaUv()));
        uv::PackOptions pl;
        pl.lockScale = true;
        pl.tilesU = 4;
        uv::PackResult rl = uv::packIslands(n, {}, "uv0", pl);
        REQUIRE(rl.ok);
        CHECK(rl.tilesUsed >= 2);
        std::vector<uv::Chart> after = islands(n);
        REQUIRE(after.size() == areas.size());
        bool same = true;
        for (size_t i = 0; i < after.size(); ++i)
            same &= std::abs(std::abs(after[i].areaUv()) - areas[i]) <= 1e-3 * areas[i];
        CHECK(same);
        CHECK(overlaps(n) == 0);
    }
}

TEST_CASE("pack: density is even across objects in world space")
{
    geo::Mesh a = shapeSoup(20, 1), b = shapeSoup(20, 2);
    uv::PackTarget t[2];
    t[0].mesh = &a;
    t[1].mesh = &b;
    t[1].linear = glm::dmat3(3.0);
    uv::PackResult r = uv::packMeshes(t, "uv0", {});
    REQUIRE(r.ok);
    double da = uv::texelDensity(a, "uv0", {}, 1024.0), db = uv::texelDensity(b, "uv0", {}, 1024.0, glm::dmat3(3.0));
    CHECK(da == doctest::Approx(db).epsilon(0.001));
}

TEST_CASE("pack: deterministic")
{
    geo::Mesh a = shapeSoup(300, 12), b = a;
    uv::PackOptions po;
    po.effort = 2;
    po.resolution = 2048;
    uv::PackResult ra = uv::packIslands(a, {}, "uv0", po), rb = uv::packIslands(b, {}, "uv0", po);
    REQUIRE(ra.ok);
    CHECK(ra.coverage == rb.coverage);
    CHECK(uvsOf(a) == uvsOf(b));
}

TEST_CASE("pack benchmark: 200 and 2000 islands" * doctest::skip())
{
    for (int n : {200, 2000})
        for (int effort : {0, 1, 2})
        {
            geo::Mesh m = shapeSoup(n);
            uv::PackOptions po;
            po.resolution = 4096;
            po.padding = 4.0;
            po.effort = effort;
            auto t0 = std::chrono::steady_clock::now();
            uv::PackResult r = uv::packIslands(m, {}, "uv0", po);
            double t = seconds(t0);
            MESSAGE(n << " islands, effort " << effort << ": " << t << " s, density " << r.coverage * 100.0 << "%, " << r.attempts << " layouts");
            CHECK(r.ok);
            CHECK(overlaps(m) == 0);
        }
}
