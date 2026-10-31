#pragma once

#include "core/Export.h"

#include <string>

class QAction;

namespace lila::ui::menus
{
struct Menu;
}

/**
 * Maya's menu sets: the menu bar shows a fixed core plus the menus of the active set, and the set follows the docked
 * shelf's tab. The hotbox and menu search still reach every menu. Off when the general pref /menuSets/enabled is false.
 */
namespace lila::ui::menuSets
{
constexpr const char* kDefault = "Modeling";

/**
 * The set a shelf selects: the general pref /menuSets/shelves ({"My Shelf": "Animation"}) first, then Lila's own
 * shelves, then a set of the same name. Empty when the shelf names no set, so the menu bar keeps the one it has.
 */
LILA_UI_API std::string forShelf(const std::string& shelf);

LILA_UI_API bool enabled();

/** Whether the menu bar shows the menu while set is active. */
LILA_UI_API bool shows(const menus::Menu& menu, const std::string& set);

/** Marks a menu bar row hidden by the active set, so the hotbox and menu search still list it. */
LILA_UI_API void setHiddenBySet(QAction* row, bool hidden);
/** A menu bar row that is visible or only hidden by the active set. */
LILA_UI_API bool reachable(const QAction* row);
} // namespace lila::ui::menuSets
