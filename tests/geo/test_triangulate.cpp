#include "TestUtil.h"

#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "geo/Triangulate.h"

#include <cmath>
#include <map>
#include <random>
#include <set>

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
constexpr double kPi = 3.14159265358979323846;

Index triBudget(const Mesh& m)
{
    Index n = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            n += m.faceSize(f) - 2;
    return n;
}

std::vector<Index> allFaces(const Mesh& m)
{
    std::vector<Index> out;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            out.push_back(f);
    return out;
}

std::vector<Index> allEdges(const Mesh& m)
{
    std::vector<Index> out;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
            out.push_back(e);
    return out;
}

double cross2(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/** Segments ab and cd cross at a point interior to both. */
bool properCross(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c, const glm::dvec2& d, double eps)
{
    const double d1 = cross2(a, b, c), d2 = cross2(a, b, d), d3 = cross2(c, d, a), d4 = cross2(c, d, b);
    return ((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) && ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps));
}

double polyArea(const std::vector<glm::dvec2>& p)
{
    double a = 0.0;
    for (size_t i = 0; i < p.size(); ++i)
        a += p[i].x * p[(i + 1) % p.size()].y - p[(i + 1) % p.size()].x * p[i].y;
    return 0.5 * a;
}

/** Star-shaped around the origin, so simple; counter-clockwise. Sharp spikes when `jag`. */
std::vector<glm::dvec2> randomStar(std::mt19937& rng, int n, bool jag)
{
    std::uniform_real_distribution<double> r(jag ? 0.15 : 0.6, 1.0), jitter(-0.3, 0.3);
    std::vector<glm::dvec2> p;
    for (int i = 0; i < n; ++i)
    {
        const double t = (i + 0.5 + jitter(rng)) * 2.0 * kPi / n;
        const double rad = jag && i % 2 ? 0.2 * r(rng) : r(rng);
        p.push_back({rad * std::cos(t), rad * std::sin(t)});
    }
    return p;
}

/** Splits random edges with collinear points, like bevels and loop inserts leave. */
std::vector<glm::dvec2> addCollinear(std::mt19937& rng, const std::vector<glm::dvec2>& in, int extra)
{
    std::vector<glm::dvec2> p = in;
    for (int k = 0; k < extra; ++k)
    {
        const size_t i = rng() % p.size();
        const glm::dvec2 a = p[i], b = p[(i + 1) % p.size()];
        const int pieces = 1 + int(rng() % 3);
        std::vector<glm::dvec2> mids;
        for (int j = 1; j <= pieces; ++j)
            mids.push_back(glm::mix(a, b, double(j) / (pieces + 1)));
        p.insert(p.begin() + i + 1, mids.begin(), mids.end());
    }
    return p;
}

/** Comb: deep teeth with collinear points along the spine. Counter-clockwise. */
std::vector<glm::dvec2> comb(int teeth)
{
    std::vector<glm::dvec2> p;
    for (int x = 0; x < teeth * 2; ++x)
        p.push_back({double(x), 0.0});
    for (int k = teeth - 1; k >= 0; --k)
    {
        p.push_back({2.0 * k + 1.0, 10.0});
        p.push_back({2.0 * k, 10.0});
        if (k > 0)
        {
            p.push_back({2.0 * k, 1.0});
            p.push_back({2.0 * k - 1.0, 1.0});
        }
    }
    return p;
}

/** One-face mesh of a 2D polygon placed on a tilted plane, optionally bent out of it. */
Mesh polygonMesh(const std::vector<glm::dvec2>& poly, double bend = 0.0)
{
    const glm::dvec3 u = glm::normalize(glm::dvec3(1.0, 0.3, -0.2));
    const glm::dvec3 v = glm::normalize(glm::cross(glm::dvec3(0.2, 0.5, 1.0), u));
    const glm::dvec3 n = glm::cross(u, v);
    Mesh m;
    std::vector<Index> pts;
    for (size_t i = 0; i < poly.size(); ++i)
        pts.push_back(m.addPoint(glm::dvec3(500, -200, 80) + u * poly[i].x * 100.0 + v * poly[i].y * 100.0
                                 + n * (bend * 100.0 * std::sin(double(i) * 1.7))));
    m.addFace(pts);
    return m;
}

