#include "scene/Fracture.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Triangulate.h"
#include "scene/Materials.h"

#include <manifold/manifold.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <numbers>
#include <optional>
#include <queue>
#include <tuple>

namespace lila::scene::fracture
{
namespace
{
using geo::Index;
using geo::kInvalid;

// Scratch face attribute: faces the current cut made, as opposed to faces carried over from an operand.
constexpr std::string_view kCutAttr = "fractureCut";
// Largest noise amplitude as a fraction of noiseScale; keeps the displacement a one-to-one map of space.
constexpr double kMaxAmplitude = 0.2;

uint64_t splitmix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/** Seeded generator whose stream is the same on every platform and standard library. */
class Rng
{
public:
    explicit Rng(uint64_t seed) : mState(splitmix(seed)) {}
    uint64_t next()
    {
        mState += 0x9e3779b97f4a7c15ull;
        uint64_t z = mState;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    /** [0, 1) from 53 random bits. */
    double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double a, double b) { return a + (b - a) * uniform(); }
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
    double gaussian()
    {
        const double u = 1.0 - uniform(), v = uniform();
        return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * std::numbers::pi * v);
    }

private:
    uint64_t mState;
};

double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

/** Perlin gradient noise, about -1..1, smooth (C2) and seeded. */
double gradientNoise(uint64_t seed, const glm::dvec3& p)
{
    static constexpr double kGrad[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
                                            {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
    const glm::dvec3 f = glm::floor(p);
    const int64_t ix = int64_t(f.x), iy = int64_t(f.y), iz = int64_t(f.z);
    const glm::dvec3 t = p - f;
    double corner[8];
    for (int k = 0; k < 8; ++k)
    {
        const int dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
        const uint64_t h = splitmix(seed ^ splitmix(uint64_t(ix + dx) ^ splitmix(uint64_t(iy + dy) ^ splitmix(uint64_t(iz + dz)))));
        const double* g = kGrad[h % 12];
        corner[k] = g[0] * (t.x - dx) + g[1] * (t.y - dy) + g[2] * (t.z - dz);
    }
    const double u = fade(t.x), v = fade(t.y), w = fade(t.z);
    auto lerp = [](double a, double b, double s) { return a + s * (b - a); };
    const double x00 = lerp(corner[0], corner[1], u), x10 = lerp(corner[2], corner[3], u);
    const double x01 = lerp(corner[4], corner[5], u), x11 = lerp(corner[6], corner[7], u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

/**
 * Smooth vector noise of position that fades to nothing at the box's walls. Small enough against its feature size to
 * be one-to-one, so cells displaced by it still tile space, and both sides of a shared face move the same way.
 */
struct Displacement
{
    double amplitude = 0.0;
    double invScale = 0.0;
    uint64_t seed = 0;
    geo::Bounds box;
    double band = 1.0;

    bool active() const { return amplitude > 0.0; }
    glm::dvec3 operator()(const glm::dvec3& p) const
    {
        const glm::dvec3 lo = p - box.min, hi = box.max - p;
        const double d = std::min({lo.x, lo.y, lo.z, hi.x, hi.y, hi.z});
        const double s = std::clamp(d / band, 0.0, 1.0);
        const double w = s * s * (3.0 - 2.0 * s);
        if (w <= 0.0)
            return glm::dvec3(0.0);
        const glm::dvec3 q = p * invScale;
        return amplitude * w *
               glm::dvec3(gradientNoise(seed, q), gradientNoise(seed + 1, q + glm::dvec3(31.7, 11.3, 47.9)),
                          gradientNoise(seed + 2, q + glm::dvec3(73.1, 59.3, 19.7)));
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Convex cells

struct PolyFace
{
    std::vector<int> v;
    /** The neighbouring point whose bisector made the face, or -1 - side for the bounding box. */
    int id = -1;
};

/** Convex polyhedron, faces wound counter-clockwise seen from outside. */
struct Poly
{
    std::vector<glm::dvec3> v;
    std::vector<PolyFace> faces;
    bool empty() const { return faces.empty(); }
};

Poly makeBox(const geo::Bounds& b)
{
    Poly p;
    for (int i = 0; i < 8; ++i)
        p.v.push_back({i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z});
    p.faces = {{{0, 4, 6, 2}, -1}, {{1, 3, 7, 5}, -2}, {{0, 1, 5, 4}, -3},
               {{2, 6, 7, 3}, -4}, {{0, 2, 3, 1}, -5}, {{4, 5, 7, 6}, -6}};
    return p;
}

void compactPoly(Poly& poly)
{
    std::vector<int> remap(poly.v.size(), -1);
    std::vector<glm::dvec3> v;
    for (PolyFace& f : poly.faces)
        for (int& i : f.v)
        {
            if (remap[i] < 0)
            {
                remap[i] = int(v.size());
                v.push_back(poly.v[i]);
            }
            i = remap[i];
        }
    poly.v = std::move(v);
}

/** Keeps the part where dot(n, x) <= d (n unit length); the cap gets id. */
void clipPoly(Poly& poly, const glm::dvec3& n, double d, int id, double eps)
{
    std::vector<double> dist(poly.v.size());
    bool anyOut = false, anyIn = false;
    for (size_t i = 0; i < poly.v.size(); ++i)
    {
        dist[i] = glm::dot(n, poly.v[i]) - d;
        anyOut |= dist[i] > eps;
        anyIn |= dist[i] < -eps;
    }
    if (!anyOut)
        return;
    if (!anyIn)
    {
        poly = {};
        return;
    }

    std::vector<std::pair<uint64_t, int>> cutOf;
    std::vector<int> cap;
    auto cutVertex = [&](int a, int b) {
        if (a > b)
            std::swap(a, b);
        const uint64_t key = (uint64_t(a) << 32) | uint32_t(b);
        for (const auto& [k, i] : cutOf)
            if (k == key)
                return i;
        const double t = dist[a] / (dist[a] - dist[b]);
        const int i = int(poly.v.size());
        poly.v.push_back(poly.v[a] + t * (poly.v[b] - poly.v[a]));
        cutOf.push_back({key, i});
        cap.push_back(i);
        return i;
    };
    std::vector<PolyFace> faces;
    for (const PolyFace& f : poly.faces)
    {
        PolyFace out{{}, f.id};
        const size_t count = f.v.size();
        for (size_t k = 0; k < count; ++k)
        {
            const int a = f.v[k], b = f.v[(k + 1) % count];
            if (dist[a] <= eps)
            {
                out.v.push_back(a);
                if (dist[a] >= -eps)
                    cap.push_back(a);
            }
            if ((dist[a] < -eps && dist[b] > eps) || (dist[a] > eps && dist[b] < -eps))
                out.v.push_back(cutVertex(a, b));
        }
        if (out.v.size() >= 3)
            faces.push_back(std::move(out));
    }
    std::sort(cap.begin(), cap.end());
    cap.erase(std::unique(cap.begin(), cap.end()), cap.end());
    if (cap.size() >= 3)
    {
        glm::dvec3 c(0.0);
        for (int i : cap)
            c += poly.v[i];
        c /= double(cap.size());
        const glm::dvec3 u = glm::normalize(glm::cross(n, std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0)));
        const glm::dvec3 w = glm::cross(n, u);
        std::vector<std::pair<double, int>> order;
        for (int i : cap)
            order.push_back({std::atan2(glm::dot(poly.v[i] - c, w), glm::dot(poly.v[i] - c, u)), i});
        std::sort(order.begin(), order.end());
        PolyFace capFace{{}, id};
        for (const auto& [angle, i] : order)
            capFace.v.push_back(i);
        faces.push_back(std::move(capFace));
    }
    poly.faces = std::move(faces);
    if (poly.faces.size() < 4)
        poly = {};
    else
        compactPoly(poly);
}

/** Uniform grid over the points for nearest-first neighbour search. */
struct PointGrid
{
    glm::dvec3 origin{0.0};
    double cell = 1.0;
    glm::ivec3 dims{1};
    std::vector<uint32_t> start; // per grid cell, into items; one extra at the end
    std::vector<uint32_t> items;

    glm::ivec3 cellOf(const glm::dvec3& p) const
    {
        const glm::dvec3 g = (p - origin) / cell;
        return glm::clamp(glm::ivec3(glm::floor(g)), glm::ivec3(0), dims - 1);
    }
    size_t flat(const glm::ivec3& c) const { return (size_t(c.z) * dims.y + c.y) * dims.x + c.x; }

    void build(std::span<const glm::dvec3> pts)
    {
        geo::Bounds b;
        for (const glm::dvec3& p : pts)
            b.extend(p);
        origin = b.min;
        const glm::dvec3 ext = b.size();
        const double maxExt = std::max({ext.x, ext.y, ext.z, 1e-9});
        const double n = double(std::max<size_t>(pts.size(), 1));
        cell = maxExt / std::max(1.0, std::ceil(std::cbrt(n)));
        auto dimsFor = [&](double c) { return glm::clamp(glm::ivec3(glm::floor(ext / c)) + 1, glm::ivec3(1), glm::ivec3(256)); };
        dims = dimsFor(cell);
        while (double(dims.x) * dims.y * dims.z < n * 0.5 && cell > maxExt * 1e-4)
        {
            cell *= 0.7;
            dims = dimsFor(cell);
        }
        const size_t cells = size_t(dims.x) * dims.y * dims.z;
        start.assign(cells + 1, 0);
        for (const glm::dvec3& p : pts)
            ++start[flat(cellOf(p)) + 1];
        for (size_t i = 0; i < cells; ++i)
            start[i + 1] += start[i];
        items.resize(pts.size());
        std::vector<uint32_t> fill(start.begin(), start.end() - 1);
        for (size_t i = 0; i < pts.size(); ++i)
            items[fill[flat(cellOf(pts[i]))]++] = uint32_t(i);
    }
};

/**
 * The Voronoi cell of point i (in the stretched space the points are given in), clipped to the box. Neighbours are
 * taken nearest first and the search stops once the next one is beyond twice the cell's radius, which no bisector
 * that far away can cut.
 */
Poly voronoiCell(size_t i, std::span<const glm::dvec3> pts, const PointGrid& grid, const geo::Bounds& box, double eps)
{
    Poly poly = makeBox(box);
    const glm::dvec3 p = pts[i];
    const glm::ivec3 home = grid.cellOf(p);
    const int maxRing = std::max({grid.dims.x, grid.dims.y, grid.dims.z});
    std::vector<std::pair<double, uint32_t>> pool;
    auto radius = [&]() {
        double r = 0.0;
        for (const glm::dvec3& v : poly.v)
            r = std::max(r, glm::length(v - p));
        return r;
    };
    double r = radius();
    bool done = false;
    for (int ring = 0; ring <= maxRing && !done; ++ring)
    {
        const glm::ivec3 lo = glm::max(home - ring, glm::ivec3(0)), hi = glm::min(home + ring, grid.dims - 1);
        for (int z = lo.z; z <= hi.z; ++z)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x)
                {
                    if (std::max({std::abs(x - home.x), std::abs(y - home.y), std::abs(z - home.z)}) != ring)
                        continue;
                    const size_t c = grid.flat({x, y, z});
                    for (uint32_t k = grid.start[c]; k < grid.start[c + 1]; ++k)
                        if (const uint32_t j = grid.items[k]; j != i)
                            pool.push_back({glm::length(pts[j] - p), j});
                }
        const double reach = ring == maxRing ? std::numeric_limits<double>::max() : ring * grid.cell;
        std::sort(pool.begin(), pool.end());
        size_t used = 0;
        for (; used < pool.size() && pool[used].first <= reach; ++used)
        {
            const auto [dist, j] = pool[used];
            if (dist > 2.0 * r)
            {
                done = true;
                break;
            }
            if (dist <= eps)
            {
                if (j < i)
                    return {};
                continue;
            }
            const glm::dvec3 n = (pts[j] - p) / dist;
            clipPoly(poly, n, glm::dot(n, 0.5 * (p + pts[j])), int(j), eps);
            if (poly.empty())
                return {};
            r = radius();
        }
        pool.erase(pool.begin(), pool.begin() + used);
        done |= 2.0 * r <= reach;
    }
    return poly;
}

// ---------------------------------------------------------------------------------------------------------------------
// Cell meshes

/** Triangles of a cell; each triangle belongs to a group (a face id for Manifold) with the neighbour it faces. */
struct CellMesh
{
    std::vector<glm::dvec3> v;
    std::vector<uint32_t> tris;
    std::vector<uint32_t> triGroup;
    std::vector<int32_t> groupNeighbor;
};

CellMesh flatCell(const Poly& poly)
{
    CellMesh m;
    m.v = poly.v;
    for (size_t f = 0; f < poly.faces.size(); ++f)
    {
        const std::vector<int>& v = poly.faces[f].v;
        for (size_t k = 1; k + 1 < v.size(); ++k)
        {
            m.tris.insert(m.tris.end(), {uint32_t(v[0]), uint32_t(v[k]), uint32_t(v[k + 1])});
            m.triGroup.push_back(uint32_t(f));
        }
        m.groupNeighbor.push_back(poly.faces[f].id >= 0 ? poly.faces[f].id : -1);
    }
    return m;
}

uint64_t edgeKey(uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; }

bool lexLess(const glm::dvec3& a, const glm::dvec3& b) { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); }

/**
 * The cell with its cutting faces subdivided to their detail size and every point displaced. Everything is decided from
 * positions alone (edge splits, the fan centre, which diagonal), so the cell across a face builds the same triangles.
 */
CellMesh roughCell(const Poly& poly, int self, std::span<const double> cellDetail, const Displacement& disp)
{
    LILA_ZONE();

    CellMesh m;
    m.v = poly.v;
    auto detailOf = [&](int id) { return id >= 0 ? cellDetail[size_t(id)] : 0.0; };

    std::map<uint64_t, std::pair<int, int>> edgeFaces;
    for (size_t f = 0; f < poly.faces.size(); ++f)
    {
        const std::vector<int>& v = poly.faces[f].v;
        for (size_t k = 0; k < v.size(); ++k)
        {
            auto [it, fresh] = edgeFaces.try_emplace(edgeKey(v[k], v[(k + 1) % v.size()]), int(f), -1);
            if (!fresh)
                it->second.second = int(f);
        }
    }

    // Each polygon edge splits into 2^k equal parts by repeated halving, the same from either end.
    std::map<uint64_t, std::vector<uint32_t>> chains;
    for (const auto& [key, faces] : edgeFaces)
    {
        const uint32_t a = uint32_t(key >> 32), b = uint32_t(key & 0xffffffffu);
        double h = cellDetail[size_t(self)];
        for (int f : {faces.first, faces.second})
            if (f >= 0)
                h = std::max(h, detailOf(poly.faces[f].id));
        const double len = glm::length(m.v[b] - m.v[a]);
        int levels = 0;
        while (levels < 12 && len > h * double(1 << levels))
            ++levels;
        const size_t parts = size_t(1) << levels;
        std::vector<glm::dvec3> pos(parts + 1);
        pos[0] = m.v[a];
        pos[parts] = m.v[b];
        for (size_t step = parts / 2; step >= 1; step /= 2)
            for (size_t i = step; i < parts; i += 2 * step)
                pos[i] = (pos[i - step] + pos[i + step]) * 0.5;
        std::vector<uint32_t>& chain = chains[key];
        for (size_t i = 1; i < parts; ++i)
        {
            chain.push_back(uint32_t(m.v.size()));
            m.v.push_back(pos[i]);
        }
    }

    struct Tri
    {
        uint32_t v[3];
        bool fixed[3];
    };
    std::map<uint64_t, uint32_t> midOf;
    auto mid = [&](uint32_t a, uint32_t b) {
        auto [it, fresh] = midOf.try_emplace(edgeKey(a, b), uint32_t(m.v.size()));
        if (fresh)
            m.v.push_back((m.v[a] + m.v[b]) * 0.5);
        return it->second;
    };

    for (size_t f = 0; f < poly.faces.size(); ++f)
    {
        const PolyFace& face = poly.faces[f];
        std::vector<uint32_t> boundary;
        for (size_t k = 0; k < face.v.size(); ++k)
        {
            const uint32_t a = uint32_t(face.v[k]), b = uint32_t(face.v[(k + 1) % face.v.size()]);
            boundary.push_back(a);
            const std::vector<uint32_t>& chain = chains[edgeKey(a, b)];
            if (a < b)
                boundary.insert(boundary.end(), chain.begin(), chain.end());
            else
                boundary.insert(boundary.end(), chain.rbegin(), chain.rend());
        }
        std::vector<glm::dvec3> corners;
        for (int i : face.v)
            corners.push_back(poly.v[size_t(i)]);
        std::sort(corners.begin(), corners.end(), lexLess);
        glm::dvec3 centre(0.0);
        for (const glm::dvec3& c : corners)
            centre += c;
        const uint32_t c = uint32_t(m.v.size());
        m.v.push_back(centre / double(corners.size()));

        std::vector<Tri> tris;
        for (size_t k = 0; k < boundary.size(); ++k)
            tris.push_back({{c, boundary[k], boundary[(k + 1) % boundary.size()]}, {false, true, false}});

        if (face.id >= 0)
        {
            const double h = std::max(cellDetail[size_t(self)], detailOf(face.id));
            for (int round = 0; round < 24; ++round)
            {
                std::vector<Tri> next;
                bool any = false;
                for (const Tri& t : tris)
                {
                    bool mark[3];
                    int marked = 0;
                    for (int k = 0; k < 3; ++k)
                    {
                        mark[k] = !t.fixed[k] && glm::length(m.v[t.v[(k + 1) % 3]] - m.v[t.v[k]]) > h;
                        marked += mark[k];
                    }
                    if (marked == 0)
                    {
                        next.push_back(t);
                        continue;
                    }
                    any = true;
                    if (marked == 3)
                    {
                        const uint32_t a = t.v[0], b = t.v[1], d = t.v[2];
                        const uint32_t m0 = mid(a, b), m1 = mid(b, d), m2 = mid(d, a);
                        next.push_back({{a, m0, m2}, {false, false, false}});
                        next.push_back({{m0, b, m1}, {false, false, false}});
                        next.push_back({{m2, m1, d}, {false, false, false}});
                        next.push_back({{m0, m1, m2}, {false, false, false}});
                    }
                    else if (marked == 1)
                    {
                        const int k = mark[0] ? 0 : mark[1] ? 1 : 2;
                        const uint32_t a = t.v[k], b = t.v[(k + 1) % 3], d = t.v[(k + 2) % 3];
                        const uint32_t mm = mid(a, b);
                        next.push_back({{a, mm, d}, {false, false, t.fixed[(k + 2) % 3]}});
                        next.push_back({{mm, b, d}, {false, t.fixed[(k + 1) % 3], false}});
                    }
                    else
                    {
                        const int u = !mark[0] ? 0 : !mark[1] ? 1 : 2;
                        const uint32_t a = t.v[u], b = t.v[(u + 1) % 3], d = t.v[(u + 2) % 3];
                        const uint32_t m1 = mid(b, d), m2 = mid(d, a);
                        next.push_back({{m1, d, m2}, {false, false, false}});
                        const double la = glm::length(m.v[m1] - m.v[a]), lb = glm::length(m.v[m2] - m.v[b]);
                        const bool fromA = la < lb || (la == lb && lexLess(m.v[a], m.v[b]));
                        if (fromA)
                        {
                            next.push_back({{a, b, m1}, {t.fixed[u], false, false}});
                            next.push_back({{a, m1, m2}, {false, false, false}});
                        }
                        else
                        {
                            next.push_back({{a, b, m2}, {t.fixed[u], false, false}});
                            next.push_back({{b, m1, m2}, {false, false, false}});
                        }
                    }
                }
                tris = std::move(next);
                if (!any || tris.size() > 2000000)
                    break;
            }
        }
        const bool cutting = face.id >= 0;
        for (const Tri& t : tris)
        {
            m.tris.insert(m.tris.end(), {t.v[0], t.v[1], t.v[2]});
            if (cutting)
            {
                m.triGroup.push_back(uint32_t(m.groupNeighbor.size()));
                m.groupNeighbor.push_back(face.id);
            }
            else
                m.triGroup.push_back(kInvalid);
        }
        if (!cutting)
        {
            // The box faces stay flat, one group each.
            const uint32_t group = uint32_t(m.groupNeighbor.size());
            m.groupNeighbor.push_back(-1);
            for (size_t t = m.triGroup.size() - tris.size(); t < m.triGroup.size(); ++t)
                m.triGroup[t] = group;
        }
    }
    for (glm::dvec3& p : m.v)
        p += disp(p);
    return m;
}

// ---------------------------------------------------------------------------------------------------------------------
// Manifold round trip

/** A float attribute that rides through Manifold as vertex properties after x, y, z and the point id. */
struct Channel
{
    geo::Domain domain;
    std::string name;
    geo::AttrType type;
    int components = 0;
    int offset = 0;
};

struct Layout
{
    std::vector<Channel> channels;
    int numProp = 4;
};

Layout layoutOf(const geo::Mesh& mesh)
{
    Layout l;
    for (geo::Domain d : {geo::Domain::Corner, geo::Domain::Point})
    {
        const geo::AttributeSet& set = mesh.attributes(d);
        for (size_t i = 0; i < set.layerCount(); ++i)
        {
            const geo::Attribute& a = set.layer(i);
            if (!geo::attrTypeIsFloat(a.type()))
                continue;
            const int n = geo::attrTypeComponents(a.type());
            l.channels.push_back({d, a.name(), a.type(), n, l.numProp});
            l.numProp += n;
        }
    }
    return l;
}

/** One Manifold vertex per corner so attributes survive, welded by position. faceID is the face plus faceOffset. */
manifold::MeshGL64 toManifold(const geo::Mesh& mesh, const Layout& layout, uint64_t faceOffset)
{
    LILA_ZONE();

    const geo::Triangulation tris = geo::triangulate(mesh);
    std::vector<const geo::Attribute*> attrs;
    for (const Channel& ch : layout.channels)
    {
        const geo::Attribute* a = mesh.attributes(ch.domain).find(ch.name);
        attrs.push_back(a && a->type() == ch.type ? a : nullptr);
    }
    manifold::MeshGL64 gl;
    gl.numProp = uint64_t(layout.numProp);
    std::vector<uint64_t> vertOfCorner(mesh.cornerCapacity(), ~uint64_t(0));
    for (size_t t = 0; t < tris.faces.size(); ++t)
    {
        for (int k = 0; k < 3; ++k)
        {
            const Index c = tris.corners[t * 3 + k];
            if (vertOfCorner[c] == ~uint64_t(0))
            {
                vertOfCorner[c] = gl.vertProperties.size() / gl.numProp;
                const Index point = mesh.cornerPoint(c);
                const glm::dvec3 p = mesh.position(point);
                gl.vertProperties.insert(gl.vertProperties.end(), {p.x, p.y, p.z, double(point)});
                for (size_t i = 0; i < layout.channels.size(); ++i)
                {
                    const Channel& ch = layout.channels[i];
                    const size_t element = ch.domain == geo::Domain::Corner ? c : point;
                    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    if (attrs[i] && element < attrs[i]->size())
                        std::memcpy(v, attrs[i]->raw(element), sizeof(float) * ch.components);
                    for (int j = 0; j < ch.components; ++j)
                        gl.vertProperties.push_back(v[j]);
                }
            }
            gl.triVerts.push_back(vertOfCorner[c]);
        }
        gl.faceID.push_back(faceOffset + tris.faces[t]);
    }
    gl.Merge();
    return gl;
}

manifold::MeshGL64 cellToManifold(const CellMesh& cell, const Layout& layout, uint64_t faceOffset)
{
    manifold::MeshGL64 gl;
    gl.numProp = uint64_t(layout.numProp);
    gl.vertProperties.assign(cell.v.size() * gl.numProp, 0.0);
    for (size_t i = 0; i < cell.v.size(); ++i)
    {
        double* p = &gl.vertProperties[i * gl.numProp];
        p[0] = cell.v[i].x;
        p[1] = cell.v[i].y;
        p[2] = cell.v[i].z;
        p[3] = -1.0;
    }
    gl.triVerts.assign(cell.tris.begin(), cell.tris.end());
    for (uint32_t g : cell.triGroup)
        gl.faceID.push_back(faceOffset + g);
    return gl;
}

struct Operand
{
    const geo::Mesh* mesh = nullptr;
    uint64_t firstFace = 0;
};

/** Faces with ids from firstFace on are the cut's: group id - firstFace indexes neighbor. */
struct CutSurface
{
    uint64_t firstFace = 0;
    std::span<const int32_t> neighbor;
    int32_t slot = 0;
};

/**
 * Rebuilds a polygon mesh from Manifold's output: triangles of one input face merge back, operand faces, corners,
 * points and edges keep their attributes, and cut faces are tagged kInsideAttr, kNeighborAttr, the slot and kCutAttr.
 */
geo::Mesh fromManifold(const manifold::MeshGL64& gl, std::span<const Operand> operands, const Layout& layout,
                       const CutSurface* cut, const std::vector<std::string>& materialNames)
{
    LILA_ZONE();

    const geo::Mesh& base = *operands[0].mesh;
    auto operandOf = [&](uint64_t id) {
        size_t o = 0;
        while (o + 1 < operands.size() && id >= operands[o + 1].firstFace)
            ++o;
        return o;
    };
    auto isCut = [&](uint64_t id) { return cut && id >= cut->firstFace; };

    geo::Mesh m;
    for (int d = 0; d < int(geo::Domain::Count); ++d)
        m.attributes(geo::Domain(d)).mergeLayout(base.attributes(geo::Domain(d)));
    m.faceAttrs().add(kInsideAttr, geo::AttrType::Int);
    m.faceAttrs().add(kNeighborAttr, geo::AttrType::Int).setDefault<int32_t>(-1);
    m.faceAttrs().add(kCutAttr, geo::AttrType::Int);
    m.faceAttrs().add(geo::attr::kMaterial, geo::AttrType::Int);
    m.materialNames() = materialNames;
    m.colorSetInfo() = base.colorSetInfo();

    const size_t verts = gl.NumVert(), numProp = gl.numProp;
    std::vector<uint64_t> canon(verts);
    for (size_t v = 0; v < verts; ++v)
        canon[v] = v;
    for (size_t i = 0; i < gl.mergeFromVert.size(); ++i)
        canon[gl.mergeFromVert[i]] = gl.mergeToVert[i];
    std::vector<Index> pointOf(verts, kInvalid);
    for (size_t v = 0; v < verts; ++v)
    {
        const uint64_t c = canon[v];
        if (pointOf[c] == kInvalid)
        {
            const double* p = &gl.vertProperties[c * numProp];
            pointOf[c] = m.addPoint({p[0], p[1], p[2]});
        }
        pointOf[v] = pointOf[c];
    }
    const size_t numTri = gl.NumTri();
    std::vector<uint64_t> faceId;
    std::vector<Index> triOfFace;
    for (size_t t = 0; t < numTri; ++t)
    {
        const Index f = m.addFace({pointOf[gl.triVerts[t * 3]], pointOf[gl.triVerts[t * 3 + 1]], pointOf[gl.triVerts[t * 3 + 2]]});
        if (f == kInvalid)
            continue;
        faceId.resize(f + 1, ~uint64_t(0));
        triOfFace.resize(f + 1, kInvalid);
        faceId[f] = gl.faceID[t];
        triOfFace[f] = Index(t);
    }

    // Which operand point each point is: its id survived exactly and it sits where that point is.
    std::vector<int64_t> origin(m.pointCapacity(), -1);
    std::vector<Index> originOperand(m.pointCapacity(), kInvalid);
    std::vector<double> tol(operands.size());
    for (size_t o = 0; o < operands.size(); ++o)
        tol[o] = glm::length(operands[o].mesh->bounds().size()) * 1e-9 + 1e-12;
    std::vector<uint8_t> pointSet(m.pointCapacity(), 0);
    std::vector<const geo::Attribute*> attrs(layout.channels.size());
    std::vector<geo::Attribute*> outAttrs(layout.channels.size());
    for (size_t i = 0; i < layout.channels.size(); ++i)
        outAttrs[i] = m.attributes(layout.channels[i].domain).find(layout.channels[i].name);

    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const size_t t = triOfFace[f];
        const uint64_t id = faceId[f];
        const bool cutFace = isCut(id);
        const size_t o = cutFace ? 0 : operandOf(id);
        const geo::Mesh& src = *operands[o].mesh;
        if (cutFace)
        {
            m.faceAttrs().get<int32_t>(kInsideAttr)[f] = 1;
            m.faceAttrs().get<int32_t>(kCutAttr)[f] = 1;
            m.faceAttrs().get<int32_t>(kNeighborAttr)[f] = cut->neighbor[id - cut->firstFace];
            m.faceAttrs().get<int32_t>(geo::attr::kMaterial)[f] = cut->slot;
        }
        else
            m.faceAttrs().copyFrom(src.faceAttrs(), f, Index(id - operands[o].firstFace));

        int k = 0;
        m.forEachFaceCorner(f, [&](Index c) {
            const uint64_t v = gl.triVerts[t * 3 + k++];
            const double* p = &gl.vertProperties[v * numProp];
            const Index point = m.cornerPoint(c);
            if (!cutFace && origin[point] < 0)
            {
                const double pid = p[3];
                if (pid >= 0.0 && pid == std::floor(pid) && Index(pid) < src.pointCapacity() && src.pointValid(Index(pid)) &&
                    glm::length(src.position(Index(pid)) - m.position(point)) <= tol[o])
                {
                    origin[point] = int64_t(pid);
                    originOperand[point] = Index(o);
                }
            }
            if (cutFace)
                return;
            for (size_t i = 0; i < layout.channels.size(); ++i)
            {
                const Channel& ch = layout.channels[i];
                if (!outAttrs[i])
                    continue;
                const size_t element = ch.domain == geo::Domain::Corner ? c : point;
                if (ch.domain == geo::Domain::Point && pointSet[point])
                    continue;
                float vals[4];
                for (int j = 0; j < ch.components; ++j)
                    vals[j] = float(p[ch.offset + j]);
                std::memcpy(outAttrs[i]->raw(element), vals, sizeof(float) * ch.components);
            }
            pointSet[point] = 1;
        });
    }
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (origin[p] >= 0)
            m.pointAttrs().copyFrom(operands[originOperand[p]].mesh->pointAttrs(), p, Index(origin[p]));

    // Edges between two points of one operand that were an edge there keep its attributes.
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        const Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
        if (origin[a] < 0 || origin[b] < 0 || originOperand[a] != originOperand[b])
            continue;
        const geo::Mesh& src = *operands[originOperand[a]].mesh;
        const Index se = src.findEdge(Index(origin[a]), Index(origin[b]));
        if (se != kInvalid)
            m.edgeAttrs().copyFrom(src.edgeAttrs(), e, se);
    }

    std::vector<Index> inner;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        const Index c = m.edgeCorner(e);
        const Index f0 = m.cornerFace(c), f1 = m.cornerFace(m.cornerRadial(c));
        if (faceId[f0] == faceId[f1])
            inner.push_back(e);
    }
    geo::dissolveEdges(m, inner, false);
    m.compact();
    return m;
}

void volumeOf(const manifold::MeshGL64& gl, double& volume, glm::dvec3& centroid)
{
    volume = 0.0;
    centroid = glm::dvec3(0.0);
    const size_t n = gl.NumVert();
    if (!n)
        return;
    glm::dvec3 ref(0.0);
    for (size_t v = 0; v < n; ++v)
        ref += glm::dvec3(gl.vertProperties[v * gl.numProp], gl.vertProperties[v * gl.numProp + 1], gl.vertProperties[v * gl.numProp + 2]);
    ref /= double(n);
    auto pos = [&](uint64_t v) {
        const double* p = &gl.vertProperties[v * gl.numProp];
        return glm::dvec3(p[0], p[1], p[2]) - ref;
    };
    glm::dvec3 moment(0.0);
    for (size_t t = 0; t < gl.NumTri(); ++t)
    {
        const glm::dvec3 a = pos(gl.triVerts[t * 3]), b = pos(gl.triVerts[t * 3 + 1]), c = pos(gl.triVerts[t * 3 + 2]);
        const double v6 = glm::dot(a, glm::cross(b, c));
        volume += v6;
        moment += v6 * (a + b + c);
    }
    volume /= 6.0;
    centroid = volume != 0.0 ? ref + moment / (24.0 * volume) : ref;
}

/** Box-projected uvs in cm / scale on the cut faces, hard edges and uv seams where they meet the rest, normals on them. */
void finishCutFaces(geo::Mesh& m, double uvScale)
{
    LILA_ZONE();

    auto cut = m.faceAttrs().get<int32_t>(kCutAttr);
    std::vector<int> axis(m.faceCapacity(), -1);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    cut = m.faceAttrs().get<int32_t>(kCutAttr);
    const double inv = 1.0 / std::max(uvScale, 1e-9);
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || !cut[f])
            continue;
        const glm::dvec3 n = m.faceNormal(f), a = glm::abs(n);
        const int ax = a.x >= a.y && a.x >= a.z ? 0 : a.y >= a.z ? 1 : 2;
        axis[f] = ax;
        m.forEachFaceCorner(f, [&](Index c) {
            const glm::dvec3 p = m.position(m.cornerPoint(c)) * inv;
            glm::dvec2 t = ax == 0 ? glm::dvec2(n.x > 0.0 ? -p.z : p.z, p.y)
                         : ax == 1 ? glm::dvec2(p.x, n.y > 0.0 ? -p.z : p.z)
                                   : glm::dvec2(n.z > 0.0 ? p.x : -p.x, p.y);
            uv[c] = glm::vec2(t);
        });
    }

    auto hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    auto seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    std::vector<Index> cutCut;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        const Index c = m.edgeCorner(e);
        const Index f0 = m.cornerFace(c), f1 = m.cornerFace(m.cornerRadial(c));
        if (bool(cut[f0]) != bool(cut[f1]))
            hard[e] = seam[e] = true;
        else if (cut[f0])
        {
            seam[e] = axis[f0] != axis[f1];
            cutCut.push_back(e);
        }
    }
    geo::setHardEdgesByAngle(m, 30.0, cutCut);

    if (auto stored = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal); !stored.empty())
    {
        const std::vector<glm::vec3> normals = geo::cornerNormals(m);
        stored = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f) && cut[f])
                m.forEachFaceCorner(f, [&](Index c) { stored[c] = normals[c]; });
    }
    if (m.cornerAttrs().has(geo::attr::kTangent))
        geo::updateTangents(m);
}

