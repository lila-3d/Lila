#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Paint a bake low's cage attributes (action "bake.paintCage"): bakeOffset scales the auto cage's push per point,
 * bakeSkew tilts rays toward the face normal. The low shows its values through the weight ramp, the group's cage shell
 * shows over it (and with live preview on, the preview bake updates as you paint). While the tool is active the UV editor
 * paints too: the brush covers the corners under it in uv0, so points split across islands take one value.
 *
 *   LMB drag             paint (one undo step per stroke), in the viewport or the UV editor
 *   Ctrl                 subtract while in Add (add while in Subtract)
 *   Shift                smooth
 *   B held + drag, [ ]   brush radius
 *   Esc                  leave the tool
 */
LILA_UI_API void startBakePaint();

/** bake.paintCage, bake.createCage, bake.cageFromPaint, bake.resetCage, bake.arrange, and the cage view (BakeCageView.h). */
LILA_UI_API void registerBakePaintUi();
} // namespace lila::ui
