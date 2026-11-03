// Settlement: routes on walkable meshes (a structure's ramps, terraces and floors), as shortest paths over its faces.

#include "SettlementCore.h"

#include <lila/Profile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>

namespace settlement
{
namespace
{
constexpr double kWalkPi = 3.14159265358979323846;

int64_t walkKey(int x, int y, int z) { return (int64_t(x & 0x1FFFFF) << 42) | (int64_t(y & 0x1FFFFF) << 21) | int64_t(z & 0x1FFFFF); }

double pointLine3(const glm::dvec3& q, const glm::dvec3& a, const glm::dvec3& b)
{
    const glm::dvec3 d = b - a;
    const double len2 = glm::dot(d, d);
    const double t = len2 > 1e-12 ? std::clamp(glm::dot(q - a, d) / len2, 0.0, 1.0) : 0.0;
    return glm::length(a + d * t - q);
}

void rdp3(const std::vector<glm::dvec3>& pts, size_t lo, size_t hi, double tol, std::vector<char>& keep)
{
    if (hi <= lo + 1)
        return;
    double worst = -1.0;
    size_t at = lo;
    for (size_t k = lo + 1; k < hi; ++k)
        if (const double d = pointLine3(pts[k], pts[lo], pts[hi]); d > worst)
        {
            worst = d;
            at = k;
        }
    if (worst > tol)
    {
        keep[at] = 1;
        rdp3(pts, lo, at, tol, keep);
        rdp3(pts, at, hi, tol, keep);
    }
}
} // namespace

WalkSurface WalkSurface::fromMesh(const geo::Mesh& mesh, const WalkOptions& o)
{
    LILA_ZONE();

    WalkSurface w;
    const double minUp = std::cos(std::clamp(o.maxSlope, 0.0, 89.0) * kWalkPi / 180.0);
    std::vector<geo::Index> faces;
    double edgeLength = 0.0;
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f) && mesh.faceSize(f) >= 3 && mesh.faceNormal(f).y >= minUp)
        {
            faces.push_back(f);
            const std::vector<geo::Index> pts = mesh.facePoints(f);
            for (size_t k = 0; k < pts.size(); ++k)
                edgeLength += glm::length(mesh.position(pts[(k + 1) % pts.size()]) - mesh.position(pts[k]));
        }
    if (faces.empty())
        return w;
    // Large meshes get fewer points along their edges, so the graph stays near a few hundred thousand nodes.
    const double spacing = std::max(o.spacing, 0.5 * edgeLength / 300000.0);

    std::unordered_map<geo::Index, int> pointNode;
    auto nodeOf = [&](geo::Index p) {
        auto [it, fresh] = pointNode.try_emplace(p, int(w.mNodes.size()));
        if (fresh)
            w.mNodes.push_back(mesh.position(p));
        return it->second;
    };
    struct EdgeNodes
    {
        std::vector<int> inner; // from the lower point id to the higher
        int faces = 0;
    };
    std::unordered_map<uint64_t, EdgeNodes> edges;
    auto edgeOf = [&](geo::Index a, geo::Index b) -> EdgeNodes& {
        const geo::Index lo = std::min(a, b), hi = std::max(a, b);
        auto [it, fresh] = edges.try_emplace((uint64_t(lo) << 32) | hi);
        if (fresh)
        {
            const glm::dvec3 pa = mesh.position(lo), pb = mesh.position(hi);
            const int n = std::min(6, int(glm::length(pb - pa) / spacing));
            for (int k = 1; k <= n; ++k)
            {
                it->second.inner.push_back(int(w.mNodes.size()));
                w.mNodes.push_back(pa + (pb - pa) * (double(k) / (n + 1)));
            }
        }
        return it->second;
    };
    std::vector<std::vector<std::pair<int, float>>>& links = w.mLinks;
    auto link = [&](int a, int b, double cost) {
        if (a == b)
            return;
        if (size_t(std::max(a, b)) >= links.size())
            links.resize(size_t(std::max(a, b)) + 1);
        links[size_t(a)].push_back({b, float(cost)});
        links[size_t(b)].push_back({a, float(cost)});
    };
    std::vector<int> ring;
    geo::Mesh walkable;
    std::unordered_map<geo::Index, geo::Index> walkPoint;
    for (geo::Index f : faces)
    {
        const std::vector<geo::Index> pts = mesh.facePoints(f);
        std::vector<geo::Index> copy;
        for (geo::Index p : pts)
        {
            auto [it, fresh] = walkPoint.try_emplace(p, 0);
            if (fresh)
                it->second = walkable.addPoint(mesh.position(p));
            copy.push_back(it->second);
        }
        const geo::Index made = walkable.addFace(copy);
        ring.clear();
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const geo::Index a = pts[k], b = pts[(k + 1) % pts.size()];
            ring.push_back(nodeOf(a));
            EdgeNodes& e = edgeOf(a, b);
            ++e.faces;
            if (a < b)
                ring.insert(ring.end(), e.inner.begin(), e.inner.end());
            else
                ring.insert(ring.end(), e.inner.rbegin(), e.inner.rend());
        }
        // Every pair across the face: straight lines on a flat face are the shortest ways over it.
        for (size_t i = 0; i < ring.size(); ++i)
            for (size_t j = i + 1; j < ring.size(); ++j)
                link(ring[i], ring[j], glm::length(w.mNodes[size_t(ring[j])] - w.mNodes[size_t(ring[i])]));
        if (made != geo::kInvalid)
        {
            if (w.mRings.size() <= size_t(made))
                w.mRings.resize(size_t(made) + 1);
            w.mRings[size_t(made)] = ring;
        }
    }
    links.resize(w.mNodes.size());

    // Separate pieces close in height join at their open edges: a step up, a threshold, a gap between slabs.
    std::vector<int> border;
    for (const auto& [key, e] : edges)
        if (e.faces == 1)
        {
            border.push_back(nodeOf(geo::Index(key >> 32)));
            border.push_back(nodeOf(geo::Index(key & 0xFFFFFFFFu)));
            border.insert(border.end(), e.inner.begin(), e.inner.end());
        }
    std::sort(border.begin(), border.end());
    border.erase(std::unique(border.begin(), border.end()), border.end());
    if (!border.empty() && o.gap > 0.0)
    {
        const double c = std::max(o.gap, 1.0);
        std::unordered_map<int64_t, std::vector<int>> cells;
        auto cellOf = [&](const glm::dvec3& p) {
            return std::array<int, 3>{int(std::floor(p.x / c)), int(std::floor(p.y / std::max(o.step, 1.0))), int(std::floor(p.z / c))};
        };
        for (int n : border)
        {
            const auto k = cellOf(w.mNodes[size_t(n)]);
            cells[walkKey(k[0], k[1], k[2])].push_back(n);
        }
        for (int n : border)
        {
            const glm::dvec3 p = w.mNodes[size_t(n)];
            const auto k = cellOf(p);
            std::vector<std::pair<double, int>> near;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        auto it = cells.find(walkKey(k[0] + dx, k[1] + dy, k[2] + dz));
                        if (it == cells.end())
                            continue;
                        for (int m : it->second)
                        {
                            if (m <= n)
                                continue;
                            const glm::dvec3 q = w.mNodes[size_t(m)];
                            const double flat = glm::length(glm::dvec2(q.x - p.x, q.z - p.z));
                            if (flat <= o.gap && std::abs(q.y - p.y) <= o.step)
                                near.push_back({glm::length(q - p), m});
                        }
                    }
            std::sort(near.begin(), near.end());
            for (size_t i = 0; i < near.size() && i < 3; ++i)
                link(n, near[i].second, near[i].first * 1.5);
        }
    }

    w.mBvh = std::make_shared<const geo::MeshBvh>(walkable);
    return w;
}

