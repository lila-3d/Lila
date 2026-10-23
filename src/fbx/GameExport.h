#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <filesystem>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Maya's Game Exporter, Model tab: exports everything, the selection or object sets to FBX (or OBJ), into one file,
 * one per set or one per object. Settings are one json object, the same keys as the gameExport command's flags:
 *
 *   path, filename, fileType (fbx | obj), mode (all | selection | sets), sets [names], separate (none | perSet |
 *   perMember), moveToOrigin, profile (ue5 | ue6 | generic), triangulate, smoothingGroups, tangents,
 *   units (cm | m | mm | in | ft), embedMedia
 *
 * Presets are named settings kept in the "gameExporter" prefs (/presets) or in the scene as gameExportPreset nodes.
 */
namespace lila::fbx::game
{
/** One file to write and the nodes that go in it. */
struct Job
{
    std::filesystem::path file;
    std::vector<scene::NodeId> nodes;
};

/** Every setting at its default. */
LILA_SCENE_API nlohmann::json defaults();
/** defaults() overlaid with `settings`; unknown keys are kept. */
LILA_SCENE_API nlohmann::json resolved(const nlohmann::json& settings);

/** The files an export with these settings writes. Throws std::invalid_argument for a bad setting or an unknown set. */
LILA_SCENE_API std::vector<Job> plan(scene::Graph& g, const nlohmann::json& settings);

/** Writes the jobs. Returns the paths written; throws std::runtime_error on the first failure. */
LILA_SCENE_API std::vector<std::filesystem::path> run(scene::Graph& g, const nlohmann::json& settings);

/** Centimetres per unit for "cm", "m", "mm", "in", "ft"; 0 for anything else. */
LILA_SCENE_API double cmPerUnit(const std::string& units);

/** gameExport, gameExportPresets, gameExportSavePreset, gameExportDeletePreset, plus the gameExportPreset node type. */
LILA_SCENE_API void registerCommands();
} // namespace lila::fbx::game