struct Checked
{
    bool ok = true;
    double area = 0.0;
};

/** Validates one face's triangles against its 2D outline: count, coverage, orientation, no overlaps. */
Checked checkTriangles(const std::vector<glm::dvec2>& poly, const std::vector<Index>& local, bool allowSlivers = false)
{
    Checked r;
    const size_t n = poly.size();
    if (local.size() != 3 * (n - 2))
    {
        MESSAGE("expected " << n - 2 << " triangles, got " << local.size() / 3);
        r.ok = false;
        return r;
    }
    double ext = 0.0;
    for (const glm::dvec2& p : poly)
        ext = std::max({ext, std::abs(p.x), std::abs(p.y)});
    const double eps = 1e-9 * ext * ext;
    std::set<std::pair<Index, Index>> segs;
    for (size_t t = 0; t < local.size(); t += 3)
    {
        const double a = cross2(poly[local[t]], poly[local[t + 1]], poly[local[t + 2]]);
        if (a < (allowSlivers ? -eps : eps))
        {
            MESSAGE("triangle " << t / 3 << " has signed area " << a);
            r.ok = false;
        }
        r.area += 0.5 * a;
        for (int k = 0; k < 3; ++k)
        {
            const Index p = local[t + k], q = local[t + (k + 1) % 3];
            segs.insert({std::min(p, q), std::max(p, q)});
        }
    }
    std::vector<std::pair<Index, Index>> list(segs.begin(), segs.end());
    for (size_t i = 0; i < list.size() && r.ok; ++i)
        for (size_t j = i + 1; j < list.size(); ++j)
        {
            auto [a, b] = list[i];
            auto [c, d] = list[j];
            if (a == c || a == d || b == c || b == d)
                continue;
            if (properCross(poly[a], poly[b], poly[c], poly[d], eps))
            {
                MESSAGE("edges " << a << "-" << b << " and " << c << "-" << d << " cross");
                r.ok = false;
                break;
            }
        }
    if (std::abs(r.area - polyArea(poly)) > 1e-9 * std::max(1.0, std::abs(polyArea(poly))))
    {
        MESSAGE("area " << r.area << " vs polygon " << polyArea(poly));
        r.ok = false;
    }
    return r;
}

/** Face triangles as local corner indices, via the mesh path that display and export use. */
std::vector<Index> meshTriangles(const Mesh& m, const TriangulateOptions& opts = {})
{
    std::vector<Index> corners;
    triangulateFace(m, 0, corners, opts);
    const std::vector<Index> order = m.faceCorners(0);
    std::vector<Index> local;
    for (Index c : corners)
        local.push_back(Index(std::find(order.begin(), order.end(), c) - order.begin()));
    return local;
}

double minAngle(const std::vector<glm::dvec2>& poly, const std::vector<Index>& local)
{
    double best = 1e9;
    for (size_t t = 0; t < local.size(); t += 3)
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec2 a = poly[local[t + k]], b = poly[local[t + (k + 1) % 3]], c = poly[local[t + (k + 2) % 3]];
            best = std::min(best, std::acos(std::clamp(glm::dot(glm::normalize(b - a), glm::normalize(c - a)), -1.0, 1.0)));
        }
    return best;
}

