#include "geo/MultiBox.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Triangulate.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <queue>

namespace lila::geo
{
namespace
{
using V = glm::dvec3;

constexpr uint8_t kSurface = 1;
constexpr uint8_t kExterior = 2;

struct Grid
{
    V origin{0.0};
    double cell = 1.0;
    int n[3] = {1, 1, 1};
    std::vector<uint8_t> state;
    std::vector<uint8_t> filled;

    size_t size() const { return size_t(n[0]) * n[1] * n[2]; }
    size_t index(int x, int y, int z) const { return (size_t(z) * n[1] + y) * n[0] + x; }
    glm::ivec3 coords(size_t i) const { return {int(i % n[0]), int((i / n[0]) % n[1]), int(i / (size_t(n[0]) * n[1]))}; }
    V center(int x, int y, int z) const { return origin + (V(x, y, z) + 0.5) * cell; }
    glm::ivec3 cellOf(const V& p) const
    {
        glm::ivec3 c;
        for (int k = 0; k < 3; ++k)
            c[k] = std::clamp(int(std::floor((p[k] - origin[k]) / cell)), 0, n[k] - 1);
        return c;
    }
};

bool separated(const V& axis, const V& h, const V& a, const V& b, const V& c)
{
    double p0 = glm::dot(a, axis), p1 = glm::dot(b, axis), p2 = glm::dot(c, axis);
    double r = h.x * std::abs(axis.x) + h.y * std::abs(axis.y) + h.z * std::abs(axis.z);
    return std::min({p0, p1, p2}) > r || std::max({p0, p1, p2}) < -r;
}

bool triangleOverlapsBox(const V& center, const V& h, V a, V b, V c)
{
    a -= center;
    b -= center;
    c -= center;
    const V edges[3] = {b - a, c - b, a - c};
    for (const V& e : edges)
        for (int k = 0; k < 3; ++k)
        {
            V unit(0.0);
            unit[k] = 1.0;
            V axis = glm::cross(unit, e);
            if (glm::dot(axis, axis) > 1e-24 && separated(axis, h, a, b, c))
                return false;
        }
    for (int k = 0; k < 3; ++k)
        if (std::min({a[k], b[k], c[k]}) > h[k] || std::max({a[k], b[k], c[k]}) < -h[k])
            return false;
    return !separated(glm::cross(edges[0], edges[1]), h, a, b, c);
}

struct Sample
{
    V p;
    size_t voxel;
    int shell;
};

struct Voxelized
{
    Grid grid;
    std::vector<Sample> samples;
    int shells = 0;
};

/** Connected pieces of the mesh, numbered by their lowest point; points on no face get their own. */
std::vector<int> shellsOf(const Mesh& mesh, int& count)
{
    std::vector<Index> parent(mesh.pointCapacity());
    for (Index i = 0; i < parent.size(); ++i)
        parent[i] = i;
    auto find = [&](Index i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
        {
            std::vector<Index> pts = mesh.facePoints(f);
            for (size_t i = 1; i < pts.size(); ++i)
            {
                Index a = find(pts[0]), b = find(pts[i]);
                if (a != b)
                    parent[std::max(a, b)] = std::min(a, b);
            }
        }
    std::vector<int> id(mesh.pointCapacity(), -1), out(mesh.pointCapacity(), -1);
    count = 0;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
        {
            Index r = find(p);
            if (id[r] < 0)
                id[r] = count++;
            out[p] = id[r];
        }
    return out;
}

/** Surface cells by triangle overlap, outside by flood fill, and any cell an odd ray count calls inside on two axes. */
Voxelized voxelize(const Mesh& mesh, int resolution)
{
    LILA_ZONE();

    Voxelized out;
    Grid& g = out.grid;
    Bounds b;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            b.extend(mesh.position(p));
    if (!b.valid())
        return out;
    V ext = b.size();
    double longest = std::max({ext.x, ext.y, ext.z});
    if (longest <= 0.0)
        return out;
    g.cell = longest / std::clamp(resolution, 4, 256);
    g.origin = b.min - V(g.cell);
    for (int k = 0; k < 3; ++k)
        g.n[k] = int(std::ceil(ext[k] / g.cell)) + 3;
    g.state.assign(g.size(), 0);

    std::vector<int> shellOf = shellsOf(mesh, out.shells);
    MeshBvh bvh;
    bvh.build(mesh);
    const Triangulation& tris = bvh.triangles();
    const V half(g.cell * 0.5 * (1.0 + 1e-9));

    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
        {
            glm::ivec3 c = g.cellOf(mesh.position(p));
            g.state[g.index(c.x, c.y, c.z)] |= kSurface;
            out.samples.push_back({mesh.position(p), g.index(c.x, c.y, c.z), shellOf[p]});
        }

    for (size_t t = 0; t * 3 + 2 < tris.corners.size(); ++t)
    {
        V v[3];
        for (int k = 0; k < 3; ++k)
            v[k] = mesh.position(mesh.cornerPoint(tris.corners[t * 3 + k]));
        const int shell = shellOf[mesh.cornerPoint(tris.corners[t * 3])];
        Bounds tb;
        for (const V& p : v)
            tb.extend(p);
        glm::ivec3 lo = g.cellOf(tb.min), hi = g.cellOf(tb.max);
        for (int z = lo.z; z <= hi.z; ++z)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x)
                    if (triangleOverlapsBox(g.center(x, y, z), half, v[0], v[1], v[2]))
                        g.state[g.index(x, y, z)] |= kSurface;

        double longestEdge = std::max({glm::length(v[1] - v[0]), glm::length(v[2] - v[1]), glm::length(v[0] - v[2])});
        int div = std::clamp(int(std::ceil(longestEdge / (g.cell * 0.5))), 1, 32);
        for (int i = 0; i <= div; ++i)
            for (int j = 0; i + j <= div; ++j)
            {
                int k = div - i - j;
                if (i == div || j == div || k == div)
                    continue;
                V p = (v[0] * double(i) + v[1] * double(j) + v[2] * double(k)) / double(div);
                glm::ivec3 c = g.cellOf(p);
                out.samples.push_back({p, g.index(c.x, c.y, c.z), shell});
            }
    }

    std::vector<size_t> stack{0};
    g.state[0] |= kExterior;
    const int step[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    while (!stack.empty())
    {
        glm::ivec3 c = g.coords(stack.back());
        stack.pop_back();
        for (const auto& s : step)
        {
            int x = c.x + s[0], y = c.y + s[1], z = c.z + s[2];
            if (x < 0 || y < 0 || z < 0 || x >= g.n[0] || y >= g.n[1] || z >= g.n[2])
                continue;
            size_t i = g.index(x, y, z);
            if (g.state[i] & (kSurface | kExterior))
                continue;
            g.state[i] |= kExterior;
            stack.push_back(i);
        }
    }

    std::vector<uint8_t> votes(g.size(), 0);
    const double eps = g.cell * 1e-6;
    for (int axis = 0; axis < 3; ++axis)
    {
        const int u = (axis + 1) % 3, w = (axis + 2) % 3;
        const double ju = g.cell * (0.0713 + 0.0191 * axis), jw = g.cell * (0.0397 + 0.0263 * axis);
        const V dir = V(axis == 0, axis == 1, axis == 2);
        const double span = g.cell * (g.n[axis] + 2);
        tbb::parallel_for(tbb::blocked_range<int>(0, g.n[w]), [&](const tbb::blocked_range<int>& r) {
            std::vector<double> hits;
            for (int cw = r.begin(); cw < r.end(); ++cw)
                for (int cu = 0; cu < g.n[u]; ++cu)
                {
                    V o(0.0);
                    o[u] = g.origin[u] + (cu + 0.5) * g.cell + ju;
                    o[w] = g.origin[w] + (cw + 0.5) * g.cell + jw;
                    o[axis] = g.origin[axis] - g.cell;
                    hits.clear();
                    for (int guard = 0; guard < 4096; ++guard)
                    {
                        BvhHit hit;
                        if (!bvh.raycast(o, dir, hit, span))
                            break;
                        hits.push_back(hit.point[axis]);
                        o[axis] = hit.point[axis] + eps;
                    }
                    size_t h = 0;
                    for (int ca = 0; ca < g.n[axis]; ++ca)
                    {
                        double c = g.origin[axis] + (ca + 0.5) * g.cell;
                        while (h < hits.size() && hits[h] < c)
                            ++h;
                        if (h & 1)
                        {
                            int xyz[3];
                            xyz[axis] = ca, xyz[u] = cu, xyz[w] = cw;
                            ++votes[g.index(xyz[0], xyz[1], xyz[2])];
                        }
                    }
                }
        });
    }

    g.filled.assign(g.size(), 0);
    for (size_t i = 0; i < g.size(); ++i)
        g.filled[i] = (g.state[i] & kSurface) || !(g.state[i] & kExterior) || votes[i] >= 2;
    return out;
}

struct IBox
{
    int lo[3], hi[3];
};

class Counts
{
public:
    explicit Counts(const Grid& g) : mNx(g.n[0] + 1), mNy(g.n[1] + 1), mSum(size_t(g.n[0] + 1) * (g.n[1] + 1) * (g.n[2] + 1), 0)
    {
        for (int z = 0; z < g.n[2]; ++z)
            for (int y = 0; y < g.n[1]; ++y)
                for (int x = 0; x < g.n[0]; ++x)
                    at(x + 1, y + 1, z + 1) = g.filled[g.index(x, y, z)] + at(x, y + 1, z + 1) + at(x + 1, y, z + 1) + at(x + 1, y + 1, z) -
                                              at(x, y, z + 1) - at(x, y + 1, z) - at(x + 1, y, z) + at(x, y, z);
    }

    long in(const IBox& b) const
    {
        const int x0 = b.lo[0], y0 = b.lo[1], z0 = b.lo[2], x1 = b.hi[0] + 1, y1 = b.hi[1] + 1, z1 = b.hi[2] + 1;
        return long(get(x1, y1, z1)) - get(x0, y1, z1) - get(x1, y0, z1) - get(x1, y1, z0) + get(x0, y0, z1) + get(x0, y1, z0) +
               get(x1, y0, z0) - get(x0, y0, z0);
    }

private:
    int& at(int x, int y, int z) { return mSum[(size_t(z) * mNy + y) * mNx + x]; }
    int get(int x, int y, int z) const { return mSum[(size_t(z) * mNy + y) * mNx + x]; }
    int mNx, mNy;
    std::vector<int> mSum;
};

long volumeOf(const IBox& b)
{
    return long(b.hi[0] - b.lo[0] + 1) * (b.hi[1] - b.lo[1] + 1) * (b.hi[2] - b.lo[2] + 1);
}

IBox unionOf(const IBox& a, const IBox& b)
{
    IBox r;
    for (int k = 0; k < 3; ++k)
        r.lo[k] = std::min(a.lo[k], b.lo[k]), r.hi[k] = std::max(a.hi[k], b.hi[k]);
    return r;
}

bool nearEachOther(const IBox& a, const IBox& b)
{
    for (int k = 0; k < 3; ++k)
        if (std::max(b.lo[k] - a.hi[k] - 1, a.lo[k] - b.hi[k] - 1) > 1)
            return false;
    return true;
}

/** Exact cover of the filled cells by axis-aligned boxes, grown greedily along x, then y, then z. */
std::vector<IBox> greedyCover(const Grid& g, std::vector<int>& boxOf)
{
    LILA_ZONE();

    std::vector<IBox> boxes;
    boxOf.assign(g.size(), -1);
    auto free = [&](int x, int y, int z) {
        size_t i = g.index(x, y, z);
        return g.filled[i] && boxOf[i] < 0;
    };
    for (int z = 0; z < g.n[2]; ++z)
        for (int y = 0; y < g.n[1]; ++y)
            for (int x = 0; x < g.n[0]; ++x)
            {
                if (!free(x, y, z))
                    continue;
                int x1 = x;
                while (x1 + 1 < g.n[0] && free(x1 + 1, y, z))
                    ++x1;
                int y1 = y;
                for (bool ok = true; ok && y1 + 1 < g.n[1];)
                {
                    for (int i = x; i <= x1 && ok; ++i)
                        ok = free(i, y1 + 1, z);
                    y1 += ok;
                }
                int z1 = z;
                for (bool ok = true; ok && z1 + 1 < g.n[2];)
                {
                    for (int j = y; j <= y1 && ok; ++j)
                        for (int i = x; i <= x1 && ok; ++i)
                            ok = free(i, j, z1 + 1);
                    z1 += ok;
                }
                int id = int(boxes.size());
                boxes.push_back({{x, y, z}, {x1, y1, z1}});
                for (int k = z; k <= z1; ++k)
                    for (int j = y; j <= y1; ++j)
                        for (int i = x; i <= x1; ++i)
                            boxOf[g.index(i, j, k)] = id;
            }
    return boxes;
}

/** Merges boxes whose union stays full, best first. Returns each box's surviving group (a node id). */
std::vector<int> mergeRegions(const std::vector<IBox>& boxes, const Counts& counts, double minFill, size_t& groupCount)
{
    LILA_ZONE();

    struct Cand
    {
        double fill;
        int i, j;
        bool operator<(const Cand& o) const
        {
            if (fill != o.fill)
                return fill < o.fill;
            return i != o.i ? i > o.i : j > o.j;
        }
    };
    std::vector<IBox> nodes = boxes;
    std::vector<int> up(boxes.size());
    std::vector<uint8_t> alive(boxes.size(), 1);
    for (size_t i = 0; i < up.size(); ++i)
        up[i] = int(i);
    std::priority_queue<Cand> queue;
    auto consider = [&](int i, int j) {
        IBox u = unionOf(nodes[i], nodes[j]);
        double fill = double(counts.in(u)) / double(volumeOf(u));
        if (fill >= minFill)
            queue.push({fill, std::min(i, j), std::max(i, j)});
    };

    std::vector<int> order(boxes.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = int(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return boxes[a].lo[0] != boxes[b].lo[0] ? boxes[a].lo[0] < boxes[b].lo[0] : a < b; });
    for (size_t a = 0; a < order.size(); ++a)
        for (size_t b = a + 1; b < order.size(); ++b)
        {
            const IBox &A = boxes[order[a]], &B = boxes[order[b]];
            if (B.lo[0] - A.hi[0] - 1 > 1)
                break;
            if (nearEachOther(A, B))
                consider(order[a], order[b]);
        }

    while (!queue.empty())
    {
        Cand c = queue.top();
        queue.pop();
        if (!alive[c.i] || !alive[c.j])
            continue;
        int k = int(nodes.size());
        nodes.push_back(unionOf(nodes[c.i], nodes[c.j]));
        alive.push_back(1);
        up.push_back(k);
        alive[c.i] = alive[c.j] = 0;
        up[c.i] = up[c.j] = k;
        for (int o = 0; o < k; ++o)
            if (alive[o] && nearEachOther(nodes[k], nodes[o]))
                consider(o, k);
    }
    groupCount = nodes.size();
    std::vector<int> root(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        int r = int(i);
        while (up[r] != r)
            r = up[r];
        root[i] = r;
    }
    return root;
}

const std::vector<V>& probeDirections()
{
    static const std::vector<V> dirs = [] {
        std::vector<V> d{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const int n = 90;
        const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
        for (int i = 0; i < n; ++i)
        {
            double z = 1.0 - 2.0 * (i + 0.5) / n, r = std::sqrt(1.0 - z * z);
            d.emplace_back(r * std::cos(golden * i), r * std::sin(golden * i), z);
        }
        return d;
    }();
    return dirs;
}

struct Node
{
    std::vector<V> pts;
    /** Per probe direction, the point farthest along it: a small stand-in for the hull when solving frames. */
    std::vector<V> ext;
    std::vector<uint32_t> vox;
    OrientedBox box;
    double fill = 0.0;
    bool alive = true;
    bool unsplittable = false;
};

struct Decomposer
{
    const MultiBoxOptions& opts;
    Grid& g;
    const Counts& counts;
    std::vector<Node> nodes;

    /** Most the box could hold: every filled voxel in its axis-aligned extent. */
    double fillCeiling(const OrientedBox& b) const
    {
        V reach(0.0);
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k)
                reach[k] += std::abs(b.axes[i][k]) * (b.size[i] * 0.5 + g.cell);
        glm::ivec3 lo = g.cellOf(b.center - reach), hi = g.cellOf(b.center + reach);
        double vol = std::max(b.size.x, g.cell * 0.5) * std::max(b.size.y, g.cell * 0.5) * std::max(b.size.z, g.cell * 0.5);
        return double(counts.in({{lo.x, lo.y, lo.z}, {hi.x, hi.y, hi.z}})) * cell3() / vol;
    }

    double cell3() const { return g.cell * g.cell * g.cell; }

    static void probe(Node& n)
    {
        const std::vector<V>& dirs = probeDirections();
        n.ext.assign(dirs.size(), n.pts.empty() ? V(0.0) : n.pts[0]);
        for (size_t d = 0; d < dirs.size(); ++d)
        {
            double best = glm::dot(n.ext[d], dirs[d]);
            for (const V& p : n.pts)
                if (double t = glm::dot(p, dirs[d]); t > best)
                    best = t, n.ext[d] = p;
        }
    }

    static std::vector<V> mergedExtremes(const Node& a, const Node& b)
    {
        const std::vector<V>& dirs = probeDirections();
        std::vector<V> out(dirs.size());
        for (size_t d = 0; d < dirs.size(); ++d)
            out[d] = glm::dot(b.ext[d], dirs[d]) > glm::dot(a.ext[d], dirs[d]) ? b.ext[d] : a.ext[d];
        return out;
    }

    static OrientedBox unionInFrame(const OrientedBox& a, const OrientedBox& b)
    {
        OrientedBox r;
        r.axes = a.axes;
        for (int i = 0; i < 3; ++i)
        {
            double lo = std::min(glm::dot(a.center, a.axes[i]) - a.size[i] * 0.5, glm::dot(b.center, a.axes[i]) - b.size[i] * 0.5);
            double hi = std::max(glm::dot(a.center, a.axes[i]) + a.size[i] * 0.5, glm::dot(b.center, a.axes[i]) + b.size[i] * 0.5);
            r.size[i] = hi - lo;
            r.center += a.axes[i] * ((lo + hi) * 0.5);
        }
        return r;
    }

    /** The box around a node (or two): frame solved from the extreme points, extents measured over every point. */
    OrientedBox fit(const Node& a, const Node* b = nullptr) const
    {
        glm::dmat3 frame(1.0);
        if (opts.oriented)
        {
            std::vector<V> e = b ? mergedExtremes(a, *b) : a.ext;
            if (std::optional<OrientedBox> ob = minimumVolumeBox(e))
                frame = ob->axes;
        }
        OrientedBox r = boxInFrame(a.pts, frame);
        return b ? unionInFrame(r, boxInFrame(b->pts, frame)) : r;
    }

    /** Filled-voxel volume inside the box; each voxel counts by how much of it the box covers. */
    double occupied(const OrientedBox& b) const
    {
        V reach(0.0);
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k)
                reach[k] += std::abs(b.axes[i][k]) * (b.size[i] * 0.5 + g.cell);
        V width;
        for (int i = 0; i < 3; ++i)
            width[i] = g.cell * (std::abs(b.axes[i].x) + std::abs(b.axes[i].y) + std::abs(b.axes[i].z));
        glm::ivec3 lo = g.cellOf(b.center - reach), hi = g.cellOf(b.center + reach);
        double cells = 0.0;
        for (int z = lo.z; z <= hi.z; ++z)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x)
                {
                    if (!g.filled[g.index(x, y, z)])
                        continue;
                    V d = g.center(x, y, z) - b.center;
                    double share = 1.0;
                    for (int i = 0; i < 3 && share > 0.0; ++i)
                        share *= std::clamp((b.size[i] * 0.5 - std::abs(glm::dot(d, b.axes[i]))) / width[i] + 0.5, 0.0, 1.0);
                    cells += share;
                }
        return cells * cell3();
    }

