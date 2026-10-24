#include "brush/Surface.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace lila::brush
{
std::vector<std::pair<geo::Index, float>> SurfaceWalk::gather(const geo::Mesh& m, const geo::MeshBvh& bvh, geo::Index triangle, const Dab& d,
                                                              const Falloff& falloff)
{
    LILA_ZONE();

    std::vector<std::pair<geo::Index, float>> out;
    const double radius = d.radius;
    if (triangle == geo::kInvalid || radius <= 0.0)
        return out;
    const bool round = roundTip(d);
    if (!round && (!mHaveTable || mTableFor.shape != falloff.shape || !(mTableFor.curve == falloff.curve)))
    {
        mTable = FalloffTable(falloff);
        mTableFor = falloff;
        mHaveTable = true;
    }
    constexpr double kFar = std::numeric_limits<double>::infinity();
    if (mDist.size() != m.pointCapacity())
        mDist.assign(m.pointCapacity(), kFar);
    using Item = std::pair<double, geo::Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    std::vector<geo::Index> touched;
    auto relax = [&](geo::Index p, double dist) {
        if (dist > radius || dist >= mDist[p])
            return;
        if (mDist[p] == kFar)
            touched.push_back(p);
        mDist[p] = dist;
        queue.push({dist, p});
    };
    const auto& corners = bvh.triangles().corners;
    for (int k = 0; k < 3; ++k)
    {
        const geo::Index p = m.cornerPoint(corners[size_t(triangle) * 3 + size_t(k)]);
        relax(p, glm::length(m.position(p) - d.center));
    }
    while (!queue.empty())
    {
        auto [dist, p] = queue.top();
        queue.pop();
        if (dist > mDist[p])
            continue;
        m.forEachPointEdge(p, [&](geo::Index e) {
            const geo::Index o = m.edgeOtherPoint(e, p);
            relax(o, dist + glm::length(m.position(o) - m.position(p)));
        });
    }
    out.reserve(touched.size());
    for (geo::Index p : touched)
    {
        const double dist = mDist[p];
        mDist[p] = kFar;
        float w;
        if (round)
            w = float(falloff.evaluate(dist / radius));
        else
        {
            // The straight direction to the point at its distance along the surface, so the tip's shape lies on the surface.
            const glm::dvec3 rel = m.position(p) - d.center;
            const double len = glm::length(rel);
            w = tipWeight(d, mTable, len > 0.0 ? rel * (dist / len) : rel);
        }
        out.push_back({p, w});
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool SurfaceWalk::snap(const geo::MeshBvh& bvh, Dab& d, geo::Index& triangle)
{
    geo::BvhClosest c;
    if (!bvh.closestPoint(d.center, c, d.radius))
        return false;
    d.center = c.point;
    triangle = c.triangle;
    return true;
}
} // namespace lila::brush
