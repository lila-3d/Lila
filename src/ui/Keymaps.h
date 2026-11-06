#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

/**
 * Named hotkey sets. The active keymap's bindings are the user's "hotkeys" prefs, which the Hotkey Editor edits live;
 * switching saves them into the keymap they came from and loads the next. Keymaps live in the user prefs folder as
 * keymaps/<name>.json, and a studio folder can provide more the same way.
 */
namespace lila::ui::keymaps
{
/** general.json: the active keymap's name. */
inline constexpr const char* kActivePref = "/hotkeys/keymap";
inline constexpr const char* kDefaultName = "Default";
/** Exported keymaps: {"lilaKeymap": 1, "name": ..., "hotkeys": {...}}. */
inline constexpr const char* kFileSuffix = ".lilakeys";

struct Info
{
    std::string name;
    /** A studio folder provides it; editing it saves your own copy over it. */
    bool studio = false;
};

LILA_UI_API std::vector<Info> list();
LILA_UI_API std::string active();

/** Throws std::invalid_argument for a name that is empty, unknown or can't be a file name, as noted per call. */
LILA_UI_API void use(const std::string& name);
/** A new keymap, a copy of the active one unless empty, made active. */
LILA_UI_API void create(const std::string& name, bool empty = false);
LILA_UI_API void rename(const std::string& from, const std::string& to);
/** Drops every change the keymap makes, leaving Lila's default hotkeys. */
LILA_UI_API void reset(const std::string& name);
/** Deleting the active keymap switches to Default. Default itself can't be deleted, nor a studio keymap. */
LILA_UI_API void remove(const std::string& name);

LILA_UI_API void exportTo(const std::string& name, const std::filesystem::path& file);
/** Adds the keymap in file, named after it unless name is given (made unique), and returns its name. */
LILA_UI_API std::string importFrom(const std::filesystem::path& file, std::string name = {});

/** base, or base followed by the first free number ("Maya 2"). */
LILA_UI_API std::string uniqueName(const std::string& base);

/** keymaps, useKeymap, resetKeymap, newKeymap, renameKeymap, deleteKeymap, exportKeymap, importKeymap. */
LILA_UI_API void registerCommands();
} // namespace lila::ui::keymaps
