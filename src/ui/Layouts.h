#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lila::ui::layouts
{
/**
 * Named UI layouts (Maya's workspaces). Saved ones are prefs files "layouts/<Name>": user level in the prefs dir,
 * workspace level in <workspace>/.lila/layouts. A workspace layout shadows a user one of the same name, which shadows a
 * built-in preset. "layouts/main" is the window's own autosave and is never listed.
 *
 * A saved layout is {"version", "state" (base64 QMainWindow::saveState), "docks": {objectName: {title, open, floating,
 * collapsed, geometry?}}, "collapsed": [...], "viewport": {quad, camera}, "shelf": {visible, current}}. A built-in has
 * "preset": {open: [...], raise: [...], size: {dock: {width|height: fraction of window}}} in place of "state".
 */
enum class Origin
{
    BuiltIn,
    User,
    Workspace,
};

struct Entry
{
    std::string name;
    Origin origin = Origin::User;
    bool shadowsBuiltIn = false;
};

const char* originName(Origin o);

/** Built-ins in their preset order, then every other name sorted case-insensitively; the topmost layer wins. */
std::vector<Entry> merge(const std::vector<std::string>& builtIns, const std::vector<std::string>& user,
                         const std::vector<std::string>& workspace);

/** Every layout: built-ins, the user prefs dir and the open workspace. */
std::vector<Entry> list();
std::optional<Entry> find(const std::string& name);

/** Names of layout files in a layouts dir, without extension, "main" excluded. */
std::vector<std::string> namesIn(const std::filesystem::path& dir);

/** Empty when valid, otherwise why not: it becomes a file name, and "main" is taken. */
std::string invalidReason(const std::string& name);

std::vector<std::string> builtInNames();
std::optional<nlohmann::json> builtIn(const std::string& name);

/** The effective layout of that name, or nullopt. */
std::optional<nlohmann::json> read(const std::string& name);

/** Writes and flushes, so list() sees it at once. Throws CommandError on a bad name or a workspace scope with none open. */
void write(const std::string& name, const nlohmann::json& layout, Origin scope);

/** Deletes the topmost saved file of that name. False when there is none (built-ins can't be deleted). */
bool remove(const std::string& name);

/** Renames the topmost saved layout, keeping its scope. Throws CommandError. */
void rename(const std::string& from, const std::string& to);

/** The last applied or saved layout, shown by the switcher. */
std::string current();
void setCurrent(const std::string& name);

/** The window installs these; without them (batch mode) applyLayout and saveLayout fail with a clear error. */
struct GuiHooks
{
    std::function<void(const std::string& name, const nlohmann::json& layout)> apply;
    std::function<nlohmann::json()> capture;
};
void setGuiHooks(GuiHooks hooks);

/** applyLayout, saveLayout, listLayouts, deleteLayout, renameLayout. Not undoable: they only touch the UI. */
void registerCommands();
} // namespace lila::ui::layouts
