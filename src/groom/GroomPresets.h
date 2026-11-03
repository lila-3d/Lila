#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
class Node;
} // namespace lila::scene

/**
 * Hairstyle presets: a groom's lock settings and style stack, and optionally its guides, as .lilagroom files in groom
 * folders from Lila, the studio, the user and the open project:
 *   {"format": "lilagroom", "version": 1, "name", "doc", "lock": {attr: value}, "stack": [{"style", "attrs": {...}}],
 *    "guides": {"min": [x, y, z], "max": [x, y, z], "splines": [...]}}
 * Guides are stored with the scalp's bounds, so loading onto another head stretches them to fit and roots them on it.
 * A preset's id is "<source>/<file name without the extension>".
 */
namespace lila::groom::presets
{
inline constexpr const char* kExtension = ".lilagroom";

struct Folder
{
    std::filesystem::path dir;
    /** "lila", "studio", "user" or "project". */
    std::string source;
    bool writable = false;
};

struct Preset
{
    std::string id;
    std::string name;
    std::string doc;
    std::string source;
    bool guides = false;
    std::filesystem::path file;
};

LILA_SCENE_API std::vector<Folder> folders();
LILA_SCENE_API std::vector<Preset> list();

/** The preset document of a groom; guides too when withGuides. */
LILA_SCENE_API nlohmann::json capture(scene::Graph& g, scene::Node& groom, bool withGuides);
/**
 * Applies a preset document to a groom: lock settings, the stack replaced, and the guides added (or replacing the
 * groom's when replaceGuides) fitted to its scalp. Returns how many guides it added.
 */
LILA_SCENE_API int apply(scene::Graph& g, scene::Node& groom, const nlohmann::json& preset, bool guides, bool replaceGuides);

void registerCommands();
} // namespace lila::groom::presets
