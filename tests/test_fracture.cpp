#include <doctest/doctest.h>

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "geo/Triangulate.h"
#include "scene/Fracture.h"

#include <tbb/global_control.h>

#include <chrono>
#include <cstring>
#include <map>

using namespace lila;
namespace fr = lila::scene::fracture;

namespace
{
double meshVolume(const geo::Mesh& m)
{
    const geo::Triangulation tris = geo::triangulate(m);
    double v = 0.0;
    for (size_t t = 0; t < tris.faces.size(); ++t)
    {
        const glm::dvec3 a = m.position(m.cornerPoint(tris.corners[t * 3]));
        const glm::dvec3 b = m.position(m.cornerPoint(tris.corners[t * 3 + 1]));
        const glm::dvec3 c = m.position(m.cornerPoint(tris.corners[t * 3 + 2]));
        v += glm::dot(a, glm::cross(b, c));
    }
    return v / 6.0;
}

/** Every edge has exactly two faces and the winding agrees. */
bool closedManifold(const geo::Mesh& m)
{
    if (!m.validate(geo::kValidateDefault).empty())
        return false;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
            return false;
    return m.numFaces() > 0;
}

uint64_t mix(uint64_t h, const void* data, size_t size)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint64_t hashMesh(const geo::Mesh& m, uint64_t h)
{
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        h = mix(h, &m.position(p), sizeof(glm::dvec3));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        for (geo::Index p : m.facePoints(f))
            h = mix(h, &p, sizeof(p));
    }
    for (int d = 0; d < int(geo::Domain::Count); ++d)
    {
        const geo::AttributeSet& set = m.attributes(geo::Domain(d));
        for (size_t l = 0; l < set.layerCount(); ++l)
        {
            const geo::Attribute& a = set.layer(l);
            h = mix(h, a.name().data(), a.name().size());
            for (size_t i = 0; i < a.size(); ++i)
                h = mix(h, a.raw(i), a.stride());
        }
    }
    for (const std::string& n : m.materialNames())
        h = mix(h, n.data(), n.size());
    return h;
}

uint64_t hashResult(const fr::FractureResult& r)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const fr::Piece& p : r.pieces)
    {
        h = hashMesh(p.mesh, h);
        h = mix(h, &p.cell, sizeof(p.cell));
        h = mix(h, &p.volume, sizeof(p.volume));
        h = mix(h, &p.centroid, sizeof(p.centroid));
    }
    for (const fr::Contact& c : r.contacts)
    {
        h = mix(h, &c.a, sizeof(c.a));
        h = mix(h, &c.b, sizeof(c.b));
        h = mix(h, &c.area, sizeof(c.area));
        h = mix(h, &c.centroid, sizeof(c.centroid));
    }
    return h;
}

void checkPieces(const fr::FractureResult& r, double sourceVolume, double tolerance)
{
    REQUIRE(r.ok());
    REQUIRE(!r.pieces.empty());
    double reported = 0.0, measured = 0.0;
    int open = 0;
    for (const fr::Piece& p : r.pieces)
    {
        open += !closedManifold(p.mesh);
        reported += p.volume;
        measured += meshVolume(p.mesh);
        CHECK(p.volume > 0.0);
    }
    CHECK(open == 0);
    CHECK(std::abs(reported - sourceVolume) <= tolerance * sourceVolume);
    CHECK(std::abs(measured - sourceVolume) <= tolerance * sourceVolume);
}

/** Inside-face area of piece i toward cell. */
double sideArea(const fr::Piece& p, int cell)
{
    auto inside = p.mesh.faceAttrs().get<int32_t>(fr::kInsideAttr);
    auto neighbor = p.mesh.faceAttrs().get<int32_t>(fr::kNeighborAttr);
    double area = 0.0;
    for (geo::Index f = 0; f < p.mesh.faceCapacity(); ++f)
        if (p.mesh.faceValid(f) && inside[f] && neighbor[f] == cell)
            area += p.mesh.faceArea(f);
    return area;
}

