#pragma once

#include "core/Export.h"

#include <string>

class QAction;

namespace lila::ui::menus
{
struct Menu;
}

/**
 * Menu sets: the menu bar shows a fixed core plus the menus of the active set (see MenuContext.h for which set that is).
 * The hotbox and menu search still reach every menu. The general pref /ui/allMenus shows every menu all the time.
 */
namespace lila::ui::menuSets
{
inline constexpr const char* kAllMenusPref = "/ui/allMenus";

LILA_UI_API bool enabled();

/** Whether the menu bar shows the menu while set is active. */
LILA_UI_API bool shows(const menus::Menu& menu, const std::string& set);

/** Marks a menu bar row hidden by the active set, so the hotbox and menu search still list it. */
LILA_UI_API void setHiddenBySet(QAction* row, bool hidden);
/** A menu bar row that is visible or only hidden by the active set. */
LILA_UI_API bool reachable(const QAction* row);
} // namespace lila::ui::menuSets
