#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <optional>
#include <span>
#include <vector>

/**
 * Shapes fitted around a point cloud, for collision and proxies. Orientation comes from the points alone, never
 * from a transform. Every fit is measured against every point at the end, so containment is exact.
 */
namespace lila::geo
{
/** A frame (orthonormal, right-handed columns), its centre, and the full size along each column. */
struct OrientedBox
{
    glm::dmat3 axes{1.0};
    glm::dvec3 center{0.0};
    glm::dvec3 size{0.0};
};

/** Round shapes run along axes column 1, so they line up with the Y-up primitives. */
struct RoundShape
{
    glm::dmat3 axes{1.0};
    glm::dvec3 center{0.0};
    double radius = 0.0;
    /** Full length along the axis; for a capsule it includes both caps. */
    double height = 0.0;
};

/**
 * Minimum-volume oriented box: each convex hull face plane is tried with a rotating-calipers rectangle, plus the
 * principal axes, and the best frame is polished by a narrowing rotation sweep. The frame's columns are then
 * permuted and flipped to sit as close to X, Y and Z as they can, so a near-aligned cloud gets near-zero rotation.
 * Empty when there are fewer than two points.
 */
LILA_SCENE_API std::optional<OrientedBox> minimumVolumeBox(std::span<const glm::dvec3> points);

/** The tight box of the points in a given frame. */
LILA_SCENE_API OrientedBox boxInFrame(std::span<const glm::dvec3> points, const glm::dmat3& axes);

/** Smallest enclosing sphere (Welzl), axes left at identity. */
LILA_SCENE_API RoundShape minimumSphere(std::span<const glm::dvec3> points);

/** `axis` picks the box column the shape runs along; -1 tries all three and keeps the smallest. */
LILA_SCENE_API RoundShape fitCylinder(std::span<const glm::dvec3> points, const glm::dmat3& frame, int axis = -1);
LILA_SCENE_API RoundShape fitCapsule(std::span<const glm::dvec3> points, const glm::dmat3& frame, int axis = -1);

/**
 * Convex hull as a mesh of outward triangles. Above `maxPoints` the hull is built from the points that are extreme
 * along evenly spread directions, so it can sit slightly inside the cloud. `offset` pushes every face out.
 */
LILA_SCENE_API Mesh convexHullMesh(std::span<const glm::dvec3> points, int maxPoints = 32, double offset = 0.0);
} // namespace lila::geo
