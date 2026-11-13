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

/** A point the chart's flow turns round: where it can't lie flat without a cut or a cone. */
struct FlowCone
{
    Index vertex = kInvalid;
    /** +1 where the surface closes like a cap (a fingertip), -1 where it saddles. */
    int index = 0;
    /** Angle defect over the two rings round it, radians: how far their corners fall short of (or over) flat. */
    double curvature = 0.0;
};

/** The singular points of the chart's flow off its border, most curved first. */
LILA_SCENE_API std::vector<FlowCone> flowCones(const Chart& chart);

/**
 * Chart vertices for cone points from the flow: the most curved count of its singular points (every one bending at
 * least minCurvature radians when count is 0), kept apart by a few edges.
 */
LILA_SCENE_API std::vector<Index> flowConeVertices(const Chart& chart, int count = 0, double minCurvature = 0.05);
} // namespace lila::uv
