#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
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
     * box for the command's other flags (see ops/CommandOptions.h) and runs with the remembered values. Left without
     * a `run`, add() supplies one that does exactly that.
     */
    std::string command;
    nlohmann::json call;
    /** Set for toggles: whether it is on now. Menus and icon bars show it as a check. */
    std::function<bool()> checked;
};

namespace actions
{
/** Every registered action, by id, with its owner. */
LILA_UI_API Registry<Action>& registry();

/**
 * Adds an action, or replaces the one with its id. Every action needs a doc: tooltips and the help line show it, and
 * the tests reject actions without one.
 */
LILA_UI_API void add(Action a, std::string owner = kLilaOwner);

/** Adds an action the extension owns, removed when it unloads. 0, with a log message, when another owner has the id. */
LILA_UI_API uint64_t add(ext::Context& ctx, Action a);

/** False when no action has that id. Menus, hotkeys and shelves that name it then do nothing. */
LILA_UI_API bool remove(const std::string& id);

/** Turns an existing action into a plain call of `command` (see Action::command), replacing its run. */
LILA_UI_API void bindCommand(const std::string& id, std::string command, nlohmann::json call = nlohmann::json::object());

/** Runs `command` with `call` plus its remembered option box values, from the UI; errors go to the log. */
LILA_UI_API void runWithOptions(const std::string& command, const nlohmann::json& call);
LILA_UI_API const Action* find(const std::string& id);
LILA_UI_API std::vector<const Action*> all();

/** Gives an existing action an enabled predicate; menus grey it out and run() skips it while it returns false. */
LILA_UI_API void setEnabledWhen(const std::string& id, std::function<bool()> enabled);

/** False only for a registered action whose predicate says so; any other target counts as enabled. */
LILA_UI_API bool isEnabled(const nlohmann::json& target);

/** Predicate: anything is selected. */
LILA_UI_API bool hasSelection();

/** "Label  (Key)\ndoc", the tooltip of any button or menu item bound to an action. The id itself when unknown. */
LILA_UI_API std::string tooltip(const std::string& id);

/**
 * Runs a binding target. Accepts:
 *   "edit.undo"                    a registered action
 *   {"command": "name", "args": {...}}
 *   {"python": "code"}
 *   {"markingMenu": "name"}        (only meaningful from a key + mouse button binding)
 * Returns false when the target is unknown or failed.
 */
LILA_UI_API bool run(const nlohmann::json& target);

/**
 * Whether running `target` replaces what Repeat Last repeats. Selection, view and camera changes, tool switches, windows,
 * file actions, undo/redo and held-key pairs are not, so Repeat Last skips past them to the last edit.
 */
LILA_UI_API bool repeatable(const nlohmann::json& target);

/** Records a target that ran without run(), such as a panel button's command, for Repeat Last and Recent Commands. */
LILA_UI_API void remember(const nlohmann::json& target);

/** What Repeat Last (edit.repeatLast) would run, if anything repeatable has run. */
LILA_UI_API std::optional<nlohmann::json> repeatTarget();

/** The label a menu shows for the action: its own, except Repeat Last names what it will repeat. */
LILA_UI_API std::string displayLabel(const std::string& id);

/** Targets run recently through run(), newest first and without repeats (the hotbox's Recent Commands). */
LILA_UI_API std::vector<nlohmann::json> recent();

/** Human-readable label for a target (menus, cheat sheet). */
LILA_UI_API std::string labelFor(const nlohmann::json& target);

/** Python source that does what target does, for "show code" and copy-to-customize. */
LILA_UI_API std::string pythonFor(const nlohmann::json& target);

LILA_UI_API void registerBuiltins();
} // namespace actions
} // namespace lila::ui