/** Points of every face of the mesh, as sorted triples, to compare triangulations independent of ids. */
std::multiset<std::array<Index, 3>> pointTriples(const Mesh& m, const std::vector<Index>& cornerTris)
{
    std::multiset<std::array<Index, 3>> out;
    for (size_t t = 0; t < cornerTris.size(); t += 3)
    {
        std::array<Index, 3> a = {m.cornerPoint(cornerTris[t]), m.cornerPoint(cornerTris[t + 1]), m.cornerPoint(cornerTris[t + 2])};
        std::rotate(a.begin(), std::min_element(a.begin(), a.end()), a.end());
        out.insert(a);
    }
    return out;
}

void checkMeshTriangulation(Mesh m, const char* what)
{
    INFO(what);
    m.compact();
    const Index pts = m.numPoints(), budget = triBudget(m);
    const Triangulation shown = triangulate(m);
    // Faces can be non-planar (round bevel corners), so the reference volume is that of the shown triangles.
    double volume = 0.0;
    for (size_t t = 0; t < shown.corners.size(); t += 3)
    {
        const glm::dvec3 a = m.position(m.cornerPoint(shown.corners[t])), b = m.position(m.cornerPoint(shown.corners[t + 1])),
                         c = m.position(m.cornerPoint(shown.corners[t + 2]));
        volume += glm::dot(a, glm::cross(b, c)) / 6.0;
    }
    const std::multiset<std::array<Index, 3>> shownTris = pointTriples(m, shown.corners);
    triangulateFaces(m, allFaces(m));
    CHECK(isValid(m));
    m.compact();
    CHECK(m.numPoints() == pts);
    CHECK(m.numFaces() == budget);
    CHECK(signedVolume(m) == doctest::Approx(volume).epsilon(1e-9));
    // What the viewport showed before is exactly what the command made.
    std::vector<Index> after;
    for (Index f : allFaces(m))
        m.forEachFaceCorner(f, [&](Index c) { after.push_back(c); });
    CHECK(pointTriples(m, after) == shownTris);
    for (Index f : allFaces(m))
        if (m.faceArea(f) < 1e-9)
        {
            MESSAGE("zero-area triangle " << f);
            CHECK(false);
            break;
        }
}
} // namespace

TEST_CASE("triangulate: random simple polygons")
{
    std::mt19937 rng(1234);
    int checked = 0;
    for (int iter = 0; iter < 400; ++iter)
    {
        const int n = 4 + int(rng() % 40);
        std::vector<glm::dvec2> poly = randomStar(rng, n, iter % 3 == 1);
        if (iter % 4 == 0)
            for (int i = 0; i < n; ++i)
                poly[i] = glm::dvec2(std::cos(i * 2.0 * kPi / n), std::sin(i * 2.0 * kPi / n));
        if (iter % 2 == 0)
            poly = addCollinear(rng, poly, 1 + int(rng() % 6));
        INFO("iteration " << iter << ", " << poly.size() << " points");
        for (bool beauty : {false, true})
        {
            TriangulateOptions o;
            o.beauty = beauty;
            const Mesh m = polygonMesh(poly);
            const std::vector<Index> local = meshTriangles(m, o);
            CHECK(checkTriangles(poly, local).ok);
            CHECK(meshTriangles(m, o) == local);
            ++checked;
        }
    }
    CHECK(checked == 800);
}