void checkContacts(const fr::FractureResult& r)
{
    REQUIRE(!r.contacts.empty());
    int asymmetric = 0;
    for (const fr::Contact& c : r.contacts)
    {
        REQUIRE(c.a < c.b);
        REQUIRE(c.b < int(r.pieces.size()));
        CHECK(c.area > 0.0);
        const double ab = sideArea(r.pieces[size_t(c.a)], r.pieces[size_t(c.b)].cell);
        const double ba = sideArea(r.pieces[size_t(c.b)], r.pieces[size_t(c.a)].cell);
        if (std::abs(ab - ba) > 1e-4 * std::max(ab, ba) + 1e-6)
            ++asymmetric;
    }
    CHECK(asymmetric == 0);
}

fr::FractureResult cubeFracture(int count, uint64_t seed, const fr::FractureOptions& o = {})
{
    const geo::Mesh cube = geo::makeCube();
    fr::ScatterOptions so;
    so.count = count;
    so.seed = seed;
    return fr::voronoiFracture(cube, fr::scatterPoints(cube, so), o);
}
} // namespace

TEST_CASE("fracture: cube into 30 closed pieces")
{
    const geo::Mesh cube = geo::makeCube();
    const double volume = meshVolume(cube);
    REQUIRE(volume == doctest::Approx(1e6));
    fr::ScatterOptions so;
    so.count = 30;
    so.seed = 7;
    const std::vector<glm::dvec3> pts = fr::scatterPoints(cube, so);
    REQUIRE(pts.size() == 30);
    const fr::FractureResult r = fr::voronoiFracture(cube, pts, {});
    CHECK(r.pieces.size() == 30);
    checkPieces(r, volume, 1e-4);
    checkContacts(r);
    for (size_t i = 1; i < r.pieces.size(); ++i)
        CHECK(r.pieces[i - 1].cell < r.pieces[i].cell);

    // Outside faces keep the cube's uvs; inside faces are tagged with the interior slot.
    const geo::MeshBvh bvh(cube);
    auto srcUv = cube.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!srcUv.empty());
    int checked = 0, wrongUv = 0, insideFaces = 0, badInside = 0;
    for (const fr::Piece& p : r.pieces)
    {
        const geo::Mesh& m = p.mesh;
        auto inside = m.faceAttrs().get<int32_t>(fr::kInsideAttr);
        auto neighbor = m.faceAttrs().get<int32_t>(fr::kNeighborAttr);
        auto material = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
        auto uv = m.cornerAttrs().get<glm::vec2>("uv0");
        REQUIRE(!inside.empty());
        REQUIRE(!material.empty());
        REQUIRE(!uv.empty());
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            if (inside[f])
            {
                ++insideFaces;
                const std::string& slot = m.materialNames()[size_t(material[f])];
                badInside += neighbor[f] < 0 || slot.size() < 9 || slot.substr(slot.size() - 9) != "_Interior";
                continue;
            }
            const glm::dvec3 mid = m.faceCentroid(f);
            m.forEachFaceCorner(f, [&](geo::Index c) {
                const glm::dvec3 at = m.position(m.cornerPoint(c));
                geo::BvhClosest hit;
                REQUIRE(bvh.closestPoint(at + 0.01 * (mid - at), hit));
                const glm::vec2 expect = bvh.interpolate<glm::vec2>(srcUv, hit.triangle, hit.bary);
                const glm::vec2 got = uv[c];
                // The probe sits 1% toward the middle, so allow for the uv gradient across that.
                wrongUv += glm::length(got - expect) > 0.02f;
                ++checked;
            });
        }
    }
    CHECK(checked > 100);
    CHECK(wrongUv == 0);
    CHECK(insideFaces > 30);
    CHECK(badInside == 0);
}