    double fillOf(const OrientedBox& b) const
    {
        double vol = std::max(b.size.x, g.cell * 0.5) * std::max(b.size.y, g.cell * 0.5) * std::max(b.size.z, g.cell * 0.5);
        return std::min(1.0, occupied(b) / vol);
    }

    void refit(Node& n) const
    {
        probe(n);
        n.box = fit(n);
        n.fill = fillOf(n.box);
    }

    std::pair<V, V> worldBounds(const OrientedBox& b) const
    {
        V reach(0.0);
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k)
                reach[k] += std::abs(b.axes[i][k]) * (b.size[i] * 0.5 + g.cell * 0.5);
        return {b.center - reach, b.center + reach};
    }

    bool adjacent(const Node& a, const Node& b) const
    {
        auto [alo, ahi] = worldBounds(a.box);
        auto [blo, bhi] = worldBounds(b.box);
        for (int k = 0; k < 3; ++k)
            if (blo[k] > ahi[k] + g.cell || alo[k] > bhi[k] + g.cell)
                return false;
        return true;
    }

    int aliveCount() const
    {
        return int(std::count_if(nodes.begin(), nodes.end(), [](const Node& n) { return n.alive; }));
    }

    /** Merges the best neighbouring pair while its refit box stays at or above the threshold; past a count limit it keeps going regardless. */
    void merge(double threshold, int limit = -1)
    {
        LILA_ZONE();

        struct Cand
        {
            double fill;
            int i, j;
            OrientedBox box;
            bool operator<(const Cand& o) const
            {
                if (fill != o.fill)
                    return fill < o.fill;
                return i != o.i ? i > o.i : j > o.j;
            }
        };
        std::priority_queue<Cand> queue;
        bool anyPair = false;
        double floor = threshold;
        auto consider = [&](const std::vector<std::pair<int, int>>& pairs) {
            std::vector<std::pair<int, int>> todo;
            for (auto [i, j] : pairs)
                if (anyPair || adjacent(nodes[i], nodes[j]))
                    todo.emplace_back(i, j);
            std::vector<Cand> cands(todo.size());
            std::vector<uint8_t> keep(todo.size(), 0);
            tbb::parallel_for(size_t(0), todo.size(), [&](size_t k) {
                auto [i, j] = todo[k];
                OrientedBox box = fit(nodes[i], &nodes[j]);
                if (floor > 0.0 && fillCeiling(box) < floor)
                    return;
                double fill = fillOf(box);
                if (fill >= floor)
                    cands[k] = {fill, i, j, box}, keep[k] = 1;
            });
            for (size_t k = 0; k < todo.size(); ++k)
                if (keep[k])
                    queue.push(cands[k]);
        };
        auto seed = [&] {
            queue = {};
            std::vector<std::pair<int, int>> pairs;
            for (int i = 0; i < int(nodes.size()); ++i)
                for (int j = i + 1; j < int(nodes.size()); ++j)
                    if (nodes[i].alive && nodes[j].alive)
                        pairs.emplace_back(i, j);
            consider(pairs);
        };
        seed();
        int count = aliveCount();
        while (count > 1 && (limit < 0 || count > limit))
        {
            if (queue.empty())
            {
                if (limit < 0 || anyPair)
                    break;
                anyPair = true;
                floor = 0.0;
                seed();
                if (queue.empty())
                    break;
            }
            Cand c = queue.top();
            queue.pop();
            if (!nodes[c.i].alive || !nodes[c.j].alive)
                continue;
            Node merged;
            merged.pts = std::move(nodes[c.i].pts);
            merged.pts.insert(merged.pts.end(), nodes[c.j].pts.begin(), nodes[c.j].pts.end());
            merged.vox = std::move(nodes[c.i].vox);
            merged.vox.insert(merged.vox.end(), nodes[c.j].vox.begin(), nodes[c.j].vox.end());
            merged.ext = mergedExtremes(nodes[c.i], nodes[c.j]);
            merged.box = c.box;
            merged.fill = c.fill;
            nodes[c.i].alive = nodes[c.j].alive = false;
            nodes[c.i].pts.clear();
            nodes[c.j].pts.clear();
            int k = int(nodes.size());
            nodes.push_back(std::move(merged));
            --count;
            std::vector<std::pair<int, int>> pairs;
            for (int o = 0; o < k; ++o)
                if (nodes[o].alive)
                    pairs.emplace_back(o, k);
            consider(pairs);
        }
    }