/** Contacts from the inside faces' neighbour cells, averaged over both sides. */
std::vector<Contact> buildContacts(const std::vector<Piece>& pieces)
{
    LILA_ZONE();

    std::map<int, int> pieceOfCell;
    for (size_t i = 0; i < pieces.size(); ++i)
        pieceOfCell.emplace(pieces[i].cell, int(i));
    struct Side
    {
        double area[2] = {0.0, 0.0};
        glm::dvec3 moment[2] = {glm::dvec3(0.0), glm::dvec3(0.0)};
    };
    std::map<std::pair<int, int>, Side> sides;
    for (size_t i = 0; i < pieces.size(); ++i)
    {
        const geo::Mesh& m = pieces[i].mesh;
        auto inside = m.faceAttrs().get<int32_t>(kInsideAttr);
        auto neighbor = m.faceAttrs().get<int32_t>(kNeighborAttr);
        if (inside.empty() || neighbor.empty())
            continue;
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f) || !inside[f] || neighbor[f] < 0)
                continue;
            auto it = pieceOfCell.find(neighbor[f]);
            if (it == pieceOfCell.end() || it->second == int(i))
                continue;
            const int j = it->second;
            Side& s = sides[{std::min(int(i), j), std::max(int(i), j)}];
            const int side = int(i) < j ? 0 : 1;
            const double area = m.faceArea(f);
            s.area[side] += area;
            s.moment[side] += area * m.faceCentroid(f);
        }
    }
    std::vector<Contact> out;
    for (const auto& [key, s] : sides)
    {
        const double total = s.area[0] + s.area[1];
        if (total <= 0.0)
            continue;
        const int sidesSeen = (s.area[0] > 0.0) + (s.area[1] > 0.0);
        out.push_back({key.first, key.second, total / sidesSeen, (s.moment[0] + s.moment[1]) / total});
    }
    return out;
}

