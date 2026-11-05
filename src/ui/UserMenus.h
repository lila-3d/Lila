#pragma once

#include "core/Export.h"
#include "ui/Owner.h"

#include <nlohmann/json.hpp>

#include <string>

/**
 * Menus the user builds, stored in the "userMenus" prefs file under /menus. Each one goes on the menu bar, or into any
 * registered menu as a submenu or a titled section, and is registered like Lila's own menus: it tears off, shows in
 * Shift+T and the menu search, and its rows are actions that hotkeys and shelves can bind.
 *
 *   {"title": "Kit", "parent": "",            menu bar; "Mesh" or "Mesh/Symmetry" places it in that menu
 *    "as": "submenu",                         or "section": its rows inline under a heading, in the parent
 *    "before": "Help",                        menu bar only: the menu it sits in front of
 *    "after": "Mesh/mesh.bevel",              in a parent: the item id it follows; empty appends
 *    "sets": ["Modeling"],                    menu bar only: the menu sets that show it; empty shows it in all
 *    "items": [{"action": "mesh.bevel"},      a target as hotkeys take it: an action id, {"python": ...}, {"command": ...}
 *              {"action": {"python": "..."}, "label": "Clean Up", "id": "cleanUp", "doc": "..."},
 *              {"separator": true}, {"section": "Title"}, {"submenu": "More", "items": [...]}]}
 */
namespace lila::ui::userMenus
{
/** The registry owner of everything user menus register. */
inline constexpr const char* kOwner = kUserMenusOwner;
inline constexpr const char* kPrefFile = "userMenus";

/** The stored menus, in order. */
LILA_UI_API nlohmann::json all();
/** Replaces the stored menus and re-registers them. */
LILA_UI_API void setAll(nlohmann::json menus);

/** Why the menu cannot be stored next to `others` (the rest of the list), or empty when it can. */
LILA_UI_API std::string problem(const nlohmann::json& menu, const nlohmann::json& others = nlohmann::json::array());

/** The action id a row registers: "user.<menu>.<row id>". Empty for separators, sections and submenus. */
LILA_UI_API std::string rowAction(const nlohmann::json& menu, const nlohmann::json& row);

/** The menu path its rows are registered under: its own id on the menu bar, "<parent>/<title>" as a submenu. */
LILA_UI_API std::string rowsPath(const nlohmann::json& menu);

/** Registers the stored menus now and whenever the prefs file changes; adds the userMenu commands. */
LILA_UI_API void install();
/** Re-registers from the stored menus. */
LILA_UI_API void apply();
} // namespace lila::ui::userMenus
