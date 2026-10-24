#pragma once

#include "core/Export.h"

#include "brush/Brush.h"
#include "geo/Symmetry.h"

#include <span>
#include <utility>
#include <vector>

/**
 * Brush symmetry: mirror copies of a dab across a plane, radial copies about an object axis, and the point-level mirror
 * through a geo::SymmetryMap that topology-matched brushes (vertex colors, weights) use instead of mirroring dabs.
 */
namespace lila::brush
{
/** The dab reflected across the plane: positions as points, normal and directions as vectors. */
LILA_SCENE_API Dab mirrorDab(const Dab& d, const geo::SymmetryPlane& plane);

/** The dab turned by angle (radians) about an axis through the origin. */
LILA_SCENE_API Dab rotateDab(const Dab& d, const glm::dvec3& axis, double angle);

/** The dab followed by its radial copies about the object axis, evenly spaced; just the dab when radial is off. */
LILA_SCENE_API std::vector<Dab> radialDabs(const Dab& d, const Radial& radial);

/**
 * Every symmetric copy: the radial copies and, with a mirror plane, each one's reflection. The dab itself comes first;
 * Dab::copy numbers the rest from 1.
 */
LILA_SCENE_API std::vector<Dab> symmetricDabs(const Dab& d, const geo::SymmetryPlane* mirror, const Radial& radial);

/**
 * Adds each hit point's mirror partner (inside mask, when one is given) at the same weight, then keeps one entry per
 * point at its strongest weight, sorted by point, so a point reached from both sides gets one symmetric application.
 */
LILA_SCENE_API void mirrorHits(std::vector<std::pair<geo::Index, float>>& hits, const geo::SymmetryMap& map,
                               std::span<const uint8_t> mask = {});

/** One entry per point at its strongest weight, sorted by point. */
LILA_SCENE_API void mergeHits(std::vector<std::pair<geo::Index, float>>& hits);
} // namespace lila::brush