    struct Cut
    {
        int axis = -1;
        int bin = 0;
    };

    struct Bin
    {
        int count = 0;
        V lo{std::numeric_limits<double>::max()};
        V hi{-std::numeric_limits<double>::max()};
    };

    static void absorb(Bin& a, const Bin& b)
    {
        a.count += b.count;
        a.lo = glm::min(a.lo, b.lo);
        a.hi = glm::max(a.hi, b.hi);
    }

    double binnedVolume(const Bin& b) const
    {
        return (b.hi.x - b.lo.x + g.cell) * (b.hi.y - b.lo.y + g.cell) * (b.hi.z - b.lo.z + g.cell);
    }

    /** The plane along the box's frame that leaves the two voxel halves the least combined volume, if it beats the whole. */
    Cut bestCut(const Node& n) const
    {
        const OrientedBox& box = n.box;
        const double minExtent = opts.minBoxSize > 0.0 ? opts.minBoxSize : 2.0 * g.cell;
        int nb[3];
        for (int i = 0; i < 3; ++i)
            nb[i] = std::max(1, int(std::ceil((box.size[i] + g.cell) / g.cell - 1e-9)));
        std::vector<Bin> bins[3];
        for (int i = 0; i < 3; ++i)
            bins[i].resize(nb[i]);
        Bin whole;
        for (uint32_t v : n.vox)
        {
            glm::ivec3 c = g.coords(v);
            V d = g.center(c.x, c.y, c.z) - box.center;
            V u(glm::dot(d, box.axes[0]), glm::dot(d, box.axes[1]), glm::dot(d, box.axes[2]));
            Bin one{1, u, u};
            absorb(whole, one);
            for (int i = 0; i < 3; ++i)
            {
                int k = std::clamp(int(std::floor((u[i] + (box.size[i] + g.cell) * 0.5) / g.cell)), 0, nb[i] - 1);
                absorb(bins[i][k], one);
            }
        }
        double best = binnedVolume(whole) * 0.97;
        Cut cut;
        for (int i = 0; i < 3; ++i)
        {
            std::vector<Bin> right(nb[i] + 1);
            for (int k = nb[i] - 1; k >= 0; --k)
            {
                right[k] = right[k + 1];
                absorb(right[k], bins[i][k]);
            }
            Bin left;
            for (int k = 0; k + 1 < nb[i]; ++k)
            {
                absorb(left, bins[i][k]);
                const Bin& r = right[k + 1];
                if (!left.count || !r.count)
                    continue;
                if (left.hi[i] - left.lo[i] + g.cell < minExtent || r.hi[i] - r.lo[i] + g.cell < minExtent)
                    continue;
                double cost = binnedVolume(left) + binnedVolume(r);
                if (cost < best)
                    best = cost, cut = {i, k};
            }
        }
        return cut;
    }

