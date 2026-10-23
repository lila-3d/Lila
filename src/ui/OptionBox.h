#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <cstdint>
#include <functional>
#include <string>

class QAction;
class QMenu;
class QToolButton;
class QWidget;

/**
 * Maya's option boxes. A menu row whose action is a plain command call with flags left to set (Action::command)
 * gets a small square at its right: clicking the square, or Shift+clicking the row, opens the command's options
 * dialog instead of running it. The dialog is generated from the flags (ops/CommandOptions.h), remembers what
 * is set in it, and the menu item, hotkeys and shelf buttons then run with those values.
 */
namespace lila::ui::optionBox
{
/** The action has an option box. */
LILA_UI_API bool available(const std::string& actionId);

/** Opens, or raises, the action's options dialog. */
LILA_UI_API void open(const std::string& actionId);

/** Custom option boxes, by action id. */
LILA_UI_API Registry<std::function<void()>>& customRegistry();

/** Gives an action whose settings are not command flags an option box that calls open instead of the generated dialog. */
LILA_UI_API void addCustom(const std::string& actionId, std::function<void()> open, std::string owner = kLilaOwner);
/** Owned by the extension and removed when it unloads. */
LILA_UI_API uint64_t addCustom(ext::Context& ctx, const std::string& actionId, std::function<void()> open);
LILA_UI_API bool removeCustom(const std::string& actionId);

/**
 * The action id whose options a commit of row a in menu at global should open instead of running it: the cursor is
 * on the row's square, or Shift is held. Empty otherwise. For commits that are not a click on the menu, such as the
 * hotbox key coming up.
 */
LILA_UI_API std::string optionsAt(const QMenu* menu, QAction* a, int globalX, bool shift);

/** Adds the squares to the menu's rows and its submenus' rows, and the clicks that open them. Idempotent. */
LILA_UI_API void decorateMenu(QMenu* menu);

/** The button followed by its option box square, for panels; just the button when the action has no options. */
LILA_UI_API QWidget* withOptionBox(QToolButton* button, const std::string& actionId, QWidget* parent);

/** Makes the rig menu's command actions plain command calls. Call once every action is registered. */
LILA_UI_API void install();
} // namespace lila::ui::optionBox
