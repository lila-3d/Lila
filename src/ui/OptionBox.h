#pragma once

#include <functional>
#include <string>

class QAction;
class QMenu;
class QToolButton;
class QWidget;

/**
 * Maya's option boxes. A menu row whose action is a plain command call with flags left to set (Action::command)
 * gets a small square at its right: clicking the square, or Shift+clicking the row, opens the command's options
 * dialog instead of running it. The dialog is generated from the flags (cmds/CommandOptions.h), remembers what
 * is set in it, and the menu item, hotkeys and shelf buttons then run with those values.
 */
namespace lila::ui::optionBox
{
/** The action has an option box. */
bool available(const std::string& actionId);

/** Opens, or raises, the action's options dialog. */
void open(const std::string& actionId);

/** Gives an action whose settings are not command flags an option box that calls open instead of the generated dialog. */
void addCustom(const std::string& actionId, std::function<void()> open);

/**
 * The action id whose options a commit of row a in menu at global should open instead of running it: the cursor is
 * on the row's square, or Shift is held. Empty otherwise. For commits that are not a click on the menu, such as the
 * hotbox key coming up.
 */
std::string optionsAt(const QMenu* menu, QAction* a, int globalX, bool shift);

/** Adds the squares to the menu's rows and its submenus' rows, and the clicks that open them. Idempotent. */
void decorateMenu(QMenu* menu);

/** The button followed by its option box square, for panels; just the button when the action has no options. */
QWidget* withOptionBox(QToolButton* button, const std::string& actionId, QWidget* parent);

/** Makes the rig menu's command actions plain command calls. Call once every action is registered. */
void install();
} // namespace lila::ui::optionBox
