#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace lila::ui
{
/**
 * Named UI actions. Menus, hotkeys, marking menus and shelves all trigger these, so anything that appears in one
 * can be bound in the others.
 */
struct Action
{
    std::string id;       // "edit.undo"
    std::string label;    // "Undo"
    std::string category; // "Edit"
    std::function<void()> run;
    std::function<bool()> enabled;
    std::string doc;
    std::string python; // equivalent source, shown to users who want to copy and adapt it
    /**
     * Set when the action is one plain command call: `command` with the flags in `call`. Such an action gets an option
     * box for the command's other flags (see cmds/CommandOptions.h) and runs with the remembered values. Left without
     * a `run`, add() supplies one that does exactly that.
     */
    std::string command;
    nlohmann::json call;
};

namespace actions
{
/** Every action needs a doc: tooltips and the help line show it, and the tests reject actions without one. */
void add(Action a);

/** Turns an existing action into a plain call of `command` (see Action::command), replacing its run. */
void bindCommand(const std::string& id, std::string command, nlohmann::json call = nlohmann::json::object());

/** Runs `command` with `call` plus its remembered option box values, from the UI; errors go to the log. */
void runWithOptions(const std::string& command, const nlohmann::json& call);
const Action* find(const std::string& id);
std::vector<const Action*> all();

/** Gives an existing action an enabled predicate; menus grey it out and run() skips it while it returns false. */
void setEnabledWhen(const std::string& id, std::function<bool()> enabled);

/** False only for a registered action whose predicate says so; any other target counts as enabled. */
bool isEnabled(const nlohmann::json& target);

/** Predicate: anything is selected. */
bool hasSelection();

/** "Label  (Key)\ndoc", the tooltip of any button or menu item bound to an action. The id itself when unknown. */
std::string tooltip(const std::string& id);

/**
 * Runs a binding target. Accepts:
 *   "edit.undo"                    a registered action
 *   {"command": "name", "args": {...}}
 *   {"python": "code"}
 *   {"markingMenu": "name"}        (only meaningful from a key + mouse button binding)
 * Returns false when the target is unknown or failed.
 */
bool run(const nlohmann::json& target);

/** Targets run recently through run(), newest first and without repeats (the hotbox's Recent Commands). */
std::vector<nlohmann::json> recent();

/** Human-readable label for a target (menus, cheat sheet). */
std::string labelFor(const nlohmann::json& target);

/** Python source that does what target does, for "show code" and copy-to-customize. */
std::string pythonFor(const nlohmann::json& target);

void registerBuiltins();
} // namespace actions
} // namespace lila::ui
