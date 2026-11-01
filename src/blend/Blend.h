#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Blender scene import without Blender: blend::File reads the .blend's own struct descriptions, and one converter turns
 * the scene's view layer into Lila nodes.
 *
 * What converts: objects (transforms in every rotation mode, delta transforms, parenting, visibility), collections as
 * groups, collection instances, meshes (positions, faces, smooth and sharp flags, UV maps, color attributes, creases,
 * custom normals, other float attributes, material slots), empties, cameras, lights, curves (Bezier, NURBS, poly),
 * armatures as joints with their skins (vertex groups and the Armature modifier), shape keys as morphs, keyframed
 * actions on objects, bones and shape keys, Principled BSDF materials with their image textures (packed images are
 * written out), and the modifiers Lila has live (mirror, subdivision, bevel, array, triangulate, solidify, decimate,
 * weighted normals). Anything else is reported and, for a dropped modifier, flagged on the object as an issue.
 *
 * Blender is Z up in metres; Lila scenes are converted to their own up axis and centimetres.
 */
namespace lila::blend
{
namespace fs = std::filesystem;

class File;

struct ImportOptions
{
    /** Parent the imported roots under this node. */
    scene::NodeId parent = scene::kNoNode;
    /** Prefix every imported name with this namespace. */
    std::string nameSpace;
    /** Collections become groups holding their objects; off puts the objects at the top. */
    bool collections = true;
    bool materials = true;
    bool skins = true;
    bool morphs = true;
    bool animation = true;
    /** Modifiers Lila has become live modifiers in the stack; off imports the meshes without them. */
    bool modifiers = true;
    /** Objects hidden in the viewport are imported hidden; off skips them. */
    bool hidden = true;
    /**
     * "off", "unsupported" (objects whose modifiers Lila can't rebuild get Blender's evaluated mesh) or "all". Runs the
     * Blender at blenderPath (found under Program Files when empty) in the background.
     */
    std::string evaluate = "off";
    std::string blenderPath;
    /** Set the scene fps and playback range from the file. */
    bool setPlayback = true;
    /** Where packed and converted images are written; <file>.textures beside the .blend when empty. */
    std::string textureDir;
};

struct ImportResult
{
    bool ok = false;
    std::string error;
    std::vector<scene::NodeId> roots;
    std::vector<scene::NodeId> nodes; // every DAG node created, parents first
    std::vector<scene::NodeId> meshes;
    std::vector<scene::NodeId> joints;
    std::vector<scene::NodeId> skins;
    std::vector<scene::NodeId> morphs;
    std::vector<scene::NodeId> cameras;
    std::vector<scene::NodeId> lights;
    std::vector<scene::NodeId> curves;
    std::vector<std::string> materials;
    std::vector<std::string> modifiers; // modifier nodes created
    /** Blender object name to the Lila node path made for it. */
    std::map<std::string, std::string> objectNames;
    /** Objects whose meshes came from Blender's evaluation. */
    std::vector<std::string> evaluated;
    /** Things in the file that were not converted, one line each. */
    std::vector<std::string> skipped;
    std::vector<std::string> warnings;
    std::string version;
    double fps = 0.0;
};

/** Main thread. Undoable as one step when run inside a command. */
LILA_SCENE_API ImportResult importBlend(scene::Graph& graph, const fs::path& path, const ImportOptions& opts = {});
/** The same from a file already read (the slow part, which touches nothing and can run off the main thread first). */
LILA_SCENE_API ImportResult importBlend(scene::Graph& graph, const File& file, const ImportOptions& opts = {});

/** What blendInfo reports: version, ID counts, object types, modifier and node types used, libraries. */
LILA_SCENE_API nlohmann::json info(const File& file);

/** Bumped whenever an import of the same file would come out differently, so cached conversions (sample scenes) are redone. */
inline constexpr const char* kImporterVersion = "2";

/** blendImport, blendOpen, blendConvert, blendInfo. Needs the scene and io commands. */
LILA_SCENE_API void registerCommands();
} // namespace lila::blend
