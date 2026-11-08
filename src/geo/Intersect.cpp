#include "geo/Intersect.h"

#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <unordered_map>

namespace lila::geo
{
namespace
{
struct Segment
{
    glm::dvec3 a, b;
    Index faceA = kInvalid;
    Index faceB = kInvalid;
};

/** Signed distances of v to the plane (n, o), snapped to 0 within eps. False when all lie strictly on one side. */
bool planeSides(const glm::dvec3 v[3], const glm::dvec3& n, const glm::dvec3& o, double eps, double d[3])
{
    int pos = 0, neg = 0;
    for (int i = 0; i < 3; ++i)
    {
        d[i] = glm::dot(n, v[i] - o);
        if (std::abs(d[i]) <= eps)
            d[i] = 0.0;
        pos += d[i] > 0.0;
        neg += d[i] < 0.0;
    }
    return pos != 3 && neg != 3;
}

/** The part of a triangle on the other's plane, as its two extreme points along dir. */
bool planeCut(const glm::dvec3 v[3], const double d[3], const glm::dvec3& dir, glm::dvec3& lo, glm::dvec3& hi)
{
    glm::dvec3 pts[4];
    int n = 0;
    for (int i = 0; i < 3; ++i)
    {
        const int j = (i + 1) % 3;
        if (d[i] == 0.0)
            pts[n++] = v[i];
        if (d[i] * d[j] < 0.0 && n < 4)
            pts[n++] = v[i] + (v[j] - v[i]) * (d[i] / (d[i] - d[j]));
    }
    if (n < 2)
        return false;
    double tLo = glm::dot(pts[0], dir), tHi = tLo;
    lo = hi = pts[0];
    for (int k = 1; k < n; ++k)
    {
        const double t = glm::dot(pts[k], dir);
        if (t < tLo)
            tLo = t, lo = pts[k];
        if (t > tHi)
            tHi = t, hi = pts[k];
    }
    return tHi > tLo;
}

glm::dvec3 xform(const glm::dmat4& m, const glm::dvec3& p)
{
    return glm::dvec3(m * glm::dvec4(p, 1.0));
}

using SegmentLists = tbb::enumerable_thread_specific<std::vector<Segment>>;

std::vector<Segment> gather(SegmentLists& lists)
{
    std::vector<Segment> all;
    for (auto& l : lists)
        all.insert(all.end(), l.begin(), l.end());
    // Thread order varies; sorting by faces and position makes chaining deterministic.
    std::sort(all.begin(), all.end(), [](const Segment& x, const Segment& y) {
        if (x.faceA != y.faceA)
            return x.faceA < y.faceA;
        if (x.faceB != y.faceB)
            return x.faceB < y.faceB;
        for (int k = 0; k < 3; ++k)
            if (x.a[k] != y.a[k])
                return x.a[k] < y.a[k];
        for (int k = 0; k < 3; ++k)
            if (x.b[k] != y.b[k])
                return x.b[k] < y.b[k];
        return false;
    });
    return all;
}

/** Welds segment endpoints within tol and walks them into polylines: open chains from their ends first, then loops. */
std::vector<IntersectionPolyline> chain(const std::vector<Segment>& segs, double tol)
{
    std::vector<IntersectionPolyline> out;
    if (segs.empty())
        return out;
    tol = std::max(tol, 1e-12);
    struct Key
    {
        int64_t x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const
        {
            return size_t(k.x * 73856093) ^ size_t(k.y * 19349663) ^ size_t(k.z * 83492791);
        }
    };
    std::unordered_map<Key, std::vector<int>, KeyHash> grid;
    std::vector<glm::dvec3> nodes;
    auto node = [&](const glm::dvec3& p) {
        const Key c{int64_t(std::floor(p.x / tol)), int64_t(std::floor(p.y / tol)), int64_t(std::floor(p.z / tol))};
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz)
                    if (auto it = grid.find(Key{c.x + dx, c.y + dy, c.z + dz}); it != grid.end())
                        for (int id : it->second)
                            if (glm::distance(nodes[size_t(id)], p) <= tol)
                                return id;
        const int id = int(nodes.size());
        nodes.push_back(p);
        grid[c].push_back(id);
        return id;
    };

