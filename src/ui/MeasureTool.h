#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Measurement UI: draws and picks measure nodes in every viewport, the interactive distance/angle/radius tools, the
 * transient measureSelection HUD and the "measure.*" actions. Call once after the built-in actions are registered.
 */
LILA_UI_API void registerMeasureUi();
} // namespace lila::ui
