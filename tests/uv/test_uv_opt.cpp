#include "TestUtil.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "uv/Align.h"
#include "uv/Bff.h"
#include "uv/OptCuts.h"
#include "uv/Optimize.h"
#include "uv/SymmetryUv.h"
#include "uv/Uv.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>

using namespace lila;
using namespace uvtest;
using geo::Index;

namespace
{
std::vector<Index> facesWhere(const geo::Mesh& m, const std::function<bool(const glm::dvec3&)>& keep)
{
    std::vector<Index> out;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && keep(m.faceCentroid(f)))
            out.push_back(f);
    return out;
}

/** One chart of the faces, ignoring the mesh's own seams. */
uv::Chart chartOf(const geo::Mesh& m, std::span<const Index> faces)
{
    uv::ChartOptions co;
    co.useSeamAttr = false;
    std::vector<uv::Chart> charts = uv::buildCharts(m, faces, co);
    REQUIRE(charts.size() == 1);
    return std::move(charts.front());
}

geo::Mesh hemisphere(int segments, std::vector<Index>& faces)
{
    geo::SphereOptions so;
    so.radius = 1.0;
    so.subdivAxis = segments;
    so.subdivHeight = segments;
    geo::Mesh m = geo::makeSphere(so);
    faces = facesWhere(m, [](const glm::dvec3& c) { return c.y > 0.0; });
    return m;
}

/** Largest relative difference between uv and 3D lengths over the chart's polygon edges. */
double edgeLengthError(const uv::Chart& ch)
{
    double worst = 0.0;
    for (size_t f = 0; f + 1 < ch.faceStart.size(); ++f)
        for (Index k = ch.faceStart[f]; k < ch.faceStart[f + 1]; ++k)
        {
            Index a = ch.cornerVertex[k], b = ch.cornerVertex[k + 1 < ch.faceStart[f + 1] ? k + 1 : ch.faceStart[f]];
            double l3 = glm::length(ch.pos[b] - ch.pos[a]), l2 = glm::length(ch.uv[b] - ch.uv[a]);
            worst = std::max(worst, std::abs(l2 - l3) / l3);
        }
    return worst;
}

/** Cube-map style islands: faces grouped by their dominant normal direction. Returns the seam length. */
double naiveCut(geo::Mesh& m, double* distortion)
{
    std::vector<int> label(m.faceCapacity(), -1);
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            glm::dvec3 n = m.faceNormal(f), a = glm::abs(n);
            int axis = a.x >= a.y && a.x >= a.z ? 0 : a.y >= a.z ? 1 : 2;
            label[f] = axis * 2 + (n[axis] < 0.0 ? 1 : 0);
        }
    std::vector<uint8_t> cut(m.edgeCapacity(), 0);
    double len = 0.0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isManifoldEdge(e) && !m.isBoundaryEdge(e))
        {
            Index c = m.edgeCorner(e);
            if (label[m.cornerFace(c)] != label[m.cornerFace(m.cornerRadial(c))])
            {
                cut[e] = 1;
                len += m.edgeLength(e);
            }
        }
    uv::ChartOptions co;
    co.useSeamAttr = false;
    co.extraSeams = cut;
    std::vector<uv::Chart> charts = uv::buildCharts(m, {}, co);
    double e = 0.0, area = 0.0;
    for (uv::Chart& ch : charts)
    {
        uv::UnwrapOptions uo;
        uo.usePins = false;
        uv::unwrapChart(ch, uo);
        double a = ch.area3d();
        e += (uv::chartDistortion(ch) + 1.0) * a;
        area += a;
    }
    *distortion = e / area - 1.0;
    return len;
}

geo::Mesh blob()
{
    geo::SphereOptions so;
    so.radius = 1.0;
    so.subdivAxis = 32;
    so.subdivHeight = 24;
    geo::Mesh m = geo::makeSphere(so);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            glm::dvec3 q = m.position(p);
            double s = 1.0 + 0.25 * std::sin(3.0 * q.x) * std::sin(2.0 * q.y + 0.5) + 0.15 * std::cos(4.0 * q.z) + 0.3 * std::max(0.0, q.y) * q.y;
            m.setPosition(p, q * s * glm::dvec3(1.0, 1.3, 0.9));
        }
    return m;
}
} // namespace

