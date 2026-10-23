#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Smart Join's option box: the smartJoin flags, live. Join runs on the two selected meshes; each change after that
 * undoes the join and runs it again on the same inputs while it is still the latest undo step, and the confidence
 * readout follows. Values are the option box's remembered settings, so the menu item uses them too.
 */
LILA_UI_API void registerSmartJoinUi();
} // namespace lila::ui
