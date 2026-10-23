#pragma once

#include "core/Export.h"

#include "uv/Chart.h"

#include <span>
#include <vector>

namespace lila::uv
{
/**
 * Straightens the chains the selected mesh edges form in the chart (edge loops, borders, seam sides):
 * each is laid on its best-fit line with its uv arc length kept, then the rest of the island is
 * re-solved around it (LSCM held by the chains and pins, refined by SLIM when it stays fold-free).
 * edgeSelected is per mesh edge. Returns how many chains were straightened.
 */
LILA_SCENE_API size_t straightenEdges(Chart& chart, const geo::Mesh& mesh, std::span<const uint8_t> edgeSelected, int iterations = 12);

enum class AlignAxis : uint8_t
{
    Nearest, // whichever of U and V is closer: orient by the edge
    U,
    V,
};

/**
 * Rotates the chart about its uv centroid so the overall direction of its selected edges lies along an
 * axis. False (chart untouched) when none of its edges are selected.
 */
LILA_SCENE_API bool alignToEdges(Chart& chart, const geo::Mesh& mesh, std::span<const uint8_t> edgeSelected, AlignAxis axis = AlignAxis::Nearest);
} // namespace lila::uv