TEST_CASE("bff disk on a hemisphere is the stereographic map")
{
    std::vector<Index> faces;
    geo::Mesh m = hemisphere(64, faces);
    uv::Chart ch = chartOf(m, faces);
    uv::BffOptions o;
    o.boundary = uv::BffBoundary::Disk;
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(uv::bff(ch, o).ok);
    MESSAGE("bff disk, " << ch.tris.size() << " triangles: " << seconds(t0) * 1000.0 << " ms");
    CHECK(uv::foldedTriangles(ch) == 0);

    Index pole = 0;
    for (Index v = 0; v < ch.vertexCount(); ++v)
        if (ch.pos[v].y > ch.pos[pole].y)
            pole = v;
    const glm::dvec2 centre = ch.uv[pole];
    const std::vector<Index> border = ch.boundaryLoops().front();
    double radius = 0.0;
    for (Index v : border)
        radius += glm::length(ch.uv[v] - centre);
    radius /= double(border.size());
    double worst = 0.0, worstCircle = 0.0;
    for (Index v = 0; v < ch.vertexCount(); ++v)
    {
        double theta = std::acos(std::clamp(ch.pos[v].y, -1.0, 1.0));
        worst = std::max(worst, std::abs(glm::length(ch.uv[v] - centre) / radius - std::tan(0.5 * theta)));
    }
    for (Index v : border)
        worstCircle = std::max(worstCircle, std::abs(glm::length(ch.uv[v] - centre) / radius - 1.0));
    MESSAGE("stereographic radius error " << worst << ", boundary off the circle " << worstCircle);
    CHECK(worst < 0.01);
    CHECK(worstCircle < 0.002);
    // Piecewise linear maps of the thin pole triangles are not exactly conformal; LSCM is the least squares best.
    uv::Chart ref = chartOf(m, faces);
    REQUIRE(uv::lscm(ref, false));
    MESSAGE("angle distortion bff " << uv::chartStats(ch).angle << ", lscm " << uv::chartStats(ref).angle);
    CHECK(uv::chartStats(ch).angle < 1.5 * uv::chartStats(ref).angle);
}

TEST_CASE("bff keeps flat and developable patches isometric")
{
    geo::PlaneOptions po;
    po.subdivX = 12;
    po.subdivZ = 9;
    geo::Mesh plane = geo::makePlane(po);
    uv::Chart flat = chartOf(plane, uv::liveFaces(plane));
    REQUIRE(uv::bff(flat).ok);
    CHECK(edgeLengthError(flat) < 1e-9);

    geo::CylinderOptions co;
    co.caps = false;
    co.subdivAxis = 32;
    co.subdivHeight = 6;
    geo::Mesh cyl = geo::makeCylinder(co);
    std::vector<Index> half = facesWhere(cyl, [](const glm::dvec3& c) { return c.x > 0.0; });
    uv::Chart patch = chartOf(cyl, half);
    REQUIRE(uv::bff(patch).ok);
    CHECK(edgeLengthError(patch) < 1e-8);
    CHECK(uv::foldedTriangles(patch) == 0);
}

TEST_CASE("bff polygon boundary gives a rectangle with straight sides")
{
    std::vector<Index> faces;
    geo::Mesh m = hemisphere(32, faces);
    for (bool chooseCorners : {false, true})
    {
        uv::Chart ch = chartOf(m, faces);
        uv::BffOptions o;
        o.boundary = uv::BffBoundary::Polygon;
        if (chooseCorners)
        {
            const auto loop = ch.boundaryLoops().front();
            for (size_t i = 0; i < 4; ++i)
                o.corners.push_back(loop[i * loop.size() / 4 + 1]);
        }
        REQUIRE(uv::bff(ch, o).ok);
        CHECK(uv::foldedTriangles(ch) == 0);
        glm::dvec2 lo = ch.uvMin(), hi = ch.uvMax();
        double size = std::max(hi.x - lo.x, hi.y - lo.y), worst = 0.0;
        const std::vector<Index> border = ch.boundaryLoops().front();
        for (Index v : border)
        {
            glm::dvec2 p = ch.uv[v];
            worst = std::max(worst, std::min({std::abs(p.x - lo.x), std::abs(p.x - hi.x), std::abs(p.y - lo.y), std::abs(p.y - hi.y)}) / size);
        }
        CAPTURE(chooseCorners);
        CHECK(worst < 1e-9);
        if (chooseCorners)
            for (Index c : o.corners)
            {
                glm::dvec2 p = ch.uv[c];
                bool corner = (std::abs(p.x - lo.x) < 1e-9 * size || std::abs(p.x - hi.x) < 1e-9 * size) &&
                              (std::abs(p.y - lo.y) < 1e-9 * size || std::abs(p.y - hi.y) < 1e-9 * size);
                CHECK(corner);
            }
    }
}

