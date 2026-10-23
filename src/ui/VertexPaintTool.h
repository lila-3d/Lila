#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Vertex color painting on the lead selected mesh's current color set (action "vcolor.paint"), with its floating panel:
 * color sets, primary and secondary colors with picker, eyedropper, swatches and recent colors, blend mode, channel
 * mask, falloff, pressure, symmetry, fills, gradients, adjustments and bakes.
 *
 *   LMB drag             paint (one undo step per stroke)
 *   Ctrl                 paint the secondary color
 *   Shift                blur
 *   B or F held + drag   brush radius (F tapped still frames)
 *   M or Shift+F + drag  strength
 *   [ ]                  radius smaller / larger
 *   I held + click       pick the color under the cursor
 *   G held + drag        gradient from the press to the release (Shift at release: radial)
 *   X                    swap primary and secondary colors
 *   Esc                  leave the tool
 */
LILA_UI_API void startVertexPaint();

/** Actions (vcolor.*) and the Mesh > Vertex Colors items. */
LILA_UI_API void registerVertexPaintUi();
} // namespace lila::ui
