#pragma once

#include "core/Export.h"

#include "uv/Chart.h"

#include <glm/glm.hpp>

#include <vector>

namespace lila::uv
{
/**
 * The surface's own flow over a chart: a smooth cross field following its curvature and creases (not its seams), one
 * unit tangent per chart vertex. Empty when the chart is too small to say.
 */
LILA_SCENE_API std::vector<glm::dvec3> chartFlow(const Chart& chart);

/**
 * The turn that puts the chart's flow, as its uvs carry it, onto U and V, and how much the triangles agree on it: 1
 * when the flow runs straight across the island, near 0 when it swirls.
 */
LILA_SCENE_API double flowAngle(const Chart& chart, double* agreement = nullptr);

/**
 * The turn an island gets when nothing else decides it: its edge grid onto the axes when it has one, else its flow when
 * that runs straight across and the turned island's box is nearly as small as its smallest, else that smallest box.
 */
LILA_SCENE_API double uprightAngle(const Chart& chart);
} // namespace lila::uv