TEST_CASE("bff exact lengths keeps every boundary edge")
{
    std::vector<Index> faces;
    geo::Mesh m = hemisphere(32, faces);
    uv::Chart ch = chartOf(m, faces);
    uv::BffOptions o;
    o.exactLengths = true;
    REQUIRE(uv::bff(ch, o).ok);
    const auto loop = ch.boundaryLoops().front();
    double worst = 0.0;
    for (size_t i = 0; i < loop.size(); ++i)
    {
        Index a = loop[i], b = loop[(i + 1) % loop.size()];
        double l3 = glm::length(ch.pos[b] - ch.pos[a]);
        worst = std::max(worst, std::abs(glm::length(ch.uv[b] - ch.uv[a]) - l3) / l3);
    }
    CHECK(worst < 1e-9);
    CHECK(uv::foldedTriangles(ch) == 0);
}

TEST_CASE("bff cones reduce area distortion")
{
    SUBCASE("open box: cones at the bottom corners make it a net")
    {
        geo::CubeOptions co;
        co.subdivX = co.subdivY = co.subdivZ = 6;
        geo::Mesh m = geo::makeCube(co);
        std::vector<Index> faces = facesWhere(m, [](const glm::dvec3& c) { return c.y < 49.0; });
        uv::Chart plain = chartOf(m, faces);
        REQUIRE(uv::bff(plain).ok);
        uv::Chart coned = chartOf(m, faces);
        uv::BffOptions o;
        o.autoCones = 4;
        auto t0 = std::chrono::steady_clock::now();
        uv::BffResult r = uv::bff(coned, o);
        REQUIRE(r.ok);
        double a0 = uv::chartStats(plain).area, a1 = uv::chartStats(coned).area;
        MESSAGE("open box area distortion " << a0 << " -> " << a1 << " with " << r.cones.size() << " cones, " << seconds(t0) * 1000.0 << " ms");
        CHECK(r.cones.size() == 4);
        CHECK(a1 < 0.5 * a0);
        CHECK(uv::foldedTriangles(coned) == 0);
    }
    SUBCASE("sphere without its cap")
    {
        geo::SphereOptions so;
        so.radius = 1.0;
        so.subdivAxis = 48;
        so.subdivHeight = 32;
        geo::Mesh m = geo::makeSphere(so);
        std::vector<Index> faces = facesWhere(m, [](const glm::dvec3& c) { return c.y < 0.7; });
        uv::Chart plain = chartOf(m, faces);
        REQUIRE(uv::bff(plain).ok);
        uv::Chart coned = chartOf(m, faces);
        uv::BffOptions o;
        o.autoCones = 6;
        uv::BffResult r = uv::bff(coned, o);
        REQUIRE(r.ok);
        double a0 = uv::chartStats(plain).area, a1 = uv::chartStats(coned).area;
        MESSAGE("capped sphere area distortion " << a0 << " -> " << a1 << " with " << r.cones.size() << " cones");
        CHECK(a1 < 0.6 * a0);
    }
}

TEST_CASE("bff as an unwrap method")
{
    std::vector<Index> faces;
    geo::Mesh m = hemisphere(24, faces);
    uv::Chart ch = chartOf(m, faces);
    uv::UnwrapOptions uo;
    uo.method = uv::UnwrapMethod::Bff;
    uo.usePins = false;
    uv::UnwrapResult r = uv::unwrapChart(ch, uo);
    CHECK(r.ok);
    CHECK(uv::foldedTriangles(ch) == 0);
    uv::Chart ref = chartOf(m, faces);
    REQUIRE(uv::lscm(ref, false));
    CHECK(uv::chartStats(ch).angle < 1.5 * uv::chartStats(ref).angle);
}

