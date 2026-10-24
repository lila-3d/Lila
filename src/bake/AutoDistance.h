#pragma once

#include "core/Export.h"

namespace lila::bake
{
class Backend;
struct BakeInputs;

/**
 * Measures the Auto (0) frontal and rear distances of every group with highs, replacing the 3% fallback buildInputs
 * put in GroupInputs::frontal / rear and setting GroupInputs::measured.
 *
 * Low samples spread by area cast rays like the bake's along the cage directions: outward (from the cage side inward,
 * so the high's front faces are seen as the bake sees them) and inward from the low. Each sample keeps the nearer
 * side. High points add their offset from the closest low point along its cage normal (what bakeLint's cageContainment
 * checks). Each side takes the 99.9th percentile over all samples plus a 10% margin; distances beyond a quarter of the
 * lows' bounds diagonal are treated as other parts and ignored. The backend must be prepared with these inputs.
 */
LILA_SCENE_API void resolveAutoDistances(BakeInputs& in, const Backend& backend);
} // namespace lila::bake