TEST_CASE("fracture: byte-identical across runs and thread counts")
{
    fr::FractureOptions rough;
    rough.noiseAmplitude = 2.0;
    rough.noiseScale = 20.0;
    for (const fr::FractureOptions& o : {fr::FractureOptions{}, rough})
    {
        const uint64_t a = hashResult(cubeFracture(40, 3, o));
        const uint64_t b = hashResult(cubeFracture(40, 3, o));
        uint64_t serial = 0;
        {
            tbb::global_control limit(tbb::global_control::max_allowed_parallelism, 1);
            serial = hashResult(cubeFracture(40, 3, o));
        }
        CHECK(a == b);
        CHECK(a == serial);
    }
    const uint64_t other = hashResult(cubeFracture(40, 4));
    CHECK(other != hashResult(cubeFracture(40, 3)));
}

TEST_CASE("fracture: rough cuts stay closed and keep the volume")
{
    fr::FractureOptions o;
    o.noiseAmplitude = 3.0;
    o.noiseScale = 15.0;
    const fr::FractureResult r = cubeFracture(20, 11, o);
    checkPieces(r, 1e6, 1e-3);
    checkContacts(r);
    size_t insideTris = 0;
    for (const fr::Piece& p : r.pieces)
    {
        auto inside = p.mesh.faceAttrs().get<int32_t>(fr::kInsideAttr);
        for (geo::Index f = 0; f < p.mesh.faceCapacity(); ++f)
            insideTris += p.mesh.faceValid(f) && inside[f];
    }
    MESSAGE("rough: ", r.pieces.size(), " pieces, ", insideTris, " inside faces");
    CHECK(insideTris > r.pieces.size() * 50);

    // The amplitude is capped against the noise scale, so even an extreme request stays valid.
    o.noiseAmplitude = 50.0;
    o.noiseScale = 12.0;
    o.stretch = {0.4, 1.0, 1.0};
    checkPieces(cubeFracture(12, 5, o), 1e6, 1e-3);
}

TEST_CASE("fracture: non-convex sources")
{
    geo::TorusOptions to;
    to.subdivAxis = 32;
    to.subdivHeight = 16;
    const geo::Mesh torus = geo::makeTorus(to);
    const double volume = meshVolume(torus);
    REQUIRE(volume > 0.0);
    fr::ScatterOptions so;
    so.count = 25;
    so.seed = 2;
    const std::vector<glm::dvec3> pts = fr::scatterPoints(torus, so);
    REQUIRE(pts.size() == 25);
    const fr::FractureResult r = fr::voronoiFracture(torus, pts, {});
    checkPieces(r, volume, 1e-4);
    checkContacts(r);

    fr::FractureOptions rough;
    rough.noiseAmplitude = 1.5;
    rough.noiseScale = 10.0;
    rough.stretch = {1.0, 0.5, 1.0};
    checkPieces(fr::voronoiFracture(torus, pts, rough), volume, 1e-3);

    // Scatter points outside the source still cut it where their cells reach in.
    std::vector<glm::dvec3> wide;
    for (int i = 0; i < 27; ++i)
        wide.push_back(glm::dvec3((i % 3) - 1, (i / 3 % 3) - 1, (i / 9) - 1) * 60.0);
    checkPieces(fr::voronoiFracture(torus, wide, {}), volume, 1e-4);
}