TEST_CASE("optimise modes trade angle against area")
{
    std::vector<Index> faces;
    geo::Mesh m = hemisphere(24, faces);
    uv::Chart start = chartOf(m, faces);
    REQUIRE(uv::lscm(start, false));
    uv::ChartStats s0 = uv::chartStats(start);

    uv::Chart area = start;
    uv::OptimizeOptions o;
    o.mode = uv::OptimizeMode::Authalic;
    o.iterations = 60;
    uv::OptimizeResult ra = uv::optimizeChart(area, o);
    REQUIRE(ra.ok);
    MESSAGE("authalic: area " << s0.area << " -> " << ra.after.area << ", angle " << s0.angle << " -> " << ra.after.angle);
    CHECK(ra.after.area < 0.5 * s0.area);
    CHECK(ra.after.folded == 0);

    uv::Chart conf = area;
    o.mode = uv::OptimizeMode::Conformal;
    uv::OptimizeResult rc = uv::optimizeChart(conf, o);
    REQUIRE(rc.ok);
    MESSAGE("conformal: angle " << ra.after.angle << " -> " << rc.after.angle);
    CHECK(rc.after.angle < 0.5 * ra.after.angle);
    CHECK(rc.after.folded == 0);

    uv::Chart blend = start;
    o.mode = uv::OptimizeMode::Blend;
    o.mix = 0.5;
    uv::OptimizeResult rb = uv::optimizeChart(blend, o);
    REQUIRE(rb.ok);
    MESSAGE("blend: area " << rb.after.area << ", angle " << rb.after.angle);
    CHECK(rb.after.area < s0.area);
    CHECK(rb.after.angle < ra.after.angle);
    CHECK(rb.after.folded == 0);

    uv::Chart iso = start;
    o.mode = uv::OptimizeMode::Isometric;
    CHECK(uv::optimizeChart(iso, o).ok);
    CHECK(uv::chartStats(iso).folded == 0);
    // Area and placement are the island's own.
    CHECK(std::abs(iso.areaUv()) == doctest::Approx(std::abs(start.areaUv())).epsilon(1e-6));
}

TEST_CASE("mosaic splits an island that cannot flatten")
{
    std::vector<Index> faces;
    geo::Mesh m = hemisphere(24, faces);
    clearSeams(m);
    uv::Chart ch = chartOf(m, faces);
    REQUIRE(uv::lscm(ch, false));
    std::vector<uv::Chart> one{ch};
    uv::writeCharts(m, one, "uv0");
    uv::MosaicOptions o;
    o.threshold = 0.03;
    o.visibility = false;
    uv::MosaicResult r = uv::mosaicFaces(m, faces, "uv0", o);
    CHECK(r.islandsBefore == 1);
    CHECK(r.split == 1);
    CHECK(r.islandsAfter > 1);
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    std::vector<uv::Chart> after = uv::buildCharts(m, faces, co);
    CHECK(after.size() == r.islandsAfter);
    for (const uv::Chart& c : after)
        CHECK(uv::foldedTriangles(c) == 0);
}