    struct Edge
    {
        int u, v;
        size_t seg;
    };
    std::vector<Edge> edges;
    std::map<std::pair<int, int>, size_t> seen;
    for (size_t s = 0; s < segs.size(); ++s)
    {
        const int u = node(segs[s].a), v = node(segs[s].b);
        if (u == v)
            continue;
        // Crossings along a shared edge come from both neighbouring triangles; keep one.
        if (!seen.try_emplace({std::min(u, v), std::max(u, v)}, s).second)
            continue;
        edges.push_back({u, v, s});
    }
    std::vector<std::vector<size_t>> adj(nodes.size());
    for (size_t e = 0; e < edges.size(); ++e)
    {
        adj[size_t(edges[e].u)].push_back(e);
        adj[size_t(edges[e].v)].push_back(e);
    }
    std::vector<char> used(edges.size(), 0);
    auto walk = [&](int start, size_t firstEdge) {
        IntersectionPolyline line;
        line.points.push_back(nodes[size_t(start)]);
        int at = start;
        size_t e = firstEdge;
        while (true)
        {
            used[e] = 1;
            const Edge& ed = edges[e];
            at = ed.u == at ? ed.v : ed.u;
            line.facesA.push_back(segs[ed.seg].faceA);
            line.facesB.push_back(segs[ed.seg].faceB);
            if (at == start)
            {
                line.closed = true;
                break;
            }
            line.points.push_back(nodes[size_t(at)]);
            if (adj[size_t(at)].size() != 2)
                break;
            size_t next = SIZE_MAX;
            for (size_t c : adj[size_t(at)])
                if (!used[c])
                    next = c;
            if (next == SIZE_MAX)
                break;
            e = next;
        }
        out.push_back(std::move(line));
    };
    for (size_t n = 0; n < nodes.size(); ++n)
        if (adj[n].size() != 2)
            for (size_t e : adj[n])
                if (!used[e])
                    walk(int(n), e);
    for (size_t e = 0; e < edges.size(); ++e)
        if (!used[e])
            walk(edges[e].u, e);
    return out;
}

MeshIntersection finish(SegmentLists& lists, size_t coplanar, double weld)
{
    MeshIntersection r;
    const std::vector<Segment> segs = gather(lists);
    r.segments = segs.size();
    r.coplanarPairs = coplanar;
    r.lines = chain(segs, weld);
    return r;
}

/**
 * Crosses each probe triangle (placed in the target's space by probeToTarget) with the target triangles under it.
 * Segments come back in the target's space; swapped puts the probe's faces in faceA.
 */
void probeInto(const MeshBvh& probe, const MeshBvh& target, const glm::dmat4& probeToTarget, bool swapped,
               SegmentLists& lists, std::atomic<size_t>& coplanar)
{
    const Bounds tb = target.bounds();
    const Triangulation& pt = probe.triangles();
    const Triangulation& tt = target.triangles();
    tbb::parallel_for(tbb::blocked_range<Index>(0, probe.triangleCount(), 256), [&](const tbb::blocked_range<Index>& r) {
        std::vector<Segment>& local = lists.local();
        for (Index t = r.begin(); t < r.end(); ++t)
        {
            glm::dvec3 p[3];
            probe.trianglePoints(t, p[0], p[1], p[2]);
            Bounds box;
            for (glm::dvec3& v : p)
            {
                v = xform(probeToTarget, v);
                box.extend(v);
            }
            bool overlap = true;
            for (int k = 0; k < 3; ++k)
                overlap &= box.min[k] <= tb.max[k] && box.max[k] >= tb.min[k];
            if (!overlap)
                continue;
            for (Index u : target.overlapping(box))
            {
                glm::dvec3 q[3];
                target.trianglePoints(u, q[0], q[1], q[2]);
                glm::dvec3 s0, s1;
                bool flat = false;
                if (!triangleIntersection(p, q, s0, s1, &flat))
                {
                    coplanar += flat;
                    continue;
                }
                const Index pf = pt.faces[size_t(t)], tf = tt.faces[size_t(u)];
                local.push_back({s0, s1, swapped ? pf : tf, swapped ? tf : pf});
            }
        }
    });
}
} // namespace

