#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Curves in the viewport and menus: the "lila.curve" draw override (the curve, and while editing its hull, handles, points
 * and curvature comb), the draw tools (Bezier pen, CV, EP, pencil, Catmull-Rom, kappa) and every curve action, placed in the
 * Curves menu and the modeling toolkit.
 */
LILA_UI_API void registerCurveUi();
/** The draw tools (CurveTools.cpp); called by registerCurveUi. */
void registerCurveTools();
/** The pipe tool and pipe actions (PipeTool.cpp); called by registerCurveUi. */
void registerPipeTool();
} // namespace lila::ui