TEST_CASE("triangulate: hard shapes")
{
    // Comb with deep teeth and collinear spine points.
    std::vector<glm::dvec2> c = comb(6);
    CHECK(checkTriangles(c, meshTriangles(polygonMesh(c))).ok);

    // Square whose sides carry many collinear points, the bevel n-gon.
    std::vector<glm::dvec2> sq;
    for (int side = 0; side < 4; ++side)
        for (int k = 0; k < 7; ++k)
        {
            const double t = k / 7.0;
            const glm::dvec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            sq.push_back(glm::mix(corners[side], corners[(side + 1) % 4], t));
        }
    std::vector<Index> local = meshTriangles(polygonMesh(sq));
    CHECK(checkTriangles(sq, local).ok);
    // A fan from one corner would leave slivers along its two sides.
    CHECK(minAngle(sq, local) > glm::radians(5.0));

    // Same, starting mid-side so the first corner is flat.
    std::rotate(sq.begin(), sq.begin() + 3, sq.end());
    CHECK(checkTriangles(sq, meshTriangles(polygonMesh(sq))).ok);

    // Nearly degenerate: a long thin rectangle with collinear points.
    std::vector<glm::dvec2> thin = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {3, 1e-4}, {1.5, 1e-4}, {0, 1e-4}};
    CHECK(checkTriangles(thin, meshTriangles(polygonMesh(thin))).ok);

    // Non-planar n-gon: triangles still cover the projected outline.
    std::mt19937 rng(7);
    for (int i = 0; i < 20; ++i)
    {
        std::vector<glm::dvec2> star = addCollinear(rng, randomStar(rng, 12, i % 2 == 0), 3);
        const Mesh m = polygonMesh(star, 0.02);
        const std::vector<Index> tris = meshTriangles(m);
        CHECK(tris.size() == 3 * (star.size() - 2));
        std::vector<Index> corners;
        triangulateFace(m, 0, corners);
        const glm::dvec3 n = m.faceNormal(0);
        for (size_t t = 0; t < corners.size(); t += 3)
        {
            const glm::dvec3 a = m.position(m.cornerPoint(corners[t])), b = m.position(m.cornerPoint(corners[t + 1])),
                             d = m.position(m.cornerPoint(corners[t + 2]));
            CHECK(glm::dot(glm::cross(b - a, d - a), n) > 0.0);
        }
    }

    // Beauty flips improve the smallest angle of a regular-ish polygon fan.
    std::vector<glm::dvec2> oval;
    for (int i = 0; i < 24; ++i)
        oval.push_back({3.0 * std::cos(i * 2.0 * kPi / 24), std::sin(i * 2.0 * kPi / 24)});
    TriangulateOptions plain;
    plain.beauty = false;
    const Mesh om = polygonMesh(oval);
    CHECK(minAngle(oval, meshTriangles(om)) >= minAngle(oval, meshTriangles(om, plain)) - 1e-12);
}

TEST_CASE("triangulate: quads")
{
    // Shorter diagonal 1-3.
    std::vector<glm::dvec2> kite = {{0, 0}, {2, -0.5}, {4, 0}, {2, 0.5}};
    std::vector<Index> t = meshTriangles(polygonMesh(kite));
    CHECK(t == std::vector<Index>{1, 2, 3, 1, 3, 0});
    TriangulateOptions first;
    first.quads = QuadSplit::FirstCorner;
    CHECK(meshTriangles(polygonMesh(kite), first) == std::vector<Index>{0, 1, 2, 0, 2, 3});

    // Concave dart: the shorter diagonal 1-3 runs outside, so the split takes 0-2.
    std::vector<glm::dvec2> dart = {{0, 0}, {2, 1}, {0, 0.2}, {-2, 1}};
    std::rotate(dart.begin(), dart.begin() + 1, dart.end());
    t = meshTriangles(polygonMesh(dart));
    CHECK(checkTriangles(dart, t).ok);

    // Non-planar quad folded: both triangles still face the quad's normal.
    Mesh m;
    const Index a = m.addPoint({0, 0, 0}), b = m.addPoint({100, 0, 0}), c = m.addPoint({100, 100, 30}), d = m.addPoint({0, 100, -30});
    m.addFace({a, b, c, d});
    std::vector<Index> corners;
    triangulateFace(m, 0, corners);
    REQUIRE(corners.size() == 6);
    for (size_t i = 0; i < 6; i += 3)
    {
        const glm::dvec3 p = m.position(m.cornerPoint(corners[i])), q = m.position(m.cornerPoint(corners[i + 1])),
                         r = m.position(m.cornerPoint(corners[i + 2]));
        CHECK(glm::dot(glm::cross(q - p, r - p), m.faceNormal(0)) > 0.0);
    }
}

