#pragma once

#include "core/Export.h"

class QDockWidget;
class QWidget;

/**
 * Find in a panel: a small bar over the panel's top-right corner. Every control whose label, name, tooltip name, menu
 * entry or list row matches is outlined; nothing is hidden. A match inside a collapsed section or another tab lights
 * that section header or tab. Enter and Shift+Enter step through the matches, opening and scrolling to each.
 */
namespace lila::ui::panelFilter
{
/** Opens the find bar over the dock's panel, or focuses it when it is open. */
LILA_UI_API void open(QDockWidget* dock);

/** The dock holding w, or null. */
LILA_UI_API QDockWidget* dockOf(QWidget* w);
} // namespace lila::ui::panelFilter
