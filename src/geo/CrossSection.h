#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <optional>
#include <span>
#include <vector>

namespace lila::geo
{
/** A mesh placed in the world for slicing. */
struct SectionMesh
{
    const Mesh* mesh = nullptr;
    glm::dmat4 world{1.0};
};

/** One closed loop of a planar cut, in world space. */
struct SectionContour
{
    std::vector<glm::dvec3> points;
    double area = 0.0; // unsigned
    glm::dvec3 centroid{0.0};
    /** The cut ran off a mesh boundary or between meshes and was closed across the gap. */
    bool bridged = false;
};

/**
 * Cuts the meshes with the plane through `origin` perpendicular to `normal` and chains the cut into closed loops.
 * N-gons are cut along their own plane; open and non-manifold meshes give open chains, which are joined to the nearest
 * other open end when that is closer than closing the chain on itself. Points exactly on the plane count as in front.
 */
LILA_SCENE_API std::vector<SectionContour> crossSections(std::span<const SectionMesh> meshes, const glm::dvec3& origin,
                                                          const glm::dvec3& normal);

struct SectionPick
{
    glm::dvec3 center{0.0}; // area centroid of the chosen loop, on the cutting plane
    double area = 0.0;
    /** The loop contains the point; otherwise it is the loop nearest to it. */
    bool inside = false;
};

/**
 * Centre of the cross-section around `point`: the meshes are cut by the plane through it perpendicular to `direction`,
 * and the smallest loop containing the point wins (the nearest loop when none does). Empty when the plane misses.
 */
LILA_SCENE_API std::optional<SectionPick> sectionCenter(std::span<const SectionMesh> meshes, const glm::dvec3& point,
                                                        const glm::dvec3& direction);
LILA_SCENE_API std::optional<SectionPick> sectionCenter(std::span<const SectionContour> contours, const glm::dvec3& point,
                                                        const glm::dvec3& direction);
} // namespace lila::geo