std::vector<std::string> namesWithDefault(const geo::Mesh& mesh)
{
    std::vector<std::string> names = mesh.materialNames();
    if (names.empty())
        names.push_back(materials::kDefaultSlot);
    return names;
}

/** Closest distance from p to the triangle abc. */
double pointTriangleDistance(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return glm::length(ap);
    const glm::dvec3 bp = p - b;
    const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
        return glm::length(bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
        return glm::length(p - (a + ab * (d1 / (d1 - d3))));
    const glm::dvec3 cp = p - c;
    const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
        return glm::length(cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
        return glm::length(p - (a + ac * (d2 / (d2 - d6))));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
        return glm::length(p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
    const double denom = 1.0 / (va + vb + vc);
    return glm::length(p - (a + ab * (vb * denom) + ac * (vc * denom)));
}

/** True when p is inside the closed mesh: odd crossings along a fixed skewed ray. */
bool insideMesh(const geo::MeshBvh& bvh, const glm::dvec3& p, double step)
{
    static const glm::dvec3 kDir = glm::normalize(glm::dvec3(0.5773, 0.6123, 0.5401));
    glm::dvec3 origin = p;
    int crossings = 0;
    for (int i = 0; i < 256; ++i)
    {
        geo::BvhHit hit;
        if (!bvh.raycast(origin, kDir, hit))
            break;
        ++crossings;
        origin = hit.point + kDir * step;
    }
    return crossings & 1;
}
} // namespace

std::vector<glm::dvec3> scatterPoints(const geo::Mesh& mesh, const ScatterOptions& o)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out;
    if (o.count <= 0 || mesh.numFaces() == 0)
        return out;
    Rng rng(o.seed);
    const geo::Triangulation tris = geo::triangulate(mesh);
    const std::span<const float> density = o.densityAttr.empty() ? std::span<const float>{} : mesh.pointAttrs().get<float>(o.densityAttr);
    auto triPoints = [&](size_t t, glm::dvec3 (&p)[3]) {
        for (int k = 0; k < 3; ++k)
            p[k] = mesh.position(mesh.cornerPoint(tris.corners[t * 3 + k]));
    };
    auto triDensity = [&](size_t t) {
        if (density.empty())
            return 1.0;
        double d = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            const Index p = mesh.cornerPoint(tris.corners[t * 3 + k]);
            d += p < density.size() ? std::max(0.0f, density[p]) : 0.0;
        }
        return d / 3.0;
    };
    const bool useImpacts = !o.impacts.empty() && o.impactBias > 0.0;
    const double sigma = std::max(o.impactRadius, 1e-6) * 0.5;

    if (o.surface)
    {
        std::vector<double> cumulative, cumulativeNear;
        double total = 0.0, totalNear = 0.0;
        for (size_t t = 0; t < tris.faces.size(); ++t)
        {
            glm::dvec3 p[3];
            triPoints(t, p);
            const double w = 0.5 * glm::length(glm::cross(p[1] - p[0], p[2] - p[0])) * triDensity(t);
            total += w;
            cumulative.push_back(total);
            if (useImpacts)
            {
                const glm::dvec3 c = (p[0] + p[1] + p[2]) / 3.0;
                double d2 = std::numeric_limits<double>::max();
                for (const glm::dvec3& imp : o.impacts)
                    d2 = std::min(d2, glm::dot(c - imp, c - imp));
                totalNear += w * std::exp(-d2 / (2.0 * sigma * sigma));
                cumulativeNear.push_back(totalNear);
            }
        }
        if (total <= 0.0)
            return out;
        for (int i = 0; i < o.count; ++i)
        {
            const bool near = useImpacts && totalNear > 0.0 && rng.uniform() < o.impactBias;
            const std::vector<double>& cum = near ? cumulativeNear : cumulative;
            const double pick = rng.uniform() * (near ? totalNear : total);
            const size_t t = std::min(size_t(std::upper_bound(cum.begin(), cum.end(), pick) - cum.begin()), cum.size() - 1);
            glm::dvec3 p[3];
            triPoints(t, p);
            double u = rng.uniform(), v = rng.uniform();
            if (u + v > 1.0)
            {
                u = 1.0 - u;
                v = 1.0 - v;
            }
            out.push_back(p[0] + u * (p[1] - p[0]) + v * (p[2] - p[0]));
        }
        return out;
    }

    const geo::MeshBvh bvh(mesh);
    const geo::Bounds b = mesh.bounds();
    const double step = glm::length(b.size()) * 1e-9 + 1e-12;
    double maxDensity = 0.0;
    for (float d : density)
        maxDensity = std::max(maxDensity, double(d));
    auto densityAt = [&](const glm::dvec3& p) {
        if (density.empty())
            return 1.0;
        geo::BvhClosest hit;
        if (!bvh.closestPoint(p, hit))
            return 0.0;
        double d = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            const Index q = mesh.cornerPoint(bvh.triangles().corners[size_t(hit.triangle) * 3 + k]);
            d += hit.bary[k] * (q < density.size() ? std::max(0.0f, density[q]) : 0.0f);
        }
        return d;
    };
    auto accept = [&](const glm::dvec3& p) {
        if (!insideMesh(bvh, p, step))
            return false;
        return density.empty() || (maxDensity > 0.0 && rng.uniform() * maxDensity < densityAt(p));
    };
    const int64_t budget = int64_t(o.count) * 2000;
    int64_t attempts = 0;
    while (int(out.size()) < o.count && attempts < budget)
    {
        bool placed = false;
        if (useImpacts && rng.uniform() < o.impactBias)
        {
            const glm::dvec3 imp = o.impacts[size_t(rng.below(o.impacts.size()))];
            for (int tries = 0; tries < 64 && !placed; ++tries, ++attempts)
            {
                const glm::dvec3 p = imp + sigma * glm::dvec3(rng.gaussian(), rng.gaussian(), rng.gaussian());
                if (accept(p))
                {
                    out.push_back(p);
                    placed = true;
                }
            }
        }
        while (!placed && attempts < budget)
        {
            ++attempts;
            const glm::dvec3 p(rng.uniform(b.min.x, b.max.x), rng.uniform(b.min.y, b.max.y), rng.uniform(b.min.z, b.max.z));
            if (accept(p))
            {
                out.push_back(p);
                placed = true;
            }
        }
    }
    return out;
}

