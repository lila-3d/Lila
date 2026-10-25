#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Actions for fields, deformer weights, membership, correction and timing, and the deformer display: nonlinear handles
 * drawn bent through their bounds (with falloff), softMod falloff spheres while selected, field shapes (draw override
 * "lila.field"), all pickable; a selected field's value shown on the meshes it drives; and the displacement heatmap
 * (command displacementHeatmap, action "deform.heatmap").
 */
LILA_UI_API void registerDeformFoundationUi();
} // namespace lila::ui