TEST_CASE("fracture: cluster and refine")
{
    const fr::FractureResult base = cubeFracture(40, 9);
    REQUIRE(base.pieces.size() == 40);

    const fr::FractureResult clusters = fr::clusterPieces(base, 4.0, 1);
    MESSAGE("clusters: ", clusters.pieces.size());
    CHECK(clusters.pieces.size() >= 8);
    CHECK(clusters.pieces.size() <= 14);
    checkPieces(clusters, 1e6, 1e-4);
    checkContacts(clusters);
    int selfFacing = 0;
    for (const fr::Piece& p : clusters.pieces)
    {
        auto inside = p.mesh.faceAttrs().get<int32_t>(fr::kInsideAttr);
        auto neighbor = p.mesh.faceAttrs().get<int32_t>(fr::kNeighborAttr);
        for (geo::Index f = 0; f < p.mesh.faceCapacity(); ++f)
            selfFacing += p.mesh.faceValid(f) && inside[f] && neighbor[f] == p.cell;
    }
    CHECK(selfFacing == 0);
    CHECK(hashResult(clusters) == hashResult(fr::clusterPieces(base, 4.0, 1)));

    const fr::FractureResult coarse = cubeFracture(8, 9);
    const fr::FractureResult refined = fr::refinePieces(coarse, 6, 0.0, {});
    MESSAGE("refined: ", coarse.pieces.size(), " -> ", refined.pieces.size());
    CHECK(refined.pieces.size() > coarse.pieces.size() * 3);
    checkPieces(refined, 1e6, 1e-4);
    REQUIRE(!refined.contacts.empty());
    for (size_t i = 0; i < refined.pieces.size(); ++i)
        CHECK(refined.pieces[i].cell == int(i));
    CHECK(hashResult(refined) == hashResult(fr::refinePieces(coarse, 6, 0.0, {})));

    // Only the big pieces split.
    double largest = 0.0;
    for (const fr::Piece& p : coarse.pieces)
        largest = std::max(largest, p.volume);
    const fr::FractureResult some = fr::refinePieces(coarse, 6, largest * 0.999, {});
    CHECK(some.pieces.size() == coarse.pieces.size() - 1 + 6);
}

TEST_CASE("fracture: scatter")
{
    const geo::Mesh cube = geo::makeCube();
    fr::ScatterOptions so;
    so.count = 50;
    so.seed = 5;
    const std::vector<glm::dvec3> a = fr::scatterPoints(cube, so);
    CHECK(a.size() == 50);
    CHECK(a == fr::scatterPoints(cube, so));
    for (const glm::dvec3& p : a)
        CHECK(glm::all(glm::lessThanEqual(glm::abs(p), glm::dvec3(50.0))));
    so.seed = 6;
    CHECK(a != fr::scatterPoints(cube, so));

    so.surface = true;
    const std::vector<glm::dvec3> s = fr::scatterPoints(cube, so);
    CHECK(s.size() == 50);
    for (const glm::dvec3& p : s)
        CHECK(std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) == doctest::Approx(50.0));

    // Impacts pull points toward them.
    so.surface = false;
    so.count = 200;
    so.impacts = {{50.0, 50.0, 50.0}};
    so.impactRadius = 20.0;
    auto meanDistance = [&](double bias) {
        so.impactBias = bias;
        double sum = 0.0;
        for (const glm::dvec3& p : fr::scatterPoints(cube, so))
            sum += glm::length(p - so.impacts[0]);
        return sum / so.count;
    };
    CHECK(meanDistance(0.8) < 0.6 * meanDistance(0.0));

    const std::vector<glm::dvec3> glass = fr::radialPoints({0, 0, 0}, {0, 0, 1}, 40.0, 4, 10, 0.5, 1);
    CHECK(glass.size() == 41);
    for (const glm::dvec3& p : glass)
        CHECK(p.z == doctest::Approx(0.0));
    CHECK(glass == fr::radialPoints({0, 0, 0}, {0, 0, 1}, 40.0, 4, 10, 0.5, 1));
}

TEST_CASE("fracture: timing")
{
    for (int count : {200, 1000})
    {
        const geo::Mesh cube = geo::makeCube();
        fr::ScatterOptions so;
        so.count = count;
        const std::vector<glm::dvec3> pts = fr::scatterPoints(cube, so);
        const auto t0 = std::chrono::steady_clock::now();
        const fr::FractureResult r = fr::voronoiFracture(cube, pts, {});
        const auto t1 = std::chrono::steady_clock::now();
        fr::FractureOptions rough;
        rough.noiseAmplitude = 1.0;
        rough.noiseScale = 8.0;
        const fr::FractureResult rr = fr::voronoiFracture(cube, pts, rough);
        const auto t2 = std::chrono::steady_clock::now();
        CHECK(r.pieces.size() == size_t(count));
        CHECK(rr.ok());
        MESSAGE(count, " cells: flat ", std::chrono::duration<double, std::milli>(t1 - t0).count(), " ms, rough ",
                std::chrono::duration<double, std::milli>(t2 - t1).count(), " ms, ", r.contacts.size(), " contacts");
    }
}
