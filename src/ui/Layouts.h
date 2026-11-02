#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::ui::layouts
{
/**
 * Named UI layouts (Maya's workspaces). Saved ones are prefs files "layouts/<Name>": user level in the prefs dir,
 * workspace level in <workspace>/.lila/layouts. A workspace layout shadows a user one of the same name, which shadows a
 * built-in preset. "layouts/main" is the window's own autosave and is never listed.
 *
 * A saved layout is {"version", "state" (base64 QMainWindow::saveState), "docks": {objectName: {title, open, floating,
 * collapsed, geometry?}}, "collapsed": [...], "viewport": {quad, camera}, "shelf": {visible, current}}. A built-in has
 * "preset": {open: [...], raise: [...], size: {dock: {width|height: fraction of window}}, bottom: [...], columns: [[...]]}
 * in place of "state"; each "bottom" dock leaves its tab group for a row of its own along the bottom of its dock area,
 * and "columns" rebuild those docks (with their tab groups) as columns at the area's outer edge, left to right.
 */
enum class Origin
{
    BuiltIn,
    User,
    Workspace,
    Extension, // contributed in code or from an extension's resources/layouts; read-only like a built-in
};

struct Entry
{
    std::string name;
    Origin origin = Origin::User;
    bool shadowsBuiltIn = false;
};

LILA_SCENE_API const char* originName(Origin o);

/** Built-ins in their preset order, then every other name sorted case-insensitively; the topmost layer wins. */
LILA_SCENE_API std::vector<Entry> merge(const std::vector<std::string>& builtIns, const std::vector<std::string>& user,
                         const std::vector<std::string>& workspace);

/** Every layout: built-ins, the user prefs dir and the open workspace. */
LILA_SCENE_API std::vector<Entry> list();
LILA_SCENE_API std::optional<Entry> find(const std::string& name);

/** Names of layout files in a layouts dir, without extension, "main" excluded. */
LILA_SCENE_API std::vector<std::string> namesIn(const std::filesystem::path& dir);

/** Empty when valid, otherwise why not: it becomes a file name, and "main" is taken. */
LILA_SCENE_API std::string invalidReason(const std::string& name);

LILA_SCENE_API std::vector<std::string> builtInNames();
/** A built-in or contributed layout. */
LILA_SCENE_API std::optional<nlohmann::json> builtIn(const std::string& name);

/** Layouts contributed by extensions (and Python), by name; listed after the built-ins. */
LILA_SCENE_API Registry<nlohmann::json>& contributed();
LILA_SCENE_API void add(const std::string& name, nlohmann::json layout, std::string owner);
/** Owned by the extension and removed when it unloads. */
LILA_SCENE_API uint64_t add(ext::Context& ctx, const std::string& name, nlohmann::json layout);

/** The effective layout of that name, or nullopt. */
LILA_SCENE_API std::optional<nlohmann::json> read(const std::string& name);

/** Writes and flushes, so list() sees it at once. Throws CommandError on a bad name or a workspace scope with none open. */
LILA_SCENE_API void write(const std::string& name, const nlohmann::json& layout, Origin scope);

/** Deletes the topmost saved file of that name. False when there is none (built-ins can't be deleted). */
LILA_SCENE_API bool remove(const std::string& name);

/** Renames the topmost saved layout, keeping its scope. Throws CommandError. */
LILA_SCENE_API void rename(const std::string& from, const std::string& to);

/** The last applied or saved layout, shown by the switcher. */
LILA_SCENE_API std::string current();
LILA_SCENE_API void setCurrent(const std::string& name);

/** The window installs these; without them (batch mode) applyLayout and saveLayout fail with a clear error. */
struct GuiHooks
{
    std::function<void(const std::string& name, const nlohmann::json& layout)> apply;
    std::function<nlohmann::json()> capture;
};
LILA_SCENE_API void setGuiHooks(GuiHooks hooks);

/** applyLayout, saveLayout, listLayouts, deleteLayout, renameLayout. Not undoable: they only touch the UI. */
LILA_SCENE_API void registerCommands();
} // namespace lila::ui::layouts
