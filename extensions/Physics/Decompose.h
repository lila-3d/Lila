#pragma once

#include <glm/glm.hpp>

#include <vector>

/**
 * Approximate convex decomposition: a concave mesh (an arch, a chair, a shelf unit) split into a few convex hulls that
 * together follow its shape, so a dynamic body collides as the thing it is instead of as its one wrapping hull.
 *
 * The mesh is voxelized, then the most concave part is cut along whichever axis-aligned plane leaves the least hull
 * volume, again and again, until every part is nearly convex or there are maxParts. Deterministic.
 */
namespace lila::physics
{
struct DecomposeOptions
{
    /** Voxels along the longest side. */
    int resolution = 40;
    /** A part stops being cut once its hull is at most this fraction bigger than the solid it wraps. */
    double concavity = 0.08;
    int maxParts = 16;
};

/** Triangles as three points each, any units. Returns the points of each convex part (at least one part for any solid input). */
std::vector<std::vector<glm::dvec3>> decompose(const std::vector<glm::dvec3>& triangles, const DecomposeOptions& options = {});
} // namespace lila::physics