std::vector<glm::dvec3> radialPoints(const glm::dvec3& impact, const glm::dvec3& normal, double radius, int rings, int spokes,
                                     double jitter, uint64_t seed)
{
    std::vector<glm::dvec3> out{impact};
    if (rings <= 0 || spokes <= 0 || radius <= 0.0)
        return out;
    Rng rng(seed);
    const double len = glm::length(normal);
    const glm::dvec3 n = len > 1e-12 ? normal / len : glm::dvec3(0, 1, 0);
    const glm::dvec3 u = glm::normalize(glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
    const glm::dvec3 w = glm::cross(n, u);
    jitter = std::clamp(jitter, 0.0, 1.0);
    double previous = 0.0;
    for (int k = 1; k <= rings; ++k)
    {
        const double r = radius * std::pow(double(k) / rings, 1.5);
        const double width = r - previous;
        previous = r;
        for (int s = 0; s < spokes; ++s)
        {
            const double a = 2.0 * std::numbers::pi * (s + 0.4 * jitter * rng.uniform(-1.0, 1.0)) / spokes;
            const double rr = r + 0.4 * jitter * width * rng.uniform(-1.0, 1.0);
            out.push_back(impact + rr * (std::cos(a) * u + std::sin(a) * w));
        }
    }
    return out;
}

FractureResult voronoiFracture(const geo::Mesh& source, std::span<const glm::dvec3> points, const FractureOptions& o)
{
    LILA_ZONE();

    FractureResult result;
    if (source.numFaces() == 0 || points.empty())
        return result;

    const Layout layout = layoutOf(source);
    const manifold::Manifold solid(toManifold(source, layout, 0));
    if (solid.Status() != manifold::Manifold::Error::NoError)
    {
        result.error = "source isn't a closed solid (" + std::to_string(int(solid.Status())) + ")";
        return result;
    }

    const glm::dvec3 stretch = glm::max(o.stretch, glm::dvec3(1e-3));
    const geo::Bounds bounds = source.bounds();
    const double diag = glm::length(bounds.size());

    Displacement disp;
    double detail = 0.0;
    if (o.noiseAmplitude > 0.0 && o.noiseScale > 0.0)
    {
        disp.amplitude = std::min(o.noiseAmplitude, kMaxAmplitude * o.noiseScale);
        disp.invScale = 1.0 / o.noiseScale;
        disp.seed = splitmix(o.seed ^ 0x6e6f697365ull);
        disp.band = 10.0 * disp.amplitude;
        detail = o.detailSize > 0.0 ? o.detailSize : o.noiseScale / 3.0;
    }
    const double pad = 0.05 * diag + 1e-3 + (disp.active() ? disp.band : 0.0);
    geo::Bounds box{bounds.min - pad, bounds.max + pad};
    disp.box = box;
    const geo::Bounds boxQ{box.min * stretch, box.max * stretch};

    std::vector<glm::dvec3> q(points.size());
    for (size_t i = 0; i < points.size(); ++i)
        q[i] = points[i] * stretch;
    PointGrid grid;
    grid.build(q);
    const double eps = glm::length(boxQ.size()) * 1e-10;

    std::vector<Poly> cells(points.size());
    tbb::parallel_for(size_t(0), points.size(), [&](size_t i) {
        const glm::dvec3& p = points[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return;
        Poly cell = voronoiCell(i, q, grid, boxQ, eps);
        geo::Bounds cb;
        for (glm::dvec3& v : cell.v)
        {
            v /= stretch;
            cb.extend(v);
        }
        if (cell.empty() || cb.min.x > bounds.max.x || cb.min.y > bounds.max.y || cb.min.z > bounds.max.z ||
            cb.max.x < bounds.min.x || cb.max.y < bounds.min.y || cb.max.z < bounds.min.z)
            return;
        cells[i] = std::move(cell);
    });

    // Detail per cell, coarsened for big cutting surfaces; a shared face uses the coarser of its two cells.
    std::vector<double> cellDetail(points.size(), detail);
    if (disp.active())
    {
        const double cap = double(std::max(o.maxInteriorFacesPerPiece, 16));
        for (size_t i = 0; i < cells.size(); ++i)
        {
            double area = 0.0;
            for (const PolyFace& f : cells[i].faces)
            {
                if (f.id < 0)
                    continue;
                glm::dvec3 an(0.0);
                for (size_t k = 0; k < f.v.size(); ++k)
                    an += glm::cross(cells[i].v[f.v[k]], cells[i].v[f.v[(k + 1) % f.v.size()]]);
                area += 0.5 * glm::length(an);
            }
            cellDetail[i] = std::max(detail, std::sqrt(area / (0.3 * cap)));
        }
    }

    std::vector<std::string> names = namesWithDefault(source);
    const std::string interior = o.interiorMaterial.empty() ? materials::slotName(source, 0) + "_Interior" : o.interiorMaterial;
    auto slotIt = std::find(names.begin(), names.end(), interior);
    if (slotIt == names.end())
        slotIt = names.insert(names.end(), interior);
    const int32_t slot = int32_t(slotIt - names.begin());
    const uint64_t cutBase = uint64_t(source.faceCapacity());

    std::vector<std::optional<Piece>> pieces(points.size());
    std::vector<std::string> errors(points.size());
    tbb::parallel_for(size_t(0), points.size(), [&](size_t i) {
        if (cells[i].empty())
            return;
        const CellMesh cell = disp.active() ? roughCell(cells[i], int(i), cellDetail, disp) : flatCell(cells[i]);
        const manifold::Manifold cutter(cellToManifold(cell, layout, cutBase));
        if (cutter.Status() != manifold::Manifold::Error::NoError)
        {
            errors[i] = "cell " + std::to_string(i) + " isn't a closed solid (" + std::to_string(int(cutter.Status())) + ")";
            return;
        }
        const manifold::Manifold part = solid ^ cutter;
        if (part.IsEmpty())
            return;
        const manifold::MeshGL64 gl = part.GetMeshGL64();
        Piece piece;
        volumeOf(gl, piece.volume, piece.centroid);
        const Operand op{&source, 0};
        const CutSurface surface{cutBase, cell.groupNeighbor, slot};
        piece.mesh = fromManifold(gl, std::span(&op, 1), layout, &surface, names);
        finishCutFaces(piece.mesh, o.interiorUvScale);
        piece.mesh.faceAttrs().remove(kCutAttr);
        piece.cell = int(i);
        pieces[i] = std::move(piece);
    });

    for (const std::string& e : errors)
        if (!e.empty() && result.error.empty())
            result.error = e;
    for (auto& p : pieces)
        if (p)
            result.pieces.push_back(std::move(*p));
    result.contacts = buildContacts(result.pieces);
    return result;
}

FractureResult clusterPieces(const FractureResult& in, double clusterSize, uint64_t seed)
{
    LILA_ZONE();

    FractureResult result;
    result.error = in.error;
    const size_t n = in.pieces.size();
    if (n == 0 || clusterSize <= 1.0)
    {
        result.pieces = in.pieces;
        result.contacts = in.contacts;
        return result;
    }
    std::vector<std::vector<int>> adjacent(n);
    for (const Contact& c : in.contacts)
    {
        adjacent[size_t(c.a)].push_back(c.b);
        adjacent[size_t(c.b)].push_back(c.a);
    }
    const size_t k = std::clamp<size_t>(size_t(std::llround(double(n) / clusterSize)), 1, n);
    std::vector<int> order(n);
    for (size_t i = 0; i < n; ++i)
        order[i] = int(i);
    Rng rng(seed);
    for (size_t i = 0; i < k; ++i)
        std::swap(order[i], order[i + size_t(rng.below(n - i))]);

    // Grow every cluster from its seed through the contacts, nearest centroid first.
    std::vector<int> clusterOf(n, -1);
    using Item = std::tuple<double, int, int>; // distance from the seed, piece, cluster
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    for (size_t c = 0; c < k; ++c)
        queue.push({0.0, order[c], int(c)});
    while (!queue.empty())
    {
        const auto [d, p, c] = queue.top();
        queue.pop();
        if (clusterOf[size_t(p)] >= 0)
            continue;
        clusterOf[size_t(p)] = c;
        const glm::dvec3 origin = in.pieces[size_t(order[size_t(c)])].centroid;
        for (int q : adjacent[size_t(p)])
            if (clusterOf[size_t(q)] < 0)
                queue.push({glm::length(in.pieces[size_t(q)].centroid - origin), q, c});
    }
    int clusters = int(k);
    for (size_t i = 0; i < n; ++i)
        if (clusterOf[i] < 0)
            clusterOf[i] = clusters++;

    const size_t numClusters = size_t(clusters);
    std::vector<std::vector<int>> members(numClusters);
    for (size_t i = 0; i < n; ++i)
        members[size_t(clusterOf[i])].push_back(int(i));
    std::vector<int> rep(numClusters, std::numeric_limits<int>::max());
    for (size_t i = 0; i < n; ++i)
        rep[size_t(clusterOf[i])] = std::min(rep[size_t(clusterOf[i])], in.pieces[i].cell);
    std::map<int, int> repOfCell;
    for (size_t i = 0; i < n; ++i)
        repOfCell[in.pieces[i].cell] = rep[size_t(clusterOf[i])];
    std::vector<int> clusterOrder(numClusters);
    for (int c = 0; c < clusters; ++c)
        clusterOrder[size_t(c)] = c;
    std::sort(clusterOrder.begin(), clusterOrder.end(), [&](int a, int b) { return rep[size_t(a)] < rep[size_t(b)]; });

    std::vector<Piece> out(numClusters);
    std::vector<std::string> errors(numClusters);
    tbb::parallel_for(size_t(0), numClusters, [&](size_t slotIndex) {
        const std::vector<int>& group = members[size_t(clusterOrder[slotIndex])];
        Piece piece;
        if (group.size() == 1)
            piece = in.pieces[size_t(group[0])];
        else
        {
            const geo::Mesh& first = in.pieces[size_t(group[0])].mesh;
            const Layout layout = layoutOf(first);
            // Points within a hair of each other become one, so faces shared by two members coincide exactly.
            geo::Bounds b;
            for (int g : group)
                b.extend(in.pieces[size_t(g)].mesh.bounds());
            const double snap = glm::length(b.size()) * 1e-9;
            std::vector<manifold::Manifold> solids;
            std::vector<Operand> ops;
            std::map<std::array<int64_t, 3>, glm::dvec3> snapped;
            uint64_t offset = 0;
            for (int g : group)
            {
                const geo::Mesh& m = in.pieces[size_t(g)].mesh;
                manifold::MeshGL64 gl = toManifold(m, layout, offset);
                for (size_t v = 0; v < gl.NumVert(); ++v)
                {
                    double* p = &gl.vertProperties[v * gl.numProp];
                    const std::array<int64_t, 3> key{std::llround(p[0] / snap), std::llround(p[1] / snap), std::llround(p[2] / snap)};
                    glm::dvec3 at(p[0], p[1], p[2]);
                    bool found = false;
                    for (int dz = -1; dz <= 1 && !found; ++dz)
                        for (int dy = -1; dy <= 1 && !found; ++dy)
                            for (int dx = -1; dx <= 1 && !found; ++dx)
                                if (auto it = snapped.find({key[0] + dx, key[1] + dy, key[2] + dz}); it != snapped.end() && glm::length(it->second - at) <= snap)
                                {
                                    at = it->second;
                                    found = true;
                                }
                    if (!found)
                        snapped.emplace(key, at);
                    p[0] = at.x;
                    p[1] = at.y;
                    p[2] = at.z;
                }
                solids.emplace_back(gl);
                ops.push_back({&m, offset});
                offset += m.faceCapacity();
            }
            const manifold::Manifold merged = manifold::Manifold::BatchBoolean(solids, manifold::OpType::Add);
            if (merged.Status() != manifold::Manifold::Error::NoError)
            {
                errors[slotIndex] = "cluster merge failed (" + std::to_string(int(merged.Status())) + ")";
                return;
            }
            const manifold::MeshGL64 gl = merged.GetMeshGL64();
            volumeOf(gl, piece.volume, piece.centroid);
            piece.mesh = fromManifold(gl, ops, layout, nullptr, namesWithDefault(first));
            piece.mesh.faceAttrs().remove(kCutAttr);
        }
        piece.cell = rep[size_t(clusterOrder[slotIndex])];
        auto neighbor = piece.mesh.faceAttrs().get<int32_t>(kNeighborAttr);
        for (int32_t& v : neighbor)
            if (auto it = repOfCell.find(v); v >= 0 && it != repOfCell.end())
                v = it->second;
        out[slotIndex] = std::move(piece);
    });
    for (const std::string& e : errors)
        if (!e.empty() && result.error.empty())
            result.error = e;
    result.pieces = std::move(out);
    result.contacts = buildContacts(result.pieces);
    return result;
}

FractureResult refinePieces(const FractureResult& in, int count, double minVolume, const FractureOptions& o)
{
    LILA_ZONE();

    FractureResult result;
    result.error = in.error;
    const size_t n = in.pieces.size();
    std::map<int, int> pieceOfCell;
    for (size_t i = 0; i < n; ++i)
        pieceOfCell.emplace(in.pieces[i].cell, int(i));

    // Old neighbours are stored as -2 - cell while the pieces are fractured, so they stay apart from the new cells.
    std::vector<std::vector<Piece>> children(n);
    std::vector<std::string> errors(n);
    tbb::parallel_for(size_t(0), n, [&](size_t i) {
        Piece base = in.pieces[i];
        for (int32_t& v : base.mesh.faceAttrs().get<int32_t>(kNeighborAttr))
            if (v >= 0)
                v = -2 - v;
        if (count >= 2 && base.volume > minVolume)
        {
            ScatterOptions so;
            so.count = count;
            so.seed = splitmix(o.seed ^ splitmix(uint64_t(int64_t(base.cell))));
            const std::vector<glm::dvec3> pts = scatterPoints(base.mesh, so);
            FractureResult sub = voronoiFracture(base.mesh, pts, o);
            errors[i] = sub.error;
            if (sub.ok() && !sub.pieces.empty())
            {
                children[i] = std::move(sub.pieces);
                return;
            }
        }
        children[i].push_back(std::move(base));
    });
    for (const std::string& e : errors)
        if (!e.empty() && result.error.empty())
            result.error = e;

    std::vector<int> firstChild(n + 1, 0);
    for (size_t i = 0; i < n; ++i)
        firstChild[i + 1] = firstChild[i] + int(children[i].size());

    // A face toward a refined neighbour goes to that neighbour's child under the face's centroid.
    auto childToward = [&](size_t parent, size_t neighbourParent, const glm::dvec3& at) {
        const std::vector<Piece>& kids = children[neighbourParent];
        if (kids.size() == 1)
            return firstChild[neighbourParent];
        const int32_t wanted = -2 - in.pieces[parent].cell;
        int best = firstChild[neighbourParent];
        double bestD = std::numeric_limits<double>::max();
        std::vector<Index> tri;
        for (size_t k = 0; k < kids.size(); ++k)
        {
            const geo::Mesh& m = kids[k].mesh;
            auto neighbor = m.faceAttrs().get<int32_t>(kNeighborAttr);
            for (Index f = 0; f < m.faceCapacity(); ++f)
            {
                if (!m.faceValid(f) || f >= neighbor.size() || neighbor[f] != wanted)
                    continue;
                const std::vector<Index> pts = m.facePoints(f);
                for (size_t j = 1; j + 1 < pts.size(); ++j)
                {
                    const double d = pointTriangleDistance(at, m.position(pts[0]), m.position(pts[j]), m.position(pts[j + 1]));
                    if (d < bestD)
                    {
                        bestD = d;
                        best = firstChild[neighbourParent] + int(k);
                    }
                }
            }
        }
        return best;
    };

    std::vector<std::vector<int32_t>> remapped(n);
    const size_t total = size_t(firstChild[n]);
    std::vector<Piece> out(total);
    tbb::parallel_for(size_t(0), n, [&](size_t i) {
        std::map<int, int> childOfCell;
        for (size_t k = 0; k < children[i].size(); ++k)
            childOfCell.emplace(children[i][k].cell, firstChild[i] + int(k));
        const bool refined = children[i].size() > 1 || children[i][0].cell != in.pieces[i].cell;
        for (size_t k = 0; k < children[i].size(); ++k)
        {
            Piece piece = children[i][k];
            geo::Mesh& m = piece.mesh;
            auto neighbor = m.faceAttrs().get<int32_t>(kNeighborAttr);
            for (Index f = 0; f < m.faceCapacity() && f < neighbor.size(); ++f)
            {
                if (!m.faceValid(f) || neighbor[f] == -1)
                    continue;
                if (neighbor[f] >= 0)
                {
                    auto it = childOfCell.find(neighbor[f]);
                    neighbor[f] = refined && it != childOfCell.end() ? it->second : -1;
                    continue;
                }
                auto it = pieceOfCell.find(-2 - neighbor[f]);
                neighbor[f] = it == pieceOfCell.end() ? -1 : childToward(i, size_t(it->second), m.faceCentroid(f));
            }
            piece.cell = firstChild[i] + int(k);
            out[size_t(piece.cell)] = std::move(piece);
        }
    });
    result.pieces = std::move(out);
    result.contacts = buildContacts(result.pieces);
    return result;
}
} // namespace lila::scene::fracture
