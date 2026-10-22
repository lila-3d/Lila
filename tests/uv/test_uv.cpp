#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "uv/Uv.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <chrono>
#include <cmath>
#include <set>

using namespace lila;
using geo::Index;

namespace
{
double seconds(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void clearSeams(geo::Mesh& m)
{
    std::span<bool> s = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    std::fill(s.begin(), s.end(), false);
}

size_t overlaps(const geo::Mesh& m, std::string_view set = "uv0")
{
    std::vector<uint8_t> flags;
    return uv::overlappingFaces(m, set, flags);
}

bool insideUnit(const geo::Mesh& m, std::string_view set, float margin = 0.0f, float tiles = 1.0f)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(set);
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            bool ok = true;
            m.forEachFaceCorner(f, [&](Index c) {
                ok &= uv[c].x >= margin - 1e-6f && uv[c].y >= margin - 1e-6f && uv[c].x <= tiles - margin + 1e-6f && uv[c].y <= 1.0f - margin + 1e-6f;
            });
            if (!ok)
                return false;
        }
    return true;
}

double segDist(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b)
{
    glm::dvec2 d = b - a;
    double t = glm::dot(d, d) > 0.0 ? std::clamp(glm::dot(p - a, d) / glm::dot(d, d), 0.0, 1.0) : 0.0;
    return glm::length(p - (a + t * d));
}

/** Smallest distance between boundary segments of different uv islands. */
double islandGap(const geo::Mesh& m, std::string_view set)
{
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    co.uvSet = set;
    std::vector<uv::Chart> charts = uv::buildCharts(m, {}, co);
    std::vector<std::vector<std::pair<glm::dvec2, glm::dvec2>>> segs(charts.size());
    for (size_t i = 0; i < charts.size(); ++i)
        for (const auto& loop : charts[i].boundaryLoops())
            for (size_t k = 0; k < loop.size(); ++k)
                segs[i].emplace_back(charts[i].uv[loop[k]], charts[i].uv[loop[(k + 1) % loop.size()]]);
    double best = 1e9;
    for (size_t i = 0; i < segs.size(); ++i)
        for (size_t j = i + 1; j < segs.size(); ++j)
            for (auto& [a, b] : segs[i])
                for (auto& [c, d] : segs[j])
                    best = std::min({best, segDist(a, c, d), segDist(b, c, d), segDist(c, a, b), segDist(d, a, b)});
    return best;
}

/** Many separate quads of assorted sizes and aspect ratios, scattered. */
geo::Mesh quadSoup(int n)
{
    std::vector<glm::dvec3> pts;
    std::vector<Index> sizes, idx;
    uint32_t seed = 12345;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return double(seed >> 8) / double(1u << 24);
    };
    for (int i = 0; i < n; ++i)
    {
        double w = 2.0 + 30.0 * rnd(), h = 2.0 + 30.0 * rnd();
        glm::dvec3 o(rnd() * 500.0, rnd() * 500.0, double(i) * 3.0);
        Index b = Index(pts.size());
        pts.push_back(o);
        pts.push_back(o + glm::dvec3(w, 0, 0));
        pts.push_back(o + glm::dvec3(w, h, 0));
        pts.push_back(o + glm::dvec3(0, h, 0));
        sizes.push_back(4);
        for (Index k = 0; k < 4; ++k)
            idx.push_back(b + k);
    }
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    std::span<glm::vec2> uv = uv::uvLayer(m, "uv0");
    for (Index f = 0; f < m.faceCapacity(); ++f)
        m.forEachFaceCorner(f, [&](Index c) {
            glm::dvec3 p = m.position(m.cornerPoint(c));
            uv[c] = glm::vec2(float(p.x), float(p.y));
        });
    return m;
}
} // namespace

TEST_CASE("svd-free sanity: lscm flattens a planar grid exactly")
{
    geo::PlaneOptions po;
    po.subdivX = 6;
    po.subdivZ = 4;
    geo::Mesh m = geo::makePlane(po);
    uv::UnwrapFacesOptions o;
    o.keepPlacement = false;
    auto r = uv::unwrapFaces(m, {}, "uv0", o);
    CHECK(r.islands == 1);
    CHECK(r.failed == 0);
    auto d = uv::summarize(m, uv::faceDistortion(m, "uv0"));
    CHECK(d.meanArea < 1e-3);
    CHECK(d.meanAngle < 1e-3);
    CHECK(d.folded == 0);
}

