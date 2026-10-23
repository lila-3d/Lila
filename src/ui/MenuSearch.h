#pragma once

#include "core/Export.h"

class QMenuBar;
class QWidget;

/**
 * Searching menus by typing. In any open QMenu, typing filters its rows (and submenus with a match inside) as you
 * type; Backspace edits, Esc clears, Enter runs the highlighted row. The search box finds any item in the main menu
 * bar by name or by its menu path.
 */
namespace lila::ui::menuSearch
{
/** Type-to-filter for every QMenu in the app. */
LILA_UI_API void install();

/** Opens the search box over the window, listing every item reachable from the menu bar. */
LILA_UI_API void openBox(QMenuBar* bar, QWidget* over);
} // namespace lila::ui::menuSearch