TEST_CASE("triangulate: beveled cube and primitives")
{
    BevelOptions bo;
    bo.offset = 10.0;
    for (int seg : {1, 2, 3})
    {
        bo.segments = seg;
        Mesh all = makeCube();
        bevelEdges(all, allEdges(all), bo);
        checkMeshTriangulation(all, "all edges");

        // The edges around one face and one corner leave n-gons with collinear bevel points on the neighbours.
        for (std::vector<Index> sel : {std::vector<Index>{0, 1, 2, 3}, std::vector<Index>{0}, std::vector<Index>{0, 5, 9}})
        {
            Mesh m = makeCube();
            bevelEdges(m, sel, bo);
            checkMeshTriangulation(m, "selected edges");
        }

        // With symmetry the stored layer rides along untouched.
        Mesh sym = makeCube();
        SymmetryMap::persistent(sym, 0, 1e-4);
        bo.symmetry = nullptr;
        bevelEdges(sym, std::vector<Index>{0, 1, 2, 3}, bo);
        sym.compact();
        SymmetryMap::persistent(sym, 0, 1e-4);
        const std::string layer = symmetryLayerName(0);
        const std::vector<int32_t> keys(sym.pointAttrs().get<int32_t>(layer).begin(), sym.pointAttrs().get<int32_t>(layer).end());
        triangulateFaces(sym, allFaces(sym));
        sym.compact();
        std::span<const int32_t> after = std::as_const(sym).pointAttrs().get<int32_t>(layer);
        CHECK(std::vector<int32_t>(after.begin(), after.end()) == keys);
    }
    checkMeshTriangulation(makeSphere(), "sphere");
    checkMeshTriangulation(makeCylinder(), "cylinder");
    checkMeshTriangulation(makeCone(), "cone");
    checkMeshTriangulation(makeCube({100, 100, 100, 3, 2, 4}), "subdivided cube");
}

