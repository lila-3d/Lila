#pragma once

#include "core/Export.h"

/**
 * Esc while dragging a panel cancels the dock it would drop into: the indicator goes away, the panel keeps following
 * the cursor and stays floating where it is released.
 */
namespace lila::ui::dockDragCancel
{
LILA_UI_API void install();
} // namespace lila::ui::dockDragCancel
