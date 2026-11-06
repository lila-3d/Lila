#pragma once

#include "core/Export.h"
#include "fbx/Fbx.h"
#include "gltf/Gltf.h"
#include "scene/Plug.h"

#include <filesystem>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * COLLADA 1.4.1 (.dae) import and export, both our own.
 *
 * Export writes what Maya's COLLADA exporter writes by default: Y up, centimetres, each node's transform
 * as translate, jointOrient, rotate and scale elements with every animated channel sampled per frame as its own
 * curve (or, with bakeTransforms, one matrix per node animated as sampled matrices), meshes as polylists with shared
 * positions, skins as controllers with bind shape and inverse bind matrices, joints as JOINT nodes, several takes as
 * animation clips, and materials as profile_COMMON effects.
 *
 * Import reads any up axis and unit, transform stacks (matrix, translate, rotate, scale, in order), polylist,
 * triangles and polygons, skin controllers (joint lists by sid or id), node instances, and animations of whole
 * matrices or single transform elements and their components, sampled once per frame.
 */
namespace lila::collada
{
namespace fs = std::filesystem;

struct ImportOptions
{
    /** Parent the imported roots under this node. */
    scene::NodeId parent = scene::kNoNode;
    /** Skin controllers become skinClusters on joints; off imports the bind meshes unskinned. */
    bool skins = true;
    /** Bake the animations onto the nodes, one key per frame. */
    bool animation = true;
    /** Frames per second to sample at; 0 guesses from the key spacing. */
    double fps = 0.0;
    /** Set the scene fps and playback range from the animation. */
    bool setPlayback = true;
    /** Material nodes from the file's effects; off keeps only the slot names on the faces. */
    bool materials = true;
};

struct ImportResult
{
    bool ok = false;
    std::string error;
    std::vector<scene::NodeId> roots;
    std::vector<scene::NodeId> nodes; // every node created, parents first
    std::vector<scene::NodeId> joints;
    std::vector<scene::NodeId> skins; // skinCluster nodes
    std::vector<std::string> materials;
    std::vector<std::string> clips; // animation clip names in the file
    double fps = 0.0;
    double start = 0.0, end = 0.0; // seconds
    int curves = 0;
    std::string upAxis;
    double unitMeters = 0.01;
    std::string authoringTool;
    std::vector<std::string> skipped;
    std::vector<std::string> warnings;
};

/** Main thread. Creates the file's visual scene under the parent, undoably. */
LILA_SCENE_API ImportResult importCollada(scene::Graph& graph, const fs::path& path, const ImportOptions& opts = {});

struct ExportOptions
{
    /** What to gather: kind, children, atOrigin, worldSpace, takes, range, influences, deformOnly, unitScale. */
    fbx::ExportOptions gather;
    /**
     * Each node's transform as one matrix, animated as sampled matrices (Maya's bakeTransforms). Off writes Maya's
     * elements, translate, jointOrient, rotate and scale, each animated channel its own curve: what Maya's exporter
     * writes by default.
     */
    bool bakeTransforms = false;
    /** Polylists keep quads and n-gons; true writes triangles. */
    bool triangles = false;
    /** Write texture maps as images and samplers; false writes material colors only. */
    bool textures = true;
    /** Copy the textures next to the file; false references them where they are. */
    bool copyTextures = false;
    /** Write vertex color sets. */
    bool vertexColors = false;
    /** Write lights. */
    bool lights = true;
};

/** Any thread. Writes the .dae (and copied textures). Warnings about what could not be written go to warnings. */
LILA_SCENE_API bool writeCollada(const gltf::ExportSnapshot& snapshot, const fs::path& path, const ExportOptions& opts, std::string* error,
                                 std::vector<std::string>* warnings = nullptr);

/** importCollada and exportCollada. */
LILA_SCENE_API void registerCommands();
} // namespace lila::collada
