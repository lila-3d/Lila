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
/** The cable tool and cable action (CableTool.cpp); called by registerCurveUi. */
void registerCableTool();
/** The section tool and contour action (SectionTool.cpp); called by registerCurveUi. */
void registerSectionTool();
/** Blend, match, trim, extend, split, fair, curve network and border blend actions and tools (VehicleCurveUi.cpp). */
void registerVehicleCurveUi();
/** The blend curve tool (BlendCurveTool.cpp). */
void registerBlendCurveTool();
/** The trim / extend tool (TrimCurveTool.cpp). */
void registerTrimCurveTool();
/** The sketch constraints tool and sketch actions (SketchTool.cpp); called by registerCurveUi. */
void registerSketchTool();
/** The Sculpt Curves tool and grooming actions (CurveSculptTool.cpp); called by registerCurveUi. */
void registerCurveSculptTool();
/** Text, Bevel Plus, curve booleans, region offsets and the straight skeleton tools (PlanarCurveUi.cpp); called by registerCurveUi. */
void registerPlanarCurveUi();
} // namespace lila::ui
