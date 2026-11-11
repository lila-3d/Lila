#include "bake/CpuBackend.h"

#include "bake/BakeInputs.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"

#include <tbb/parallel_for.h>

#include <numeric>

namespace lila::bake
{
namespace
{
/** A BVH over a soup, its triangles in soup order; soupTri maps BVH triangles to soup triangles (the identity). */
std::unique_ptr<geo::MeshBvh> buildBvh(const MeshSoup& soup, std::vector<uint32_t>& soupTri)
{
    LILA_ZONE();

    soupTri.resize(soup.triangleCount());
    std::iota(soupTri.begin(), soupTri.end(), 0u);
    auto bvh = std::make_unique<geo::MeshBvh>();
    bvh->buildSoup(soup.positions);
    return bvh;
}
} // namespace

CpuBackend::CpuBackend() = default;
CpuBackend::~CpuBackend() = default;

void CpuBackend::prepare(const BakeInputs& inputs)
{
    LILA_ZONE();

    mInputs = &inputs;
    mBvh.clear();
    mBvh.resize(inputs.groups.size());
    mSoupTri.assign(inputs.groups.size(), {});
    std::vector<std::pair<size_t, size_t>> jobs;
    for (size_t g = 0; g < inputs.groups.size(); ++g)
    {
        mBvh[g].resize(inputs.groups[g].highs.size());
        mSoupTri[g].resize(inputs.groups[g].highs.size());
        for (size_t h = 0; h < inputs.groups[g].highs.size(); ++h)
            jobs.emplace_back(g, h);
    }
    tbb::parallel_for(size_t(0), jobs.size(), [&](size_t i) {
        const auto [g, h] = jobs[i];
        const MeshSoup& soup = inputs.groups[g].highs[h].soup;
        if (soup.triangleCount())
            mBvh[g][h] = buildBvh(soup, mSoupTri[g][h]);
    });
}

void CpuBackend::traceProjection(size_t group, std::span<const TexelSample> samples, std::span<HitRecord> hits) const
{
    LILA_ZONE();

    const GroupInputs& gi = mInputs->groups[group];
    const auto& bvhs = mBvh[group];
    const bool backfaces = !gi.settings.ignoreBackfaces;
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const TexelSample& s = samples[i];
        HitRecord& r = hits[i];
        r = HitRecord{};
        r.high = kNoHit;
        r.triangle = kNoHit;
        const glm::dvec3 o(s.origin[0], s.origin[1], s.origin[2]), d(s.direction[0], s.direction[1], s.direction[2]);
        geo::BvhHit best;
        size_t bestHigh = SIZE_MAX;
        for (size_t h = 0; h < bvhs.size(); ++h)
        {
            if (!bvhs[h] || skips(gi, s.low, gi.highBase + uint32_t(h), RayKind::Projection))
                continue;
            geo::BvhHit hit;
            if (bvhs[h]->raycast(o, d, hit, s.rayLength, backfaces) && hit.t < best.t)
            {
                best = hit;
                bestHigh = h;
            }
        }
        if (bestHigh == SIZE_MAX)
        {
            if (!backfaces)
                for (size_t h = 0; h < bvhs.size(); ++h)
                {
                    const auto& b = bvhs[h];
                    geo::BvhHit hit;
                    if (b && !skips(gi, s.low, gi.highBase + uint32_t(h), RayKind::Projection) && b->raycast(o, d, hit, s.rayLength, true))
                    {
                        r.flags = kHitBackface;
                        break;
                    }
                }
            continue;
        }
        const MeshSoup& soup = gi.highs[bestHigh].soup;
        const uint32_t t = mSoupTri[group][bestHigh][best.triangle];
        const glm::vec3* n = &soup.normals[size_t(t) * 3];
        glm::vec3 nrm = n[0] * float(best.bary.x) + n[1] * float(best.bary.y) + n[2] * float(best.bary.z);
        const float l = glm::length(nrm);
        nrm = l > 0.0f ? nrm / l : glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 p(best.point);
        const glm::vec3 P(s.position[0], s.position[1], s.position[2]);
        for (int k = 0; k < 3; ++k)
        {
            r.position[k] = p[k];
            r.normal[k] = nrm[k];
        }
        r.high = uint32_t(bestHigh);
        r.triangle = t;
        r.bary[0] = float(best.bary.y);
        r.bary[1] = float(best.bary.z);
        r.distance = float(best.t);
        r.height = glm::dot(p - P, -glm::vec3(d));
        r.flags = kHitValid | (best.frontFacing ? 0u : uint32_t(kHitBackface));
    }
}

bool CpuBackend::closestHit(size_t group, OccluderScope scope, uint32_t high, uint32_t low, RayKind kind, const glm::vec3& origin,
                            const glm::vec3& dir, float tMax, TraceHit& out) const
{
    const glm::dvec3 o(origin), d(dir);
    geo::BvhHit best;
    size_t bestGroup = 0, bestHigh = SIZE_MAX;
    const GroupInputs& gi = mInputs->groups[group];
    auto test = [&](size_t g, size_t h) {
        if (skips(gi, low, mInputs->groups[g].highBase + uint32_t(h), kind))
            return;
        const auto& b = mBvh[g][h];
        geo::BvhHit hit;
        if (b && b->raycast(o, d, hit, double(tMax), true) && hit.t < best.t)
        {
            best = hit;
            bestGroup = g;
            bestHigh = h;
        }
    };
    if (scope == OccluderScope::Self)
    {
        if (high < mBvh[group].size())
            test(group, high);
    }
    else
        for (size_t g = 0; g < mBvh.size(); ++g)
            if (scope == OccluderScope::All || g == group)
                for (size_t h = 0; h < mBvh[g].size(); ++h)
                    test(g, h);
    if (bestHigh == SIZE_MAX)
        return false;
    const MeshSoup& soup = mInputs->groups[bestGroup].highs[bestHigh].soup;
    const uint32_t t = mSoupTri[bestGroup][bestHigh][best.triangle];
    const glm::vec3* n = &soup.normals[size_t(t) * 3];
    const glm::vec3 nrm = n[0] * float(best.bary.x) + n[1] * float(best.bary.y) + n[2] * float(best.bary.z);
    const float l = glm::length(nrm);
    out.distance = float(best.t);
    out.normal = l > 0.0f ? nrm / l : glm::vec3(0.0f);
    out.backface = !best.frontFacing;
    out.high = bestGroup == group ? uint32_t(bestHigh) : kNoHit;
    out.triangle = bestGroup == group ? t : kNoHit;
    return true;
}

bool CpuBackend::occluded(size_t group, OccluderScope scope, uint32_t high, uint32_t low, const glm::vec3& origin, const glm::vec3& dir,
                          float tMax) const
{
    const glm::dvec3 o(origin), d(dir);
    const GroupInputs& gi = mInputs->groups[group];
    auto any = [&](size_t g) {
        for (size_t h = 0; h < mBvh[g].size(); ++h)
            if (const auto& b = mBvh[g][h]; b && !skips(gi, low, mInputs->groups[g].highBase + uint32_t(h), RayKind::Occlusion) && b->occluded(o, d, tMax))
                return true;
        return false;
    };
    switch (scope)
    {
    case OccluderScope::Self:
        return high < mBvh[group].size() && mBvh[group][high] && mBvh[group][high]->occluded(o, d, tMax);
    case OccluderScope::Group: return any(group);
    case OccluderScope::All:
        for (size_t g = 0; g < mBvh.size(); ++g)
            if (any(g))
                return true;
        return false;
    }
    return false;
}
} // namespace lila::bake