    void addCorners(std::vector<V>& pts, uint32_t v) const
    {
        glm::ivec3 c = g.coords(v);
        for (int k = 0; k < 8; ++k)
            pts.push_back(g.origin + (V(c) + V(k & 1, (k >> 1) & 1, (k >> 2) & 1)) * g.cell);
    }

    void split(int index, const Cut& cut)
    {
        Node parent = std::move(nodes[index]);
        nodes[index].alive = false;
        const OrientedBox& box = parent.box;
        const int a = cut.axis;
        const double plane = -(box.size[a] + g.cell) * 0.5 + (cut.bin + 1) * g.cell;
        Node halves[2];
        auto side = [&](const V& p) { return glm::dot(p - box.center, box.axes[a]) < plane ? 0 : 1; };
        for (const V& p : parent.pts)
            halves[side(p)].pts.push_back(p);
        for (uint32_t v : parent.vox)
        {
            glm::ivec3 c = g.coords(v);
            V center = g.center(c.x, c.y, c.z);
            int s = side(center);
            halves[s].vox.push_back(v);
            if (!(g.state[v] & kSurface) && std::abs(glm::dot(center - box.center, box.axes[a]) - plane) <= g.cell)
                addCorners(halves[s].pts, v);
        }
        for (Node& h : halves)
        {
            if (h.pts.empty())
                for (uint32_t v : h.vox)
                    addCorners(h.pts, v);
            refit(h);
            nodes.push_back(std::move(h));
        }
    }

