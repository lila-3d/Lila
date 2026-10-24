#pragma once

#include "core/Export.h"

#include "bake/Backend.h"

#include <memory>
#include <vector>

namespace lila::geo
{
class MeshBvh;
}

namespace lila::bake
{
/** The reference backend: geo::MeshBvh per high, TBB parallel from the caller's loops. */
class LILA_SCENE_API CpuBackend final : public Backend, public Tracer
{
public:
    CpuBackend();
    ~CpuBackend() override;
    CpuBackend(const CpuBackend&) = delete;
    CpuBackend& operator=(const CpuBackend&) = delete;

    std::string name() const override { return "cpu"; }
    void prepare(const BakeInputs& inputs) override;
    void traceProjection(size_t group, std::span<const TexelSample> samples, std::span<HitRecord> hits) const override;
    const Tracer* tracer() const override { return this; }

    bool occluded(size_t group, OccluderScope scope, uint32_t high, const glm::vec3& origin, const glm::vec3& dir, float tMax) const override;

    /** A high's BVH after prepare, null for an empty high. */
    const geo::MeshBvh* bvh(size_t group, size_t high) const { return mBvh[group][high].get(); }
    /** The soup triangle of each of that BVH's triangles. */
    const std::vector<uint32_t>& soupTriangles(size_t group, size_t high) const { return mSoupTri[group][high]; }
    bool closestHit(size_t group, OccluderScope scope, uint32_t high, const glm::vec3& origin, const glm::vec3& dir, float tMax,
                    TraceHit& hit) const override;

private:
    const BakeInputs* mInputs = nullptr;
    /** Per group, per high. */
    std::vector<std::vector<std::unique_ptr<geo::MeshBvh>>> mBvh;
    /** Per group, per high: soup triangle of each BVH triangle. */
    std::vector<std::vector<std::vector<uint32_t>>> mSoupTri;
};
} // namespace lila::bake