bool WalkSurface::locate(glm::dvec3 p, double reach, Spot* out) const
{
    if (!mBvh || mBvh->empty())
        return false;
    geo::BvhClosest c;
    if (!mBvh->closestPoint(p, c, reach) || c.face == geo::kInvalid || size_t(c.face) >= mRings.size())
        return false;
    out->p = c.point;
    out->face = int(c.face);
    return true;
}

std::vector<glm::dvec3> WalkSurface::search(const Spot& a, const Spot* b, int node, std::vector<double>* all) const
{
    LILA_ZONE();

    if (a.face < 0 || size_t(a.face) >= mRings.size())
        return {};
    if (b && b->face == a.face)
        return {a.p, b->p};
    const glm::dvec3 goal = b ? b->p : node >= 0 ? mNodes[size_t(node)] : glm::dvec3(0.0);
    const bool guided = !all;
    auto h = [&](int n) { return guided ? glm::length(goal - mNodes[size_t(n)]) : 0.0; };
    std::vector<double> dist(mNodes.size(), std::numeric_limits<double>::infinity());
    std::vector<int> prev(mNodes.size(), -1);
    std::vector<char> inGoal;
    if (b)
    {
        if (b->face < 0 || size_t(b->face) >= mRings.size())
            return {};
        inGoal.assign(mNodes.size(), 0);
        for (int n : mRings[size_t(b->face)])
            inGoal[size_t(n)] = 1;
    }
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> q;
    for (int n : mRings[size_t(a.face)])
    {
        const double d = glm::length(mNodes[size_t(n)] - a.p);
        if (d < dist[size_t(n)])
        {
            dist[size_t(n)] = d;
            q.push({d + h(n), n});
        }
    }
    double best = std::numeric_limits<double>::infinity();
    int last = -1;
    while (!q.empty())
    {
        const auto [f, n] = q.top();
        q.pop();
        if (f - h(n) > dist[size_t(n)] + 1e-6)
            continue;
        if (guided && f >= best)
            break;
        if (b && inGoal[size_t(n)])
            if (const double total = dist[size_t(n)] + glm::length(b->p - mNodes[size_t(n)]); total < best)
            {
                best = total;
                last = n;
            }
        if (!b && n == node)
        {
            last = n;
            break;
        }
        for (const auto& [m, cost] : mLinks[size_t(n)])
            if (const double nd = dist[size_t(n)] + cost; nd < dist[size_t(m)])
            {
                dist[size_t(m)] = nd;
                prev[size_t(m)] = n;
                q.push({nd + h(m), m});
            }
    }
    if (all)
    {
        *all = std::move(dist);
        return {};
    }
    if (last < 0)
        return {};
    std::vector<glm::dvec3> pts;
    for (int n = last; n != -1 && pts.size() <= mNodes.size(); n = prev[size_t(n)])
        pts.push_back(mNodes[size_t(n)]);
    pts.push_back(a.p);
    std::reverse(pts.begin(), pts.end());
    if (b)
        pts.push_back(b->p);
    if (pts.size() < 3)
        return pts;
    std::vector<char> keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    rdp3(pts, 0, pts.size() - 1, 10.0, keep);
    std::vector<glm::dvec3> out;
    for (size_t k = 0; k < pts.size(); ++k)
        if (keep[k] && (out.empty() || glm::length(pts[k] - out.back()) > 1e-6))
            out.push_back(pts[k]);
    return out;
}

std::vector<glm::dvec3> WalkSurface::route(const Spot& a, const Spot& b) const { return search(a, &b, -1, nullptr); }

std::vector<glm::dvec3> WalkSurface::route(const Spot& a, int node) const { return search(a, nullptr, node, nullptr); }

std::vector<double> WalkSurface::distances(const Spot& a) const
{
    std::vector<double> all(mNodes.size(), std::numeric_limits<double>::infinity());
    search(a, nullptr, -1, &all);
    return all;
}
} // namespace settlement