TEST_CASE("optcuts gets under the bound with shorter seams than a naive cut")
{
    struct Case
    {
        const char* name;
        geo::Mesh mesh;
    };
    geo::SphereOptions so;
    so.radius = 1.0;
    so.subdivAxis = 32;
    so.subdivHeight = 24;
    geo::TorusOptions to;
    to.radius = 1.0;
    to.sectionRadius = 0.35;
    to.subdivAxis = 36;
    to.subdivHeight = 16;
    Case cases[] = {{"sphere", geo::makeSphere(so)}, {"torus", geo::makeTorus(to)}, {"blob", blob()}};
    for (Case& c : cases)
    {
        CAPTURE(std::string(c.name));
        clearSeams(c.mesh);
        geo::Mesh naive = c.mesh;
        double naiveDistortion = 0.0;
        double naiveLength = naiveCut(naive, &naiveDistortion);

        uv::AutoSeamOptions o;
        o.method = uv::SeamMethod::OptCuts;
        o.distortionBound = 0.05;
        o.visibility = false;
        auto t0 = std::chrono::steady_clock::now();
        uv::OptCutsResult r = uv::optCuts(c.mesh, {}, o);
        double t = seconds(t0);
        MESSAGE(std::string(c.name) << ": " << c.mesh.numFaces() << " faces, " << t << " s, distortion " << r.distortion << ", seams " << r.seamLength << " ("
                       << r.splits << " splits, " << r.merges << " zipped, " << r.seams.charts.size() << " islands); naive cut " << naiveLength
                       << " at distortion " << naiveDistortion);
        CHECK(r.reachedBound);
        CHECK(r.distortion <= 0.05 + 1e-9);
        CHECK(r.seamLength < naiveLength);
        for (const uv::Chart& ch : r.seams.charts)
            CHECK(uv::foldedTriangles(ch) == 0);
    }
}

TEST_CASE("symmetric unwrap mirrors exactly")
{
    for (uv::MirrorUvMode mode : {uv::MirrorUvMode::Flip, uv::MirrorUvMode::Stack})
    {
        CAPTURE(int(mode));
        geo::SphereOptions so;
        so.radius = 1.0;
        so.subdivAxis = 24;
        so.subdivHeight = 12;
        geo::Mesh m = geo::makeSphere(so);
        clearSeams(m);
        uv::AutoSeamOptions ao;
        ao.visibility = false;
        ao.maxDistortion = 0.05;
        uv::autoSeams(m, {}, ao);
        uv::SymmetryUvOptions o;
        o.axis = 0;
        o.mode = mode;
        uv::SymmetryUvResult r = uv::unwrapSymmetric(m, {}, "uv0", o);
        MESSAGE("islands " << r.islands << ", pairs " << r.pairs << ", across the plane " << r.selfSymmetric << ", alone " << r.asymmetric);
        CHECK(r.asymmetric == 0);
        CHECK(r.pairs + r.selfSymmetric > 0);

        geo::SymmetryMap map = geo::SymmetryMap::persistent(m, 0, 1e-4);
        std::span<const glm::vec2> uvs = m.cornerAttrs().get<glm::vec2>("uv0");
        uv::ChartOptions co;
        co.source = uv::IslandSource::Uvs;
        std::vector<std::vector<Index>> islands = uv::islandFaces(m, {}, co);
        std::vector<Index> islandOf(m.faceCapacity());
        for (size_t i = 0; i < islands.size(); ++i)
            for (Index f : islands[i])
                islandOf[f] = Index(i);
        std::map<std::pair<Index, Index>, glm::dvec2> axisOf;
        double worst = 0.0;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            m.forEachFaceCorner(f, [&](Index c) {
                Index mf = map.mirrorFace(m, f);
                Index mc = m.findCorner(mf, map.mirrorPoint(m.cornerPoint(c)));
                REQUIRE(mc != geo::kInvalid);
                glm::dvec2 a = uvs[c], b = uvs[mc];
                if (mode == uv::MirrorUvMode::Stack && islandOf[f] != islandOf[mf])
                {
                    worst = std::max(worst, glm::length(a - b));
                    return;
                }
                // Mirrored about a vertical line, up to where packing put the partner.
                glm::dvec2 m2 = islandOf[f] <= islandOf[mf] ? glm::dvec2(a.x + b.x, b.y - a.y) : glm::dvec2(a.x + b.x, a.y - b.y);
                auto [it, fresh] = axisOf.try_emplace(std::minmax(islandOf[f], islandOf[mf]), m2);
                worst = std::max(worst, glm::length(m2 - it->second));
            });
        CHECK(worst < 1e-5);
        std::vector<uint8_t> flags;
        if (mode == uv::MirrorUvMode::Flip)
            CHECK(uv::overlappingFaces(m, "uv0", flags) == 0);
    }
}