TEST_CASE("cylinder with one seam unwraps with low stretch")
{
    geo::CylinderOptions co;
    co.caps = false;
    co.subdivAxis = 32;
    co.subdivHeight = 8;
    co.height = 200.0;
    geo::Mesh m = geo::makeCylinder(co);
    clearSeams(m);
    // One straight seam along the side.
    std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    int cut = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
        if (std::abs(a.x - b.x) < 1e-6 && std::abs(a.z - b.z) < 1e-6 && a.x > 49.9 && std::abs(a.z) < 1e-6)
        {
            seam[e] = true;
            ++cut;
        }
    }
    REQUIRE(cut == co.subdivHeight);
    uv::UnwrapFacesOptions o;
    o.keepPlacement = false;
    auto t0 = std::chrono::steady_clock::now();
    auto r = uv::unwrapFaces(m, {}, "uv0", o);
    MESSAGE("cylinder unwrap " << seconds(t0) * 1000.0 << " ms");
    CHECK(r.islands == 1);
    CHECK(r.seamsAdded == 0);
    auto d = uv::summarize(m, uv::faceDistortion(m, "uv0"));
    MESSAGE("cylinder distortion area " << d.meanArea << " angle " << d.meanAngle << " max " << d.maxArea << "/" << d.maxAngle);
    CHECK(d.meanArea < 0.01);
    CHECK(d.meanAngle < 0.02);
    CHECK(d.folded == 0);
    CHECK(overlaps(m) == 0);
}

TEST_CASE("closed shapes are opened before unwrapping")
{
    geo::Mesh m = geo::makeSphere({});
    clearSeams(m);
    uv::UnwrapFacesOptions o;
    o.keepPlacement = false;
    auto r = uv::unwrapFaces(m, {}, "uv0", o);
    CHECK(r.seamsAdded > 0);
    CHECK(r.failed == 0);
    CHECK(overlaps(m) == 0);
}

TEST_CASE("packer has no overlaps and respects padding")
{
    geo::Mesh m = quadSoup(120);
    uv::PackOptions po;
    po.resolution = 512;
    po.padding = 4.0;
    auto t0 = std::chrono::steady_clock::now();
    auto r = uv::packIslands(m, {}, "uv0", po);
    MESSAGE("packed 120 islands in " << seconds(t0) * 1000.0 << " ms, coverage " << r.coverage);
    REQUIRE(r.ok);
    CHECK(overlaps(m) == 0);
    double gapTexels = islandGap(m, "uv0") * po.resolution;
    MESSAGE("smallest gap " << gapTexels << " texels");
    CHECK(gapTexels >= po.padding - 1e-3);
    CHECK(insideUnit(m, "uv0", float(po.padding * 0.5 / po.resolution)));
    CHECK(r.coverage > 0.45);
}

