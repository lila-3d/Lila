#pragma once

#include "core/Export.h"

#include <optional>
#include <string>

namespace lila::ui
{
/**
 * Registers the ghosting actions (anim.toggleGhosts, anim.ghostMode.*, anim.ghostShow.*, anim.refreshGhosts) and the
 * viewport drawer that shows the ghosts. Call once the QApplication exists.
 */
LILA_UI_API void registerGhostingUi();

/** A "Ghosting" submenu of `menu` holding the ghosting actions, checked by their state. */
LILA_UI_API void addGhostingMenu(const std::string& menu);

/** Checked state of a ghosting action, for menus and buttons; nullopt for other ids. */
LILA_UI_API std::optional<bool> ghostActionState(const std::string& id);
} // namespace lila::ui
