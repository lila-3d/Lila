#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Auto bevel UI: actions (modifier.autoBevel, select.foldEdges, bevel.editEdges, bevel.applyEdges, bevel.cancelEdges,
 * bevel.measureThin, bevel.clearOverrides, bevel.showEdges), the attribute editor section with the Edit Edges buttons,
 * and the viewport overlay of a selected modifier's edges.
 */
LILA_UI_API void registerAutoBevelUi();
} // namespace lila::ui
