#pragma once

#include "UsdCommon.h"

#include <lila/Graph.h>

#include <pxr/usd/usd/stage.h>

#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lila::usd
{
enum class Format
{
    Auto, // by extension: .usda text, .usdc and .usd binary, .usdz package
    Usda,
    Usdc,
};

struct ExportOptions
{
    /** File axes: Z up (Unreal's, the default) or Y up. */
    bool zUp = true;
    /** 0.01 is centimetres, what Unreal expects. */
    double metersPerUnit = 0.01;
    /** For .usd: text or binary crate (binary by default). */
    Format format = Format::Auto;
    bool materials = true;
    /** How texture files are referenced; usdz always packages them. */
    enum class Textures
    {
        Relative,
        Absolute,
        Copy,
    } textures = Textures::Relative;
    bool animation = true;
    /** Scene takes to write: names, or just "all". Empty writes the active take (or the playback range). */
    std::vector<std::string> takes;
    /** Range in seconds; NaN is the playback range. */
    double start = std::numeric_limits<double>::quiet_NaN();
    double end = std::numeric_limits<double>::quiet_NaN();
    bool normals = true;
    bool triangulate = false;
    int maxInfluences = 8;
    /** Deform joints only, as FBX skeletal exports do. */
    bool deformOnly = true;
    bool atOrigin = false;
    bool children = true;
    bool worldSpace = false;
    /** LOD groups as Unreal's LOD variant set; off writes every level as a plain child. */
    bool lods = true;
    /** Geometry, materials and animation in their own sublayers, referenced from the file as its root layer. */
    bool splitLayers = false;
    /** Time codes per second of the stage; 0 takes the animation's fps. */
    double timeCodesPerSecond = 0.0;
    /** The roots' transforms relative to this node instead of the world. */
    scene::NodeId relativeTo = scene::kNoNode;
    /** Roots written alone, without their descendants. */
    std::set<scene::NodeId> childless;
    /** Roots below a node other than relativeTo keep their own local transforms (fbx::ExportOptions::localRoots). */
    bool localRoots = false;
    /** Keep the stage in memory instead of writing it: ExportResult::stage, with where each node and material went. */
    bool inMemory = false;
};

struct ExportResult
{
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    /** Every file written, the root layer first. */
    std::vector<std::string> files;

    /** inMemory: the stage, the prim each node was written as and the material prim of each slot. */
    pxr::UsdStageRefPtr stage;
    std::vector<std::pair<scene::NodeId, pxr::SdfPath>> prims;
    std::map<std::string, pxr::SdfPath> materials;
    /** inMemory: each Skeleton prim written, with its joints' nodes in the order of its joints attr (none when synthetic). */
    std::vector<std::pair<pxr::SdfPath, std::vector<scene::NodeId>>> skeletons;
};

/** Main thread: snapshots the scene (fbx::gatherExport) and writes it as a USD stage. */
ExportResult exportUsd(scene::Graph& graph, const std::vector<scene::NodeId>& roots, const fs::path& path, const ExportOptions& opts);
} // namespace lila::usd
