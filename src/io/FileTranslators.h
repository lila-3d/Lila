#pragma once

#include "cmds/Command.h"
#include "core/Export.h"
#include "core/Registry.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ext
{
class Context;
}

/**
 * File translators (Maya's MPxFileTranslator): what File > Open, Import and Export offer and which code reads or writes
 * each extension. Built-ins (.lila/.lilaa, FBX, OBJ, Maya) and extensions register here alike. Main thread only.
 */
namespace lila::io::translators
{
using json = nlohmann::json;
namespace fs = std::filesystem;

enum class Op : uint8_t
{
    Open,   // replaces the scene
    Import, // adds the file's contents to the scene
    Export, // writes the selection
};

LILA_SCENE_API const char* opName(Op op);

/** One of a translator's operations. Set command or run; neither means the translator does not support it. */
struct Operation
{
    /** Run with the path as its first argument plus flags and the caller's options, so it is undoable and scriptable. */
    std::string command;
    /** Fixed flags every call gets ({"profile": "ue5"}). The caller's options do not override them. */
    json flags = json::object();
    /** Instead of command: does the work itself and returns what the call returns. */
    std::function<json(const fs::path& path, const json& options)> run;
    /** Opens this operation's options UI. Unset, a UI shows the command's generated option box (cmds/CommandOptions.h). */
    std::function<void()> optionBox;

    bool supported() const { return !command.empty() || static_cast<bool>(run); }
};

struct FileTranslator
{
    std::string id;    // "fbx"
    std::string label; // "FBX", what file dialogs list
    std::string doc;
    /** Lowercase with the dot: ".fbx". */
    std::vector<std::string> extensions;
    Operation open;
    Operation import;
    Operation exportSelection;
    /** Higher wins when two translators claim an extension for the same operation. */
    int priority = 0;

    const Operation& operation(Op op) const;
    bool handles(const fs::path& path) const;
};

LILA_SCENE_API Registry<FileTranslator>& registry();

/** Throws std::invalid_argument when the id is taken. */
LILA_SCENE_API const FileTranslator& add(FileTranslator t);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the id is taken. */
LILA_SCENE_API const FileTranslator* add(ext::Context& ctx, FileTranslator t);
LILA_SCENE_API bool remove(std::string_view id);
LILA_SCENE_API const FileTranslator* find(std::string_view id);

/** The translator that handles path for op (highest priority, then id), or null. */
LILA_SCENE_API const FileTranslator* forPath(Op op, const fs::path& path);
/** Translators supporting op, highest priority first. */
LILA_SCENE_API std::vector<const FileTranslator*> supporting(Op op);

/**
 * A file dialog filter for op: "<all> (*.a *.b);;Label (*.a);;...", first entry everything. Empty when nothing supports
 * op. all names the combined entry ("Scenes and meshes").
 */
LILA_SCENE_API std::string dialogFilter(Op op, std::string_view all);

/**
 * Reads or writes path with the translator for its extension (or translatorId when given). The command form runs
 * with options filled in from the option box's remembered values when source is UI. Throws CommandError when nothing
 * handles the file.
 */
LILA_SCENE_API json run(Op op, const fs::path& path, const json& options = json::object(), CallSource source = CallSource::Script,
                        std::string_view translatorId = {});

/** {id, label, doc, extensions, open, import, export, owner} per translator. */
LILA_SCENE_API json describe(const FileTranslator& t);

/** Lila's own: lila, fbx, obj, maya. Idempotent. */
LILA_SCENE_API void registerBuiltins();
/** fileTranslators, openFile, importFile, exportFile. */
LILA_SCENE_API void registerCommands();
} // namespace lila::io::translators
