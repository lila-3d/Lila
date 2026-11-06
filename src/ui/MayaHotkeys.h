#pragma once

#include "core/Export.h"

#include "ui/HotkeyBindings.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace lila::ui::mayaHotkeys
{
/** One `hotkey` statement from a Maya hotkey set (.mhk) or userHotkeys.mel. */
struct MayaHotkey
{
    std::string key; // -keyShortcut as written: "w", "W", "Page_Up", "F8"
    bool ctrl = false, alt = false, shift = false, command = false;
    bool hasPress = false, hasRelease = false;
    std::string press, release; // -name / -releaseName; empty with hasPress unbinds
    std::string editor;         // -ctxClient, empty for the whole app
};

/** A nameCommand or runTimeCommand definition. */
struct MayaCommand
{
    std::string command;
    std::string language = "mel";
};

struct Parsed
{
    std::vector<MayaHotkey> hotkeys;
    std::map<std::string, MayaCommand> nameCommands;
    std::map<std::string, MayaCommand> runTimeCommands;
};

/** Reads hotkey, nameCommand and runTimeCommand statements, adding to `into`. */
LILA_UI_API void parse(const std::string& mel, Parsed& into);

/** A Maya binding the import leaves out, and why. */
struct Skipped
{
    std::string key;     // "Ctrl+Shift+W"
    std::string command; // the Maya command, or its MEL/Python source when it is custom
    std::string reason;
};

struct Plan
{
    std::vector<hotkeys::Assignment> assignments;
    std::vector<Skipped> skipped;
};

/** The chord for a Maya -keyShortcut and modifiers; empty for a key Lila can't bind. */
LILA_UI_API std::optional<QKeyCombination> chordFor(const MayaHotkey& h);

/** The Lila action for a Maya runtime command, or "". */
LILA_UI_API std::string actionFor(const std::string& runtimeCommand);

/** Maps every hotkey to Lila contexts and actions, without touching prefs. */
LILA_UI_API Plan plan(const Parsed& parsed);

struct Result
{
    int imported = 0;
    std::vector<Skipped> skipped;
    /** The keymap the keys went into. */
    std::string keymap;
};

/**
 * Imports a Maya hotkey set into a new keymap named keymap (made unique) on Lila's defaults, and switches to it. Reads userNamedCommands.mel and userRunTimeCommands.mel from the
 * file's folder and the folder above it, so custom commands that wrap built-in ones still map.
 */
LILA_UI_API Result importFile(const std::filesystem::path& file, const std::string& keymap = "Maya");

/** The importMayaHotkeys command. */
LILA_UI_API void registerCommands();

/** The newest Maya prefs/hotkeys folder under Documents/maya, or "". */
LILA_UI_API std::filesystem::path defaultFolder();
} // namespace lila::ui::mayaHotkeys
