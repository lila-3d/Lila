#pragma once

#include "core/Export.h"
#include "ui/ViewportTool.h"

#include <vector>

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
/** The Curve Point tool and Insert Point (CurvePointTool.cpp); called by registerCurveUi. */
void registerCurvePointTool();
/** The EZ Curve tool (EzCurveTool.cpp); called by registerCurveUi. */
void registerEzCurveTool();
/** The pipe tool and pipe actions (PipeTool.cpp); called by registerCurveUi. */
void registerPipeTool();
/** The cable tool and cable action (CableTool.cpp); called by registerCurveUi. */
void registerCableTool();
/** Pipe Route tool, Pipe Supports and Carve Channel actions, and their Level Toolkit section. */
void registerPipeLevelTools();
/** The section tool and contour action (SectionTool.cpp); called by registerCurveUi. */
void registerSectionTool();
/** The strap tool and strap preset actions (StrapTool.cpp); called by registerCurveUi. */
void registerStrapTool();
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
/** The Draw Hair tool, groom actions and the groom Attribute Editor section (GroomTool.cpp); called by registerCurveUi. */
void registerGroomUi();
/** Sculpt Curves' brush (groom::Brush) and its Settle While Grabbing, for the Groom palette. */
int curveSculptBrush();
void setCurveSculptBrush(int brush);
bool curveSculptSettleGrab();
void setCurveSculptSettleGrab(bool on);
/** The Sculpt Curves keys that matter now, for the key list. */
std::vector<KeyHint> curveSculptKeys();
/** The Groom palette: shown while Draw Hair or Sculpt Curves is active, on that tool's page; synced when settings change. */
void showGroomPalette(bool drawHair);
void hideGroomPalette();
void syncGroomPalette();
/** Whether the tools list their keys in the corner of the view. */
bool groomKeysShown();
/** Text, Bevel Plus, curve booleans, region offsets and the straight skeleton tools (PlanarCurveUi.cpp); called by registerCurveUi. */
void registerPlanarCurveUi();
} // namespace lila::ui