bool triangleIntersection(const glm::dvec3 p[3], const glm::dvec3 q[3], glm::dvec3& s0, glm::dvec3& s1, bool* coplanar)
{
    if (coplanar)
        *coplanar = false;
    const glm::dvec3 np = glm::cross(p[1] - p[0], p[2] - p[0]);
    const glm::dvec3 nq = glm::cross(q[1] - q[0], q[2] - q[0]);
    const double lp = glm::length(np), lq = glm::length(nq);
    if (lp == 0.0 || lq == 0.0)
        return false;
    // Distances are scaled by the normal's length (twice the area), so eps scales with the triangle's size.
    const double scale = std::max({glm::length(p[1] - p[0]), glm::length(p[2] - p[0]), glm::length(q[1] - q[0]),
                                   glm::length(q[2] - q[0])});
    const double rel = 1e-10 * scale;
    double dp[3], dq[3];
    if (!planeSides(p, nq, q[0], rel * lq, dp))
        return false;
    if (dp[0] == 0.0 && dp[1] == 0.0 && dp[2] == 0.0)
    {
        if (coplanar)
            *coplanar = true;
        return false;
    }
    if (!planeSides(q, np, p[0], rel * lp, dq))
        return false;
    const glm::dvec3 dir = glm::cross(np, nq);
    if (glm::dot(dir, dir) == 0.0)
        return false;
    glm::dvec3 pLo, pHi, qLo, qHi;
    if (!planeCut(p, dp, dir, pLo, pHi) || !planeCut(q, dq, dir, qLo, qHi))
        return false;
    const double a0 = glm::dot(pLo, dir), a1 = glm::dot(pHi, dir);
    const double b0 = glm::dot(qLo, dir), b1 = glm::dot(qHi, dir);
    if (std::max(a0, b0) >= std::min(a1, b1))
        return false;
    s0 = a0 >= b0 ? pLo : qLo;
    s1 = a1 <= b1 ? pHi : qHi;
    return true;
}

MeshIntersection intersectMeshes(const MeshBvh& a, const MeshBvh& b, const glm::dmat4& bToA, double weld)
{
    SegmentLists lists;
    std::atomic<size_t> coplanar{0};
    if (a.empty() || b.empty())
        return {};
    // The smaller mesh probes the larger one's tree.
    if (b.triangleCount() <= a.triangleCount())
    {
        probeInto(b, a, bToA, false, lists, coplanar);
    }
    else
    {
        probeInto(a, b, glm::inverse(bToA), true, lists, coplanar);
        for (auto& l : lists)
            for (Segment& s : l)
                s.a = xform(bToA, s.a), s.b = xform(bToA, s.b);
    }
    return finish(lists, coplanar, weld);
}

