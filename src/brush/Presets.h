#pragma once

#include "core/Export.h"

#include "brush/Brush.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Brush presets: a brush with its stroke settings and alpha under a name of its own, kept as .lilabrush files in brushes
 * folders from Lila, each loaded extension, the studio, the user and the open project; each folder's subfolders are
 * categories. Texture paint presets (tex/BrushPresets.h) share the folders and the format; "tool" tells them apart:
 *   {"format": "lilabrush", "version": 1, "tool": "sculpt", "name", "icon", "doc", "sculptBrush": "damStandard",
 *    "brush": BrushSettings::toJson() without the alpha, "alphaRef": "alpha://...", "alphaImage": {"name", "data": base64}}
 * The alpha image travels inside the file, so one file is the whole preset. A preset's id is
 * "<source>/<path in its folder without the extension>".
 */
namespace lila::brush::presets
{
inline constexpr std::string_view kExtension = ".lilabrush";

struct Folder
{
    std::filesystem::path dir;
    /** "lila", an extension id, "studio", "user" or "project". */
    std::string source;
    std::string label;
    /** Only the user's and the project's presets are saved or deleted from Lila. */
    bool writable = false;
};

struct Preset
{
    std::string id;
    std::string label;
    /** An icon name from Lila's icon set; empty draws the brush's own. */
    std::string icon;
    std::string doc;
    /** The tool it is for: "sculpt", or "paint" for texture paint presets. */
    std::string tool = "sculpt";
    /** The brush it drives: a built-in's name or a registered brush's. */
    std::string brush;
    /** Stroke settings, alphaPath pointing at the library alpha or the embedded image's extracted copy. */
    BrushSettings settings;
    std::filesystem::path file;
    std::string source;
    std::string sourceLabel;
    std::string category;
};

LILA_SCENE_API std::vector<Folder> folders();
LILA_SCENE_API std::filesystem::path userDir();
/** Empty without an open project. */
LILA_SCENE_API std::filesystem::path projectDir();

/** Every preset in the folders, for one tool or all, in folder order, then category, then label. */
LILA_SCENE_API std::vector<Preset> library(std::string_view tool = {});
LILA_SCENE_API std::optional<Preset> find(std::string_view id);
/** Reads one sculpt preset file; its id stays empty. */
LILA_SCENE_API std::optional<Preset> load(const std::filesystem::path& file, std::string* error = nullptr);
/** Writes p to file with its alpha image embedded. */
LILA_SCENE_API bool write(const Preset& p, const std::filesystem::path& file, std::string* error = nullptr);
/** Saves p into dir under a file name from its label, renaming on a clash unless replace. Returns the new id, empty on failure. */
LILA_SCENE_API std::string save(const Preset& p, const std::filesystem::path& dir, bool replace = false, std::string* error = nullptr);
/** Copies a .lilabrush file (any tool's) into dir, renaming on a clash. Returns its id, empty on failure. */
LILA_SCENE_API std::string install(const std::filesystem::path& file, const std::filesystem::path& dir, std::string* error = nullptr);
/** Deletes a user or project preset. */
LILA_SCENE_API bool remove(std::string_view id, std::string* error = nullptr);

/** Called after save, install and remove. */
LILA_SCENE_API int addListener(std::function<void()> fn);
LILA_SCENE_API void removeListener(int id);
LILA_SCENE_API void notifyChanged();

/** brushPresets, brushPresetSave, brushPresetInstall, brushPresetExport, brushPresetDelete. */
LILA_SCENE_API void registerCommands();
} // namespace lila::brush::presets
