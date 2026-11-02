#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Turntables in the UI: the clock that spins them, the actions and hotkeys, the ring each one draws, the on-view
 * controls, Alt+Shift+drag to turn one by hand and Alt+Shift+wheel for its speed, and its Attribute Editor section.
 */
LILA_UI_API void registerTurntableUi();
} // namespace lila::ui
