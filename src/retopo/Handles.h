#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

/** Handles: surface control points that move, turn and scale a region of a retopo mesh with a smooth falloff. */
namespace lila::retopo
{
using geo::Index;

/** What a handle has done since it was placed: moved, turned about its axis and scaled about its rest position. */
struct HandleXform
{
    glm::dvec3 pivot{0.0};
    glm::dvec3 axis{0.0, 1.0, 0.0};
    glm::dvec3 translate{0.0};
    /** Degrees about axis. */
    double rotate = 0.0;
    double scale = 1.0;

    glm::dvec3 apply(const glm::dvec3& p) const;
};

/**
 * Binds a region of a mesh to handles. Each region point blends the handles by inverse squared distance along the mesh's
 * edges, and fades out toward the region's border over `falloff` (a fraction of the region's depth), so the region
 * deforms smoothly and its border stays joined to the rest of the mesh.
 */
class LILA_SCENE_API HandleRig
{
public:
    HandleRig() = default;
    HandleRig(const geo::Mesh& mesh, std::span<const Index> region, std::span<const glm::dvec3> handles, double falloff = 0.5);

    const std::vector<Index>& points() const { return mPoints; }
    size_t handleCount() const { return mHandles; }
    /** New positions for points(), in order. xforms holds one per handle. */
    std::vector<glm::dvec3> deform(std::span<const HandleXform> xforms) const;
    /** Each region point's weight for each handle (points x handles, row major), after the border fade. */
    const std::vector<double>& weights() const { return mWeights; }

private:
    std::vector<Index> mPoints;
    std::vector<glm::dvec3> mRest;
    std::vector<double> mWeights;
    size_t mHandles = 0;
};

/**
 * Points of the faces whose centres lie within radius of the polyline (or touch it), the region a Handles drag sets up.
 */
LILA_SCENE_API std::vector<Index> regionAlong(const geo::Mesh& mesh, std::span<const glm::dvec3> path, double radius);
} // namespace lila::retopo