std::vector<ObjectIntersection> intersectObjects(std::span<const IntersectionInput> inputs, double weld)
{
    std::vector<Bounds> world(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        if (!inputs[i].bvh || inputs[i].bvh->empty())
            continue;
        const Bounds b = inputs[i].bvh->bounds();
        for (int c = 0; c < 8; ++c)
            world[i].extend(xform(inputs[i].world, glm::dvec3(c & 1 ? b.max.x : b.min.x, c & 2 ? b.max.y : b.min.y,
                                                              c & 4 ? b.max.z : b.min.z)));
    }
    std::vector<std::pair<size_t, size_t>> pairs;
    for (size_t i = 0; i < inputs.size(); ++i)
        for (size_t j = i + 1; j < inputs.size(); ++j)
        {
            if (!world[i].valid() || !world[j].valid() || inputs[i].object == inputs[j].object)
                continue;
            bool overlap = true;
            for (int k = 0; k < 3; ++k)
                overlap &= world[i].min[k] <= world[j].max[k] && world[i].max[k] >= world[j].min[k];
            if (overlap)
                pairs.push_back({i, j});
        }
    std::vector<ObjectIntersection> out;
    for (const auto& [i, j] : pairs)
    {
        const glm::dmat4 bToA = glm::inverse(inputs[i].world) * inputs[j].world;
        MeshIntersection r = intersectMeshes(*inputs[i].bvh, *inputs[j].bvh, bToA, weld);
        if (r.lines.empty())
            continue;
        for (IntersectionPolyline& l : r.lines)
            for (glm::dvec3& p : l.points)
                p = xform(inputs[i].world, p);
        out.push_back({i, j, std::move(r)});
    }
    return out;
}

MeshIntersection intersectHeightfield(const MeshBvh& mesh, const glm::dmat4& meshToHeightfield, const Heightfield& ground,
                                      double weld, const std::string& holes)
{
    SegmentLists lists;
    std::atomic<size_t> coplanar{0};
    if (mesh.empty() || ground.empty())
        return {};
    const std::vector<float>* holeMask = holes.empty() ? nullptr : ground.findMask(holes);
    const Triangulation& tris = mesh.triangles();
    const int cx = ground.nx - 1, cz = ground.nz - 1;
    tbb::parallel_for(tbb::blocked_range<Index>(0, mesh.triangleCount(), 256), [&](const tbb::blocked_range<Index>& r) {
        std::vector<Segment>& local = lists.local();
        for (Index t = r.begin(); t < r.end(); ++t)
        {
            glm::dvec3 p[3];
            mesh.trianglePoints(t, p[0], p[1], p[2]);
            Bounds box;
            for (glm::dvec3& v : p)
            {
                v = xform(meshToHeightfield, v);
                box.extend(v);
            }
            const int x0 = std::max(0, int(std::floor((box.min.x - ground.origin.x) / ground.cell)));
            const int x1 = std::min(cx - 1, int(std::floor((box.max.x - ground.origin.x) / ground.cell)));
            const int z0 = std::max(0, int(std::floor((box.min.z - ground.origin.y) / ground.cell)));
            const int z1 = std::min(cz - 1, int(std::floor((box.max.z - ground.origin.y) / ground.cell)));
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                {
                    const float h[4] = {ground.at(x, z), ground.at(x + 1, z), ground.at(x, z + 1), ground.at(x + 1, z + 1)};
                    if (box.min.y > std::max({h[0], h[1], h[2], h[3]}) || box.max.y < std::min({h[0], h[1], h[2], h[3]}))
                        continue;
                    if (holeMask)
                    {
                        const auto& m = *holeMask;
                        if (m[ground.index(x, z)] > 0.5f || m[ground.index(x + 1, z)] > 0.5f ||
                            m[ground.index(x, z + 1)] > 0.5f || m[ground.index(x + 1, z + 1)] > 0.5f)
                            continue;
                    }
                    const glm::dvec3 c00 = ground.position(x, z), c10 = ground.position(x + 1, z);
                    const glm::dvec3 c01 = ground.position(x, z + 1), c11 = ground.position(x + 1, z + 1);
                    const glm::dvec3 halves[2][3] = {{c00, c11, c10}, {c00, c01, c11}};
                    for (int half = 0; half < 2; ++half)
                    {
                        glm::dvec3 s0, s1;
                        bool flat = false;
                        if (!triangleIntersection(p, halves[half], s0, s1, &flat))
                        {
                            coplanar += flat;
                            continue;
                        }
                        local.push_back({s0, s1, tris.faces[size_t(t)], Index((size_t(z) * size_t(cx) + size_t(x)) * 2 + size_t(half))});
                    }
                }
        }
    });
    return finish(lists, coplanar, weld);
}
} // namespace lila::geo
