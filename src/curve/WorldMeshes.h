#pragma once

#include "core/Export.h"
#include "curve/Cable.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"

#include <memory>
#include <optional>
#include <vector>

namespace lila::curve
{
/** Meshes placed in the world, for ray casts and distance queries in world space (routing and supports). */
class LILA_SCENE_API WorldMeshes
{
public:
    void add(std::shared_ptr<const geo::Mesh> mesh, const glm::dmat4& world);
    bool empty() const { return mItems.empty(); }

    /** The nearest surface along a ray (dir need not be unit length) within maxDistance cm; the normal faces the ray. */
    std::optional<SurfacePoint> raycast(const glm::dvec3& origin, const glm::dvec3& dir, double maxDistance) const;
    /** Distance to the nearest surface (up to maxDistance, else maxDistance), negative inside a closed mesh. */
    double signedDistance(const glm::dvec3& p, double maxDistance) const;
    /** World bounds of every mesh. */
    geo::Bounds bounds() const;

private:
    struct Item
    {
        std::shared_ptr<const geo::Mesh> mesh;
        std::shared_ptr<geo::MeshBvh> bvh;
        glm::dmat4 world{1.0}, toLocal{1.0};
        glm::dmat3 normalToWorld{1.0};
        double scale = 1.0;
    };
    std::vector<Item> mItems;
};
} // namespace lila::curve