TEST_CASE("packer is deterministic and keeps texel density even")
{
    geo::Mesh a = quadSoup(40), b = quadSoup(40);
    uv::PackOptions po;
    po.resolution = 1024;
    po.padding = 8.0;
    uv::packIslands(a, {}, "uv0", po);
    uv::packIslands(b, {}, "uv0", po);
    std::span<const glm::vec2> ua = a.cornerAttrs().get<glm::vec2>("uv0"), ub = b.cornerAttrs().get<glm::vec2>("uv0");
    bool same = true;
    for (size_t i = 0; i < ua.size(); ++i)
        same &= ua[i] == ub[i];
    CHECK(same);
    double lo = 1e9, hi = 0.0;
    for (Index f = 0; f < a.faceCapacity(); ++f)
    {
        Index one[] = {f};
        double d = uv::texelDensity(a, "uv0", one, 1024.0);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    CHECK(hi / lo < 1.001);
}

TEST_CASE("packer stacks identical islands and fills udim tiles")
{
    geo::Mesh m = quadSoup(30);
    geo::Mesh dup = m;
    m.append(dup);
    uv::PackOptions po;
    po.stack = true;
    auto r = uv::packIslands(m, {}, "uv0", po);
    CHECK(r.ok);
    CHECK(r.stacked == 30);

    geo::Mesh t = quadSoup(60);
    uv::PackOptions pt;
    pt.tilesU = 3;
    pt.texelsPerUnit = 9.0;
    auto rt = uv::packIslands(t, {}, "uv0", pt);
    CHECK(rt.ok);
    CHECK(rt.tilesUsed >= 2);
    CHECK(overlaps(t) == 0);
    CHECK(insideUnit(t, "uv0", 0.0f, 3.0f));
}

TEST_CASE("cube auto uv: six or fewer islands, no overlaps")
{
    geo::Mesh m = geo::makeCube({});
    clearSeams(m);
    auto r = uv::autoUv(m);
    MESSAGE("cube islands " << r.islands << " seams " << r.seams);
    CHECK(r.islands <= 6);
    CHECK(r.islands >= 1);
    CHECK(r.pack.ok);
    CHECK(overlaps(m) == 0);
    CHECK(insideUnit(m, "uv0"));
    auto d = uv::summarize(m, uv::faceDistortion(m, "uv0"));
    CHECK(d.meanArea < 1e-3);
}

TEST_CASE("auto uv on curved shapes stays low distortion")
{
    for (int shape = 0; shape < 3; ++shape)
    {
        geo::Mesh m = shape == 0 ? geo::makeSphere({}) : shape == 1 ? geo::makeTorus({}) : geo::makeCylinder({});
        clearSeams(m);
        auto r = uv::autoUv(m);
        auto d = uv::summarize(m, uv::faceDistortion(m, "uv0"));
        MESSAGE("shape " << shape << ": " << r.islands << " islands, area " << d.meanArea << " angle " << d.meanAngle << " coverage "
                         << r.pack.coverage);
        CHECK(r.pack.ok);
        CHECK(overlaps(m) == 0);
        CHECK(d.folded == 0);
        CHECK(d.meanArea < 0.25);
        CHECK(d.meanAngle < 0.25);
        if (shape == 2)
            CHECK(r.islands <= 4);
    }
}

TEST_CASE("projections")
{
    geo::Mesh m = geo::makeCube({});
    uv::ProjectOptions o;
    o.type = uv::Projection::Box;
    REQUIRE(uv::project(m, {}, "uv0", o));
    CHECK(uv::islandFaces(m, {}, {}).size() == 6);
    CHECK(insideUnit(m, "uv0"));
    auto d = uv::summarize(m, uv::faceDistortion(m, "uv0"));
    CHECK(d.meanArea < 1e-4);
    CHECK(d.folded == 0);

    geo::CylinderOptions co;
    co.caps = false;
    geo::Mesh c = geo::makeCylinder(co);
    o.type = uv::Projection::Cylindrical;
    o.axis = uv::ProjectAxis::Y;
    REQUIRE(uv::project(c, {}, "uv0", o));
    auto dc = uv::summarize(c, uv::faceDistortion(c, "uv0"));
    CHECK(dc.folded == 0);
    CHECK(dc.meanAngle < 0.05);

    o.type = uv::Projection::Planar;
    o.axis = uv::ProjectAxis::BestFit;
    geo::Mesh p = geo::makePlane({});
    REQUIRE(uv::project(p, {}, "uv0", o));
    CHECK(uv::summarize(p, uv::faceDistortion(p, "uv0")).meanAngle < 1e-4);

    o.type = uv::Projection::Spherical;
    geo::Mesh s = geo::makeSphere({});
    REQUIRE(uv::project(s, {}, "uv0", o));
    CHECK(uv::summarize(s, uv::faceDistortion(s, "uv0")).folded == 0);
}

TEST_CASE("straighten turns a bent quad strip into a straight grid")
{
    geo::CylinderOptions co;
    co.caps = false;
    co.subdivAxis = 16;
    co.subdivHeight = 3;
    geo::Mesh m = geo::makeCylinder(co);
    auto charts = uv::buildCharts(m);
    REQUIRE(!charts.empty());
    uv::Chart& ch = charts.front();
    for (glm::dvec2& p : ch.uv)
        p += glm::dvec2(std::sin(p.y * 7.0) * 0.01, 0.0);
    CHECK(uv::straightenChart(ch) == uv::StraightenResult::Grid);
    std::set<double> us, vs;
    for (const glm::dvec2& p : ch.uv)
    {
        us.insert(std::round(p.x * 1e6) / 1e6);
        vs.insert(std::round(p.y * 1e6) / 1e6);
    }
    CHECK(us.size() == size_t(co.subdivAxis + 1));
    CHECK(vs.size() == size_t(co.subdivHeight + 1));
}

TEST_CASE("orient aligns an island to the axes")
{
    geo::Mesh m = geo::makePlane({});
    std::span<glm::vec2> uv = uv::uvLayer(m, "uv0");
    for (glm::vec2& p : uv)
    {
        float a = 0.5f;
        p = glm::vec2(p.x * 2.0f * std::cos(a) - p.y * std::sin(a), p.x * 2.0f * std::sin(a) + p.y * std::cos(a));
    }
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    auto charts = uv::buildCharts(m, {}, co);
    uv::orientChart(charts.front());
    glm::dvec2 ext = charts.front().uvMax() - charts.front().uvMin();
    CHECK(ext.x == doctest::Approx(2.0).epsilon(1e-4));
    CHECK(ext.y == doctest::Approx(1.0).epsilon(1e-4));
}

TEST_CASE("texel density measure and set")
{
    geo::Mesh m = geo::makePlane({}); // 100 x 100 cm, uv 0-1
    CHECK(uv::texelDensity(m, "uv0", {}, 1024.0) == doctest::Approx(10.24));
    uv::setTexelDensity(m, "uv0", {}, 1024.0, 5.12);
    CHECK(uv::texelDensity(m, "uv0", {}, 1024.0) == doctest::Approx(5.12));
}

TEST_CASE("overlap detection finds stacked faces")
{
    geo::Mesh m = quadSoup(4);
    std::span<glm::vec2> uv = uv::uvLayer(m, "uv0");
    for (glm::vec2& p : uv)
        p = glm::vec2(0.5f);
    // Same unit square for every face.
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        int k = 0;
        m.forEachFaceCorner(f, [&](Index c) {
            const glm::vec2 sq[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            uv[c] = sq[k++];
        });
    }
    CHECK(overlaps(m) == 4);
}

TEST_CASE("lightmap uvs: no overlaps, padded for the resolution")
{
    geo::Mesh m = geo::makeCylinder({});
    // Stack every island of uv0 on top of each other, as tiling textures often do.
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    auto charts = uv::buildCharts(m, {}, co);
    for (uv::Chart& ch : charts)
    {
        glm::dvec2 lo = ch.uvMin();
        for (glm::dvec2& p : ch.uv)
            p -= lo;
    }
    uv::writeCharts(m, charts, "uv0");
    REQUIRE(overlaps(m, "uv0") > 0);
    uv::LightmapOptions lo;
    lo.resolution = 64;
    lo.padding = 2.0;
    auto r = uv::generateLightmapUvs(m, lo);
    REQUIRE(r.ok);
    CHECK(overlaps(m, "uv1") == 0);
    double gap = islandGap(m, "uv1") * lo.resolution;
    MESSAGE("lightmap islands " << r.islands << ", gap " << gap << " texels");
    CHECK(gap >= lo.padding - 1e-3);
    CHECK(insideUnit(m, "uv1", float(lo.padding * 0.5 / lo.resolution)));
}

TEST_CASE("timings on a 200k face mesh")
{
    geo::TorusOptions to;
    to.subdivAxis = 500;
    to.subdivHeight = 400;
    geo::Mesh m = geo::makeTorus(to);
    MESSAGE("faces " << m.numFaces());
    clearSeams(m);

    auto t0 = std::chrono::steady_clock::now();
    geo::Mesh seamed = m;
    auto sr = uv::autoSeams(seamed);
    MESSAGE("autoSeams: " << seconds(t0) << " s, " << sr.charts.size() << " islands");
    t0 = std::chrono::steady_clock::now();
    auto r = uv::autoUv(m);
    double tAuto = seconds(t0);
    MESSAGE("autoUv: " << tAuto << " s, " << r.islands << " islands, coverage " << r.pack.coverage);
    CHECK(r.pack.ok);

    t0 = std::chrono::steady_clock::now();
    auto charts = uv::buildCharts(m);
    double tCharts = seconds(t0);
    t0 = std::chrono::steady_clock::now();
    uv::unwrapCharts(charts);
    double tUnwrap = seconds(t0);
    t0 = std::chrono::steady_clock::now();
    auto pr = uv::packCharts(charts);
    double tPack = seconds(t0);
    uv::writeCharts(m, charts, "uv0");
    t0 = std::chrono::steady_clock::now();
    size_t ov = overlaps(m);
    double tOverlap = seconds(t0);
    t0 = std::chrono::steady_clock::now();
    auto d = uv::summarize(m, uv::faceDistortion(m, "uv0"));
    double tDist = seconds(t0);
    MESSAGE("islands " << charts.size() << ": buildCharts " << tCharts << " s, unwrap " << tUnwrap << " s, pack " << tPack << " s, overlaps "
                       << tOverlap << " s, distortion " << tDist << " s");
    MESSAGE("distortion area " << d.meanArea << " angle " << d.meanAngle);
    CHECK(pr.ok);
    CHECK(ov == 0);

    t0 = std::chrono::steady_clock::now();
    uv::LightmapOptions lo;
    lo.resolution = 256;
    auto lr = uv::generateLightmapUvs(m, lo);
    MESSAGE("lightmap " << seconds(t0) << " s");
    CHECK(lr.ok);
}

TEST_CASE("timings on a 200k face mesh of many parts")
{
    // A thousand small spheres and boxes: many islands, as a kitbashed prop has.
    std::vector<geo::Mesh> parts;
    for (int i = 0; i < 500; ++i)
    {
        geo::SphereOptions so;
        so.radius = 5.0 + (i % 7);
        so.subdivAxis = 20;
        so.subdivHeight = 16;
        geo::Mesh s = geo::makeSphere(so);
        geo::CubeOptions bo;
        bo.width = 4.0 + (i % 5) * 3.0;
        bo.subdivX = bo.subdivY = bo.subdivZ = 4;
        geo::Mesh b = geo::makeCube(bo);
        for (glm::dvec3& p : s.positions())
            p += glm::dvec3((i % 25) * 30.0, (i / 25) * 30.0, 0.0);
        for (glm::dvec3& p : b.positions())
            p += glm::dvec3((i % 25) * 30.0, (i / 25) * 30.0, 40.0);
        parts.push_back(std::move(s));
        parts.push_back(std::move(b));
    }
    std::vector<const geo::Mesh*> ptrs;
    for (const geo::Mesh& p : parts)
        ptrs.push_back(&p);
    geo::Mesh m = geo::combine(ptrs);
    MESSAGE("faces " << m.numFaces());
    clearSeams(m);
    auto t0 = std::chrono::steady_clock::now();
    auto vis = uv::pointVisibility(m);
    MESSAGE("visibility: " << seconds(t0) << " s");
    t0 = std::chrono::steady_clock::now();
    geo::Mesh seamed = m;
    uv::AutoSeamOptions so;
    so.merge = false;
    auto sr = uv::autoSeams(seamed, {}, so);
    MESSAGE("autoSeams without merge: " << seconds(t0) << " s, " << sr.charts.size() << " islands");
    t0 = std::chrono::steady_clock::now();
    auto r = uv::autoUv(m);
    MESSAGE("autoUv: " << seconds(t0) << " s, " << r.islands << " islands, coverage " << r.pack.coverage);
    CHECK(r.pack.ok);
    CHECK(overlaps(m) == 0);
    t0 = std::chrono::steady_clock::now();
    uv::PackOptions po;
    po.resolution = 4096;
    po.padding = 4.0;
    auto pr = uv::packIslands(m, {}, "uv0", po);
    MESSAGE("repack at 4096: " << seconds(t0) << " s, coverage " << pr.coverage);
    CHECK(pr.ok);
    t0 = std::chrono::steady_clock::now();
    uv::LightmapOptions lo;
    lo.resolution = 1024;
    auto lr = uv::generateLightmapUvs(m, lo);
    MESSAGE("lightmap at 1024: " << seconds(t0) << " s");
    CHECK(lr.ok);
}
