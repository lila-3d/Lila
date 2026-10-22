#pragma once

namespace lila::ui
{
/**
 * Paint skin weights on the selected skinned mesh (action "rig.paintWeights"). Opens the tool panel (influence list
 * with lock toggles, mode, value, radius, strength, mirror, flood) and shows the current influence's weights through
 * the weight colour ramp (black 0 through blue, green, yellow, red to white 1; gray by display pref). LMB paints;
 * Ctrl inverts add, Shift smooths; B + drag or [ ] resize; Esc exits. Each stroke is one undo step.
 */
void startWeightPaint();
} // namespace lila::ui