    void splitWorst()
    {
        LILA_ZONE();

        while (aliveCount() < std::max(1, opts.maxBoxes))
        {
            int pick = -1;
            double worst = 0.0;
            for (int i = 0; i < int(nodes.size()); ++i)
            {
                const Node& n = nodes[i];
                if (!n.alive || n.unsplittable || n.fill >= opts.minFill)
                    continue;
                double waste = (n.box.size.x + g.cell) * (n.box.size.y + g.cell) * (n.box.size.z + g.cell) * (1.0 - n.fill);
                if (waste > worst)
                    worst = waste, pick = i;
            }
            if (pick < 0)
                return;
            Cut cut = bestCut(nodes[pick]);
            if (cut.axis < 0)
            {
                nodes[pick].unsplittable = true;
                continue;
            }
            split(pick, cut);
        }
    }
};

double meshVolume(const Mesh& m)
{
    double sum = 0.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<Index> pts = m.facePoints(f);
        for (size_t i = 1; i + 1 < pts.size(); ++i)
            sum += glm::dot(m.position(pts[0]), glm::cross(m.position(pts[i]), m.position(pts[i + 1])));
    }
    return std::abs(sum) / 6.0;
}
} // namespace

MultiBoxResult multiBoxDecompose(const Mesh& mesh, const MultiBoxOptions& opts)
{
    LILA_ZONE();

    MultiBoxResult result;
    Voxelized vx = voxelize(mesh, opts.resolution);
    Grid& g = vx.grid;
    if (g.state.empty())
        return result;

    Counts counts(g);
    std::vector<int> label(g.size(), -1);
    int regions = 0;
    // Separate shells are the mesh's own parts, so they seed the regions when there are several.
    const bool byShell = vx.shells > 1 && vx.shells <= 4096;
    if (byShell)
    {
        regions = vx.shells;
        std::vector<size_t> frontier;
        for (const Sample& s : vx.samples)
            if (label[s.voxel] < 0)
                label[s.voxel] = s.shell, frontier.push_back(s.voxel);
        const int step6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (size_t head = 0; head < frontier.size(); ++head)
        {
            glm::ivec3 c = g.coords(frontier[head]);
            for (const auto& st : step6)
            {
                int x = c.x + st[0], y = c.y + st[1], z = c.z + st[2];
                if (x < 0 || y < 0 || z < 0 || x >= g.n[0] || y >= g.n[1] || z >= g.n[2])
                    continue;
                size_t i = g.index(x, y, z);
                if (g.filled[i] && label[i] < 0)
                    label[i] = label[frontier[head]], frontier.push_back(i);
            }
        }
        for (size_t i = 0; i < g.size(); ++i)
            if (g.filled[i] && label[i] < 0)
                label[i] = 0;
    }
    else
    {
        std::vector<int> boxOf;
        std::vector<IBox> cover = greedyCover(g, boxOf);
        if (cover.empty())
            return result;
        size_t groups = 0;
        std::vector<int> root = mergeRegions(cover, counts, opts.minFill, groups);
        std::vector<int> regionOfRoot(groups, -1);
        for (int r : root)
            if (regionOfRoot[r] < 0)
                regionOfRoot[r] = regions++;
        for (size_t i = 0; i < g.size(); ++i)
            if (boxOf[i] >= 0)
                label[i] = regionOfRoot[root[boxOf[i]]];
    }

    Decomposer d{opts, g, counts, {}};
    d.nodes.resize(regions);
    const int step[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (size_t i = 0; i < g.size(); ++i)
    {
        if (label[i] < 0)
            continue;
        Node& n = d.nodes[label[i]];
        n.vox.push_back(uint32_t(i));
        if ((g.state[i] & kSurface) || byShell)
            continue;
        glm::ivec3 c = g.coords(i);
        bool rim = false;
        for (const auto& s : step)
        {
            int x = c.x + s[0], y = c.y + s[1], z = c.z + s[2];
            rim = rim || x < 0 || y < 0 || z < 0 || x >= g.n[0] || y >= g.n[1] || z >= g.n[2] || label[g.index(x, y, z)] != label[i];
        }
        if (rim)
            d.addCorners(n.pts, uint32_t(i));
    }
    for (const Sample& s : vx.samples)
    {
        int l = byShell ? s.shell : label[s.voxel];
        if (l < 0)
        {
            glm::ivec3 c = g.coords(s.voxel);
            for (int dz = -1; dz <= 1 && l < 0; ++dz)
                for (int dy = -1; dy <= 1 && l < 0; ++dy)
                    for (int dx = -1; dx <= 1 && l < 0; ++dx)
                    {
                        int x = c.x + dx, y = c.y + dy, z = c.z + dz;
                        if (x >= 0 && y >= 0 && z >= 0 && x < g.n[0] && y < g.n[1] && z < g.n[2])
                            l = label[g.index(x, y, z)];
                    }
        }
        d.nodes[std::max(l, 0)].pts.push_back(s.p);
    }
    for (Node& n : d.nodes)
        if (n.pts.empty())
            for (uint32_t v : n.vox)
                d.addCorners(n.pts, v);
    tbb::parallel_for(size_t(0), d.nodes.size(), [&](size_t i) { d.refit(d.nodes[i]); });

    const int limit = std::max(1, opts.maxBoxes);
    d.merge(opts.minFill);
    d.merge(0.0, limit);
    d.splitWorst();
    d.merge(opts.minFill);
    d.merge(0.0, limit);

    std::vector<const Node*> live;
    for (const Node& n : d.nodes)
        if (n.alive)
            live.push_back(&n);
    std::sort(live.begin(), live.end(), [](const Node* a, const Node* b) {
        const V &p = a->box.center, &q = b->box.center;
        if (p.x != q.x)
            return p.x < q.x;
        if (p.y != q.y)
            return p.y < q.y;
        return p.z < q.z;
    });

    std::vector<uint8_t> covered(g.size(), 0);
    double fillSum = 0.0;
    for (const Node* n : live)
    {
        BoxChunk chunk;
        chunk.box = n->box;
        for (int i = 0; i < 3; ++i)
            chunk.box.size[i] = std::max(chunk.box.size[i], g.cell * 0.5);
        chunk.fill = n->fill;
        chunk.volume = chunk.box.size.x * chunk.box.size.y * chunk.box.size.z;

        auto [lo, hi] = d.worldBounds(n->box);
        glm::ivec3 clo = g.cellOf(lo), chi = g.cellOf(hi);
        for (int z = clo.z; z <= chi.z; ++z)
            for (int y = clo.y; y <= chi.y; ++y)
                for (int x = clo.x; x <= chi.x; ++x)
                {
                    V dv = g.center(x, y, z) - n->box.center;
                    bool in = true;
                    for (int i = 0; i < 3 && in; ++i)
                        in = std::abs(glm::dot(dv, n->box.axes[i])) <= (n->box.size[i] + g.cell) * 0.5 + 1e-9;
                    if (in)
                        covered[g.index(x, y, z)] = 1;
                }

        if (opts.shape != "box")
        {
            Mesh hull = convexHullMesh(n->pts, opts.maxHullPoints, 0.0);
            bool use = hull.numFaces() > 0;
            if (use && opts.shape == "auto")
                use = meshVolume(hull) < 0.9 * chunk.volume;
            if (use)
            {
                chunk.hull = true;
                chunk.hullMesh = opts.padding > 0.0 ? convexHullMesh(n->pts, opts.maxHullPoints, opts.padding) : std::move(hull);
            }
        }
        for (int i = 0; i < 3; ++i)
            chunk.box.size[i] += 2.0 * opts.padding;
        fillSum += chunk.fill;
        result.chunks.push_back(std::move(chunk));
    }
    result.averageFill = fillSum / double(result.chunks.size());
    size_t total = 0, hit = 0;
    for (size_t i = 0; i < g.size(); ++i)
    {
        total += g.filled[i];
        hit += g.filled[i] && covered[i];
    }
    result.coverage = total ? double(hit) / double(total) : 0.0;
    return result;
}
} // namespace lila::geo
