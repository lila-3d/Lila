#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/**
 * The brush alpha library: grayscale images in alphas folders, from Lila itself, each loaded extension, the user and
 * the open project; each folder's subfolders are categories. A brush names a library alpha as
 * "alpha://<source>/<path in the folder>", so the choice survives the folders moving; any other alpha path is an image file.
 */
namespace lila::brush::alphas
{
inline constexpr std::string_view kScheme = "alpha://";

struct Folder
{
    std::filesystem::path dir;
    /** "lila", an extension id, "user" or "project". */
    std::string source;
    /** Shown in pickers. */
    std::string label;
};

struct Entry
{
    /** The alpha:// reference brushes store. */
    std::string ref;
    /** The file name without its extension. */
    std::string name;
    std::filesystem::path file;
    std::string source;
    std::string sourceLabel;
    /** The subfolder it sits in ("skin", "hard surface"), empty at the folder's top. */
    std::string category;
};

/** Lila's resources/alphas, each loaded extension's resources/alphas, the user's alphas, the project's .lila/alphas. */
LILA_SCENE_API std::vector<Folder> folders();
/** The user's folder, or the project's (empty without an open project). */
LILA_SCENE_API std::filesystem::path userDir();
LILA_SCENE_API std::filesystem::path projectDir();
/** Every image in the folders and their subfolders, in folder order, then category, then name. */
LILA_SCENE_API std::vector<Entry> library();
/** The image file a brush's alpha path means: a library reference resolved (empty when gone), else the path itself. */
LILA_SCENE_API std::string resolve(std::string_view alphaPath);
/** Short text for a stored alpha path: the library name, or the file name. */
LILA_SCENE_API std::string displayName(std::string_view alphaPath);
/** Copies an image into dir (created), renaming on a clash. Returns the reference to it, empty on failure. */
LILA_SCENE_API std::string add(const std::filesystem::path& image, const std::filesystem::path& dir, std::string* error = nullptr);
} // namespace lila::brush::alphas
