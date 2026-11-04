#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <vector>

/** Splitting a flat region along a drawn cut, for cutting pictures into pieces. */
namespace lila::curve
{
using RegionLoop = std::vector<glm::dvec2>;

/** Even-odd inside test over every loop. */
LILA_SCENE_API bool regionContains(const std::vector<RegionLoop>& region, glm::dvec2 p);

/** Distance from p to the nearest loop edge; 0 inside. */
LILA_SCENE_API double regionDistance(const std::vector<RegionLoop>& region, glm::dvec2 p);

/**
 * A two-point cut across a joint as cutRegion makes it: the limb lies on the side away from keep, and each end inside the
 * region runs on to its edge, turned 30 degrees out from straight across the limb so the run-ons fan out around it
 * instead of carrying on into the body. Longer cuts are lassos and come back as they are.
 */
LILA_SCENE_API RegionLoop runOnCut(const std::vector<RegionLoop>& region, const RegionLoop& cut, glm::dvec2 keep, double overshoot);

struct RegionCut
{
    /** Counter-clockwise loops. Both empty when the cut leaves the region whole. */
    std::vector<RegionLoop> limb;
    std::vector<RegionLoop> rest;
};

/**
 * Parts a region (simple loops, filled even-odd). Two points: along runOnCut(cut), the pieces gap cm apart. Three or
 * more: a lasso, closed back to its first point, and the limb is what lies inside it.
 */
LILA_SCENE_API RegionCut cutRegion(const std::vector<RegionLoop>& region, const RegionLoop& cut, glm::dvec2 keep, double gap = 1e-3);
} // namespace lila::curve
