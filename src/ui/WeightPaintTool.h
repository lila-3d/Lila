#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Paint skin weights on the selected skinned mesh (action "rig.paintWeights"), or the weight map of one of its deformers
 * or a painted layer of a field they read ("deform.paintWeights"). Opens the tool panel (influences with lock toggles,
 * then deformers and field layers; mode, value, radius, strength, mirror, flood) and shows the current influence's
 * weights through the weight colour ramp (black 0 through blue, green, yellow, red to white 1; gray by display pref),
 * deformer and field maps always in gray. LMB paints;
 * Ctrl inverts add, Shift smooths; B + drag or [ ] resize; Esc exits. Each stroke is one undo step.
 */
LILA_UI_API void startWeightPaint();
} // namespace lila::ui
