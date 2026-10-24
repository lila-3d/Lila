#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Target Weld: press on a vertex and release on another to merge the first into the second, which keeps its place. In
 * edge mode it welds an edge onto another, end to nearest end. Shift on release meets halfway instead. Each weld is
 * one polyTargetWeld call and follows the mesh's symmetry. Esc leaves the tool.
 */
LILA_UI_API void registerTargetWeldActions();
} // namespace lila::ui