TEST_CASE("triangulate: attributes")
{
    Mesh m = makeCylinder();
    m.compact();
    std::span<float> crease = m.edgeAttrs().getOrAdd<float>(attr::kCrease);
    std::span<float> weight = m.edgeAttrs().getOrAdd<float>(attr::kBevelWeight);
    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(attr::kHard);
    std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(attr::kSeam);
    std::map<std::pair<Index, Index>, std::tuple<float, float, bool, bool>> edgeFlags;
    for (Index e : allEdges(m))
    {
        crease[e] = float(e % 5) * 0.25f;
        weight[e] = float(e % 3) * 0.5f;
        hard[e] = e % 2 == 0;
        seam[e] = e % 7 == 0;
        const Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
        edgeFlags[{std::min(a, b), std::max(a, b)}] = {crease[e], weight[e], hard[e], seam[e]};
    }
    std::span<glm::vec4> color = m.cornerAttrs().getOrAdd<glm::vec4>(attr::kColor);
    std::span<glm::vec2> uv1 = m.cornerAttrs().getOrAdd<glm::vec2>(attr::uvName(1));
    std::span<int32_t> mat = m.faceAttrs().getOrAdd<int32_t>(attr::kMaterial);
    // Per (face, point): the corner values the triangles must keep.
    std::map<Index, std::map<Index, std::tuple<glm::vec2, glm::vec2, glm::vec4>>> byFace;
    std::map<Index, int32_t> faceMat;
    std::span<const glm::vec2> uv0 = std::as_const(m).cornerAttrs().get<glm::vec2>(attr::uvName(0));
    for (Index f : allFaces(m))
    {
        mat[f] = int32_t(f % 4);
        faceMat[f] = mat[f];
        m.forEachFaceCorner(f, [&](Index c) {
            color[c] = glm::vec4(float(c), float(f), 0.5f, 1.0f);
            uv1[c] = glm::vec2(float(c) * 0.01f, float(f) * 0.02f);
            byFace[f][m.cornerPoint(c)] = {uv0[c], uv1[c], color[c]};
        });
    }
    const Index pts = m.numPoints();
    triangulateFaces(m, allFaces(m));
    m.compact();
    CHECK(m.numPoints() == pts);

    std::span<const glm::vec2> nu0 = std::as_const(m).cornerAttrs().get<glm::vec2>(attr::uvName(0));
    std::span<const glm::vec2> nu1 = std::as_const(m).cornerAttrs().get<glm::vec2>(attr::uvName(1));
    std::span<const glm::vec4> ncol = std::as_const(m).cornerAttrs().get<glm::vec4>(attr::kColor);
    std::span<const int32_t> nmat = std::as_const(m).faceAttrs().get<int32_t>(attr::kMaterial);
    int mismatches = 0;
    for (Index f : allFaces(m))
        m.forEachFaceCorner(f, [&](Index c) {
            // The colour's y holds the source face.
            const Index src = Index(ncol[c].y);
            auto it = byFace[src].find(m.cornerPoint(c));
            if (it == byFace[src].end() || std::get<0>(it->second) != nu0[c] || std::get<1>(it->second) != nu1[c]
                || std::get<2>(it->second) != ncol[c] || nmat[f] != faceMat[src])
                ++mismatches;
        });
    CHECK(mismatches == 0);

    std::span<const float> ncrease = std::as_const(m).edgeAttrs().get<float>(attr::kCrease);
    std::span<const float> nweight = std::as_const(m).edgeAttrs().get<float>(attr::kBevelWeight);
    std::span<const bool> nhard = std::as_const(m).edgeAttrs().get<bool>(attr::kHard);
    std::span<const bool> nseam = std::as_const(m).edgeAttrs().get<bool>(attr::kSeam);
    int kept = 0, wrong = 0, diagonalsFlagged = 0;
    for (Index e : allEdges(m))
    {
        const Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
        auto it = edgeFlags.find({std::min(a, b), std::max(a, b)});
        if (it == edgeFlags.end())
        {
            diagonalsFlagged += nhard[e] || nseam[e] || ncrease[e] != 0.0f || nweight[e] != 0.0f;
            continue;
        }
        ++kept;
        wrong += std::tuple{ncrease[e], nweight[e], nhard[e], nseam[e]} != it->second;
    }
    CHECK(kept == Index(edgeFlags.size()));
    CHECK(wrong == 0);
    CHECK(diagonalsFlagged == 0);
}

TEST_CASE("triangulate: deterministic and parallel-safe")
{
    Mesh m = makeCube({100, 100, 100, 4, 4, 4});
    BevelOptions bo;
    bo.offset = 5.0;
    bo.segments = 3;
    bevelEdges(m, std::vector<Index>{0, 3, 7, 11, 20}, bo);
    m.compact();
    const Triangulation a = triangulate(m), b = triangulate(m);
    CHECK(a.corners == b.corners);
    std::vector<Index> serial;
    for (Index f : allFaces(m))
        triangulateFace(m, f, serial);
    CHECK(serial == a.corners);
}

TEST_CASE("triangulate: large n-gons")
{
    std::mt19937 rng(99);
    std::vector<glm::dvec2> circle;
    for (int i = 0; i < 2000; ++i)
        circle.push_back({std::cos(i * 2.0 * kPi / 2000), std::sin(i * 2.0 * kPi / 2000)});
    std::vector<glm::dvec2> star = addCollinear(rng, randomStar(rng, 400, true), 50);
    for (const std::vector<glm::dvec2>* poly : std::array{&circle, &star})
    {
        const Mesh m = polygonMesh(*poly);
        std::vector<Index> corners;
        triangulateFace(m, 0, corners);
        CHECK(corners.size() == 3 * (poly->size() - 2));
    }
    CHECK(checkTriangles(star, meshTriangles(polygonMesh(star))).ok);
}
