// Convex decomposition by voxels and axis-aligned cuts. Hull volumes come from Jolt's hull builder (hullVolume), so
// a part counts as convex by the same hull the simulation will use.

#include "Decompose.h"

#include "RigidScene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace lila::physics
{
namespace
{
enum : uint8_t
{
    kEmpty = 0,
    kInside = 1,
    kSurface = 2,
};

struct Grid
{
    glm::ivec3 n{1};
    glm::dvec3 lo{0.0};
    double h = 1.0;
    std::vector<uint8_t> cells;
    std::vector<int32_t> sums; // solid cells in [0, x) x [0, y) x [0, z), (n + 1)^3

    size_t at(int x, int y, int z) const { return (size_t(z) * size_t(n.y) + size_t(y)) * size_t(n.x) + size_t(x); }
    size_t sumAt(int x, int y, int z) const { return (size_t(z) * size_t(n.y + 1) + size_t(y)) * size_t(n.x + 1) + size_t(x); }

    glm::ivec3 cellOf(const glm::dvec3& p) const
    {
        const glm::dvec3 f = (p - lo) / h;
        return glm::clamp(glm::ivec3(glm::floor(f)), glm::ivec3(0), n - 1);
    }

    void buildSums()
    {
        sums.assign(size_t(n.x + 1) * size_t(n.y + 1) * size_t(n.z + 1), 0);
        for (int z = 1; z <= n.z; ++z)
            for (int y = 1; y <= n.y; ++y)
                for (int x = 1; x <= n.x; ++x)
                    sums[sumAt(x, y, z)] = (cells[at(x - 1, y - 1, z - 1)] != kEmpty ? 1 : 0) + sums[sumAt(x - 1, y, z)] + sums[sumAt(x, y - 1, z)] +
                                           sums[sumAt(x, y, z - 1)] - sums[sumAt(x - 1, y - 1, z)] - sums[sumAt(x - 1, y, z - 1)] -
                                           sums[sumAt(x, y - 1, z - 1)] + sums[sumAt(x - 1, y - 1, z - 1)];
    }

    /** Solid cells in [a, b). */
    int64_t solid(const glm::ivec3& a, const glm::ivec3& b) const
    {
        if (a.x >= b.x || a.y >= b.y || a.z >= b.z)
            return 0;
        auto s = [&](int x, int y, int z) { return int64_t(sums[sumAt(x, y, z)]); };
        return s(b.x, b.y, b.z) - s(a.x, b.y, b.z) - s(b.x, a.y, b.z) - s(b.x, b.y, a.z) + s(a.x, a.y, b.z) + s(a.x, b.y, a.z) + s(b.x, a.y, a.z) -
               s(a.x, a.y, a.z);
    }

    /** The box shrunk to the solid cells inside it; empty (a == b) when there are none. */
    void shrink(glm::ivec3& a, glm::ivec3& b) const
    {
        if (solid(a, b) == 0)
        {
            b = a;
            return;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            auto slab = [&](int from, int to) {
                glm::ivec3 sa = a, sb = b;
                sa[axis] = from;
                sb[axis] = to;
                return solid(sa, sb);
            };
            while (b[axis] - a[axis] > 1 && slab(a[axis], a[axis] + 1) == 0)
                ++a[axis];
            while (b[axis] - a[axis] > 1 && slab(b[axis] - 1, b[axis]) == 0)
                --b[axis];
        }
    }
};

/** Fills the cells inside a closed mesh: rays along +X through each row of cell centres, inside between crossings. */
void fillInside(Grid& g, const std::vector<glm::dvec3>& t)
{
    // Rows a ray passes through a triangle's corner or edge would count it twice; a tiny irrational nudge avoids that.
    const double ny = g.h * 1e-4 * 0.6180339887, nz = g.h * 1e-4 * 0.4142135624;
    std::vector<std::vector<uint32_t>> rows(size_t(g.n.y) * size_t(g.n.z));
    for (size_t i = 0; i + 2 < t.size(); i += 3)
    {
        const double y0 = std::min({t[i].y, t[i + 1].y, t[i + 2].y}), y1 = std::max({t[i].y, t[i + 1].y, t[i + 2].y});
        const double z0 = std::min({t[i].z, t[i + 1].z, t[i + 2].z}), z1 = std::max({t[i].z, t[i + 1].z, t[i + 2].z});
        const int ja = std::max(0, int(std::ceil((y0 - g.lo.y) / g.h - 0.5 - 1e-9))), jb = std::min(g.n.y - 1, int(std::floor((y1 - g.lo.y) / g.h - 0.5 + 1e-9)));
        const int ka = std::max(0, int(std::ceil((z0 - g.lo.z) / g.h - 0.5 - 1e-9))), kb = std::min(g.n.z - 1, int(std::floor((z1 - g.lo.z) / g.h - 0.5 + 1e-9)));
        for (int k = ka; k <= kb; ++k)
            for (int j = ja; j <= jb; ++j)
                rows[size_t(k) * size_t(g.n.y) + size_t(j)].push_back(uint32_t(i));
    }
    std::vector<double> xs;
    for (int k = 0; k < g.n.z; ++k)
        for (int j = 0; j < g.n.y; ++j)
        {
            const std::vector<uint32_t>& row = rows[size_t(k) * size_t(g.n.y) + size_t(j)];
            if (row.size() < 2)
                continue;
            const double py = g.lo.y + (j + 0.5) * g.h + ny, pz = g.lo.z + (k + 0.5) * g.h + nz;
            xs.clear();
            for (uint32_t i : row)
            {
                const glm::dvec3 &a = t[i], &b = t[i + 1], &c = t[i + 2];
                const double den = (b.y - a.y) * (c.z - a.z) - (c.y - a.y) * (b.z - a.z);
                if (std::abs(den) < 1e-18)
                    continue;
                const double u = ((py - a.y) * (c.z - a.z) - (c.y - a.y) * (pz - a.z)) / den;
                const double v = ((b.y - a.y) * (pz - a.z) - (py - a.y) * (b.z - a.z)) / den;
                if (u < 0.0 || v < 0.0 || u + v > 1.0)
                    continue;
                xs.push_back(a.x + u * (b.x - a.x) + v * (c.x - a.x));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t p = 0; p + 1 < xs.size(); p += 2)
            {
                const int ia = std::max(0, int(std::ceil((xs[p] - g.lo.x) / g.h - 0.5)));
                const int ib = std::min(g.n.x - 1, int(std::floor((xs[p + 1] - g.lo.x) / g.h - 0.5)));
                for (int i = ia; i <= ib; ++i)
                    g.cells[g.at(i, j, k)] = kInside;
            }
        }
}

struct Sample
{
    glm::dvec3 p;
    glm::ivec3 cell;
};

/** Points over every triangle about half a cell apart (and the corners), marking the cells they fall in as surface. */
std::vector<Sample> sampleSurface(Grid& g, const std::vector<glm::dvec3>& t)
{
    const double spacing = g.h * 0.5;
    const double quantum = spacing * 0.5;
    std::unordered_set<uint64_t> seen;
    std::vector<Sample> out;
    auto add = [&](const glm::dvec3& p) {
        const glm::dvec3 q = glm::floor((p - g.lo) / quantum);
        const uint64_t key = (uint64_t(int64_t(q.x) & 0x1FFFFF) << 42) | (uint64_t(int64_t(q.y) & 0x1FFFFF) << 21) | uint64_t(int64_t(q.z) & 0x1FFFFF);
        if (!seen.insert(key).second)
            return;
        const glm::ivec3 c = g.cellOf(p);
        g.cells[g.at(c.x, c.y, c.z)] = kSurface;
        out.push_back({p, c});
    };
    for (size_t i = 0; i + 2 < t.size(); i += 3)
    {
        const glm::dvec3 &a = t[i], &b = t[i + 1], &c = t[i + 2];
        const double longest = std::max({glm::length(b - a), glm::length(c - b), glm::length(a - c)});
        const int m = std::clamp(int(std::ceil(longest / spacing)), 1, 256);
        for (int u = 0; u <= m; ++u)
            for (int v = 0; u + v <= m; ++v)
            {
                const double fu = double(u) / m, fv = double(v) / m;
                add(a + (b - a) * fu + (c - a) * fv);
            }
    }
    return out;
}

/** Hull volume of at most about cap of the points, every k-th taken: enough to compare cuts. */
double roughHullVolume(const std::vector<glm::dvec3>& pts, size_t cap = 1024)
{
    if (pts.size() <= cap)
        return hullVolume(pts);
    std::vector<glm::dvec3> few;
    const size_t step = (pts.size() + cap - 1) / cap;
    few.reserve(cap + 1);
    for (size_t k = 0; k < pts.size(); k += step)
        few.push_back(pts[k]);
    return hullVolume(few);
}

struct Part
{
    glm::ivec3 a{0}, b{0};
    std::vector<Sample> samples;
    std::vector<glm::dvec3> extra; // corners on the cuts that made it
    double hull = 0.0, solid = 0.0;
    bool final = false;

    std::vector<glm::dvec3> points() const
    {
        std::vector<glm::dvec3> out;
        out.reserve(samples.size() + extra.size());
        for (const Sample& s : samples)
            out.push_back(s.p);
        out.insert(out.end(), extra.begin(), extra.end());
        return out;
    }
    double concavity() const { return hull > 0.0 ? (hull - solid) / hull : 0.0; }
};

void measure(const Grid& g, Part& p)
{
    p.solid = double(g.solid(p.a, p.b)) * g.h * g.h * g.h;
    p.hull = roughHullVolume(p.points());
}

/**
 * The corners of the cell faces on the plane between index c - 1 and c along axis, where solid interior meets solid
 * interior inside the box: closes the gap a cut would leave between the two hulls.
 */
std::vector<glm::dvec3> cutCorners(const Grid& g, const glm::ivec3& a, const glm::ivec3& b, int axis, int c)
{
    std::vector<glm::dvec3> out;
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    for (int i = a[u]; i < b[u]; ++i)
        for (int j = a[v]; j < b[v]; ++j)
        {
            glm::ivec3 lo, hi;
            lo[axis] = c - 1;
            hi[axis] = c;
            lo[u] = hi[u] = i;
            lo[v] = hi[v] = j;
            if (g.cells[g.at(lo.x, lo.y, lo.z)] != kInside || g.cells[g.at(hi.x, hi.y, hi.z)] != kInside)
                continue;
            for (int du = 0; du <= 1; ++du)
                for (int dv = 0; dv <= 1; ++dv)
                {
                    glm::dvec3 p;
                    p[axis] = g.lo[axis] + c * g.h;
                    p[u] = g.lo[u] + (i + du) * g.h;
                    p[v] = g.lo[v] + (j + dv) * g.h;
                    out.push_back(p);
                }
        }
    return out;
}

/** The two halves of a part cut at index c along axis, before shrinking. */
void splitAt(const Grid& g, const Part& p, int axis, int c, Part& lo, Part& hi)
{
    lo = {};
    hi = {};
    lo.a = p.a;
    lo.b = p.b;
    lo.b[axis] = c;
    hi.a = p.a;
    hi.a[axis] = c;
    hi.b = p.b;
    for (const Sample& s : p.samples)
        (s.cell[axis] < c ? lo : hi).samples.push_back(s);
    const double plane = g.lo[axis] + c * g.h;
    for (const glm::dvec3& e : p.extra)
    {
        if (e[axis] <= plane + g.h * 1e-6)
            lo.extra.push_back(e);
        if (e[axis] >= plane - g.h * 1e-6)
            hi.extra.push_back(e);
    }
}
} // namespace

std::vector<std::vector<glm::dvec3>> decompose(const std::vector<glm::dvec3>& t, const DecomposeOptions& o)
{
    std::vector<std::vector<glm::dvec3>> out;
    if (t.size() < 3)
        return out;
    glm::dvec3 lo(1e300), hi(-1e300);
    for (const glm::dvec3& p : t)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::dvec3 size = hi - lo;
    const double longest = std::max({size.x, size.y, size.z});
    if (!(longest > 0.0))
        return out;

    Grid g;
    g.h = longest / std::clamp(o.resolution, 4, 256);
    // Half a cell of margin all round, so surface samples on the bounds land inside the grid.
    g.lo = lo - glm::dvec3(g.h * 0.5);
    for (int k = 0; k < 3; ++k)
        g.n[k] = std::max(1, int(std::ceil((size[k] + g.h) / g.h)));
    g.cells.assign(size_t(g.n.x) * size_t(g.n.y) * size_t(g.n.z), kEmpty);
    fillInside(g, t);
    std::vector<Sample> samples = sampleSurface(g, t);
    g.buildSums();

    std::vector<Part> parts(1);
    parts[0].a = glm::ivec3(0);
    parts[0].b = g.n;
    g.shrink(parts[0].a, parts[0].b);
    parts[0].samples = std::move(samples);
    measure(g, parts[0]);

    const int maxParts = std::max(1, o.maxParts);
    while (int(parts.size()) < maxParts)
    {
        // The part with the most hull volume to spare is cut first.
        int worst = -1;
        double spare = 0.0;
        for (size_t k = 0; k < parts.size(); ++k)
        {
            const Part& p = parts[k];
            if (p.final || p.concavity() <= o.concavity)
                continue;
            if (p.hull - p.solid > spare)
            {
                spare = p.hull - p.solid;
                worst = int(k);
            }
        }
        if (worst < 0)
            break;
        Part& p = parts[size_t(worst)];
        int bestAxis = -1, bestCut = 0;
        double bestCost = 1e300;
        for (int axis = 0; axis < 3; ++axis)
        {
            const int extent = p.b[axis] - p.a[axis];
            if (extent < 2)
                continue;
            const int tries = std::min(extent - 1, 7);
            int last = -1;
            for (int k = 1; k <= tries; ++k)
            {
                const int c = p.a[axis] + int(std::lround(double(k) * extent / (tries + 1)));
                if (c <= p.a[axis] || c >= p.b[axis] || c == last)
                    continue;
                last = c;
                Part l, h;
                splitAt(g, p, axis, c, l, h);
                if (g.solid(l.a, l.b) == 0 || g.solid(h.a, h.b) == 0)
                    continue;
                const double cost = roughHullVolume(l.points(), 384) + roughHullVolume(h.points(), 384);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestAxis = axis;
                    bestCut = c;
                }
            }
        }
        if (bestAxis < 0)
        {
            p.final = true;
            continue;
        }
        Part l, h;
        splitAt(g, p, bestAxis, bestCut, l, h);
        const std::vector<glm::dvec3> corners = cutCorners(g, p.a, p.b, bestAxis, bestCut);
        l.extra.insert(l.extra.end(), corners.begin(), corners.end());
        h.extra.insert(h.extra.end(), corners.begin(), corners.end());
        for (Part* q : {&l, &h})
        {
            g.shrink(q->a, q->b);
            measure(g, *q);
        }
        // A cut that saves no hull volume only adds parts.
        if (l.hull + h.hull >= p.hull * 0.98)
        {
            p.final = true;
            continue;
        }
        p = std::move(l);
        parts.push_back(std::move(h));
    }

    for (const Part& p : parts)
    {
        std::vector<glm::dvec3> pts = p.points();
        if (pts.size() >= 4)
            out.push_back(std::move(pts));
    }
    if (out.empty())
        out.push_back(t);
    return out;
}
} // namespace lila::physics
