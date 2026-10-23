#pragma once

#include "core/Export.h"

namespace lila::ui
{
/** MCP tools that need the Qt UI: window capture and synthetic input. Registers the screenshot command too. */
LILA_UI_API void registerUiTools();
/** The screenshot command (window, viewport or panel to PNG). Idempotent; fails with a clear error without the window. */
LILA_UI_API void registerScreenshotCommand();
} // namespace lila::ui