TEST_CASE("straighten edges lays a chain on a line keeping its length")
{
    geo::PlaneOptions po;
    po.subdivX = 10;
    po.subdivZ = 10;
    geo::Mesh m = geo::makePlane(po);
    clearSeams(m);
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        glm::dvec3 q = m.position(p);
        m.setPosition(p, q + glm::dvec3(0.0, 12.0 * std::sin(q.x * 0.05) * std::cos(q.z * 0.04), 0.0));
    }
    uv::Chart ch = chartOf(m, uv::liveFaces(m));
    uv::UnwrapOptions uo;
    uo.usePins = false;
    REQUIRE(uv::unwrapChart(ch, uo).ok);
    // A row of edges across the middle, at z = 0.
    std::vector<uint8_t> sel(m.edgeCapacity(), 0);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && std::abs(m.position(m.edgePoint(e, 0)).z) < 1e-6 && std::abs(m.position(m.edgePoint(e, 1)).z) < 1e-6)
            sel[e] = 1;
    std::vector<Index> row;
    for (Index v = 0; v < ch.vertexCount(); ++v)
        if (std::abs(ch.pos[v].z) < 1e-6)
            row.push_back(v);
    std::sort(row.begin(), row.end(), [&](Index a, Index b) { return ch.pos[a].x < ch.pos[b].x; });
    auto arc = [&] {
        double s = 0.0;
        for (size_t i = 1; i < row.size(); ++i)
            s += glm::length(ch.uv[row[i]] - ch.uv[row[i - 1]]);
        return s;
    };
    double before = arc();
    REQUIRE(uv::straightenEdges(ch, m, sel) == 1);
    glm::dvec2 a = ch.uv[row.front()], d = glm::normalize(ch.uv[row.back()] - a);
    double off = 0.0;
    for (Index v : row)
    {
        glm::dvec2 q = ch.uv[v] - a;
        off = std::max(off, std::abs(q.x * d.y - q.y * d.x));
    }
    CHECK(off < 1e-9 * before);
    CHECK(arc() == doctest::Approx(before).epsilon(1e-12));
    CHECK(uv::foldedTriangles(ch) == 0);

    // Align: that row along V, then orient it to the nearest axis (U or V both fine, it is already on V).
    uv::transformChart(ch, ch.uvCentroid(), 0.4);
    REQUIRE(uv::alignToEdges(ch, m, sel, uv::AlignAxis::V));
    glm::dvec2 dir = ch.uv[row.back()] - ch.uv[row.front()];
    CHECK(std::abs(dir.x) < 1e-9 * glm::length(dir));
    REQUIRE(uv::alignToEdges(ch, m, sel, uv::AlignAxis::U));
    dir = ch.uv[row.back()] - ch.uv[row.front()];
    CHECK(std::abs(dir.y) < 1e-9 * glm::length(dir));
}

TEST_CASE("bff and optcuts timings on denser meshes")
{
    std::vector<Index> faces;
    geo::Mesh m = hemisphere(256, faces);
    uv::Chart ch = chartOf(m, faces);
    auto t0 = std::chrono::steady_clock::now();
    uv::BffSolver solver(ch);
    REQUIRE(solver.ok());
    double factor = seconds(t0);
    std::vector<double> u(solver.boundary().size(), 0.0);
    std::vector<glm::dvec2> uv;
    t0 = std::chrono::steady_clock::now();
    REQUIRE(solver.flatten(u, solver.dirichletToNeumann(u), false, uv));
    double solve = seconds(t0);
    MESSAGE("bff " << ch.tris.size() << " triangles: factorise " << factor * 1000.0 << " ms, each re-flatten " << solve * 1000.0 << " ms");

    geo::SphereOptions so;
    so.radius = 1.0;
    so.subdivAxis = 96;
    so.subdivHeight = 64;
    geo::Mesh sphere = geo::makeSphere(so);
    clearSeams(sphere);
    uv::AutoSeamOptions o;
    o.method = uv::SeamMethod::OptCuts;
    o.visibility = false;
    t0 = std::chrono::steady_clock::now();
    uv::OptCutsResult r = uv::optCuts(sphere, {}, o);
    MESSAGE("optcuts sphere " << sphere.numFaces() << " faces: " << seconds(t0) << " s, distortion " << r.distortion << ", " << r.splits << " splits, "
                              << r.merges << " zipped, seams " << r.seamLength);
    CHECK(r.reachedBound);
}
