#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "render/GpuImage.h"
#include "scene/Node.h"

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Live mesh map sets: a bake project's maps kept on the GPU at a preview resolution, as images the texture graph and the
 * viewport sample directly. The baker writes each map's finished result (resolved, finalized, dilated) straight into a
 * GpuImage with no read back; file output is still the bake command's job.
 *
 * One image per low, map and UDIM tile: RGBA16F for vector data, R16F for single-channel data, RGBA8 for colour (sRGB)
 * and exact maps (IDs, islands). Values are what the file would hold. Each image has a version, bumped every time a
 * rebake lands in it, and listeners hear which texel regions changed.
 *
 * While a set is enabled, edits to the project's lows, cages and groups rebake it: the scene is snapshot (debounced),
 * compared with the last snapshot per low triangle, and only the texel tiles whose triangles changed are cleared,
 * re-rasterised and re-traced; whole textures then re-finish on the GPU, which is cheap. Highs, materials, distances or
 * settings changing rebakes everything. GPU work runs as short render-thread slices so frames keep their pace.
 *
 * Sets are named per project: "" is the user's (bakeLiveMaps), the cage view keeps its own ("cagePreview").
 *
 * Commands (registerLiveMapCommands): bakeLiveMaps, bakeMapInfo.
 */
namespace lila::render
{
using json = nlohmann::json;

/** The cage view's set: the group's normal and error maps while live preview is on. */
inline constexpr const char* kCagePreviewSet = "cagePreview";

struct MeshMapInfo
{
    GpuImageRef image;
    /** Bumped every time a rebake lands in the image. */
    uint64_t version = 0;
    scene::NodeId project = scene::kNoNode;
    std::string projectName;
    std::string set;
    std::string group;
    /** The low's node path. */
    std::string low;
    std::string map;
    int udim = 1001;
    int width = 0;
    int height = 0;
    /** srgb (colour maps, sampled through an sRGB view) or linear. */
    std::string colorSpace = "linear";
    /** The finalize's decode data (height range, curvature range, ...), as the bake result's meta. */
    json meta = json::object();

    /** Everything but the image: {project, set, group, low, map, udim, width, height, format, colorSpace, version, meta}. */
    json toJson() const;
};

/**
 * The image of a map by project, map id and UDIM tile. low (a node path, or the soup name the bake used) picks one low
 * of a project with several; empty takes the first low (in group order) that has the tile. Any thread.
 */
LILA_UI_API std::optional<MeshMapInfo> findMeshMap(scene::NodeId project, std::string_view map, int udim = 1001, std::string_view low = {},
                                                   std::string_view set = {});
/** findMeshMap by the project's name, for consumers that hold names (texture sets). Any thread. */
LILA_UI_API std::optional<MeshMapInfo> findMeshMap(std::string_view projectName, std::string_view map, int udim = 1001,
                                                   std::string_view low = {}, std::string_view set = {});
/** findMeshMap's version, 0 when there is no such image. Any thread. */
LILA_UI_API uint64_t meshMapVersion(scene::NodeId project, std::string_view map, int udim = 1001, std::string_view low = {},
                                    std::string_view set = {});
/** Every image of a set, in group, low, map and tile order. Any thread. */
LILA_UI_API std::vector<MeshMapInfo> meshMapSet(scene::NodeId project, std::string_view set = {});

struct MeshMapChange
{
    scene::NodeId project = scene::kNoNode;
    std::string projectName;
    std::string set;
    std::string low;
    std::string map;
    int udim = 1001;
    uint64_t version = 0;
    /** Texel rectangles (x, y, w, h) that changed; empty when the whole image may have. */
    std::vector<glm::ivec4> regions;
};

/** Hears every image an update rewrote, on the main thread, after the update has landed. Returns an id. */
LILA_UI_API int addMeshMapSetListener(std::function<void(const MeshMapChange&)> fn);
LILA_UI_API void removeMeshMapSetListener(int id);

struct LiveMapsOptions
{
    /** Map ids from the project's map list (its settings and options); empty takes every enabled one the GPU keeps resident. */
    std::vector<std::string> maps;
    /** Maps given whole (the cage view's normal and error), instead of maps. */
    std::vector<bake::MapSettings> mapSettings;
    /** bakeGroup nodes; empty takes all of the project's. */
    std::vector<scene::NodeId> groups;
    /** Texels per side; 0 takes the bake/liveResolution pref (1024). */
    int resolution = 0;
    /** 1, 4, 16 or 64; 0 takes the bake/liveSupersampling pref (1). */
    int supersampling = 0;
    /** Dilation in texels; below 0 scales the project's padding to the resolution. */
    int padding = -1;
    /** Quiet time after an edit before the rebake starts; below 0 takes the bake/liveDebounceMs pref (150). */
    int debounceMs = -1;
};

/**
 * Enables (or reconfigures) a project's live set and bakes it: at once on the calling thread in batch mode, off the main
 * thread in the app. Main thread. Throws std::runtime_error without a GPU or when the project has nothing to bake.
 */
LILA_UI_API void enableLiveMaps(scene::Graph& g, scene::NodeId project, const LiveMapsOptions& options = {}, std::string_view set = {});
/** Drops the set and its images. Main thread. */
LILA_UI_API void disableLiveMaps(scene::NodeId project, std::string_view set = {});
LILA_UI_API bool liveMapsEnabled(scene::NodeId project, std::string_view set = {});
/** {enabled, set, maps, skipped, resolution, supersampling, tiles, updates, fullBakes, busy, dirty, last: {seconds, tilesTraced, ...}, error}. */
LILA_UI_API json liveMapsStatus(scene::NodeId project, std::string_view set = {});

/**
 * Cage offsets to bake with instead of the scene's, per low node path, one per point slot (a cage stroke in progress);
 * empty values for a low go back to the scene's. Schedules a rebake. Main thread.
 */
LILA_UI_API void setLiveCageOffsets(scene::NodeId project, std::string_view set, const std::string& low, std::vector<float> values);

/**
 * Snapshots the scene now and rebakes what changed, on the calling thread, waiting for it (batch, tests). Main thread.
 * Returns the update's stats: {full, tilesTraced, tilesDirty, images, seconds, ...}.
 */
LILA_UI_API json updateLiveMaps(scene::Graph& g, scene::NodeId project, std::string_view set = {});

/** Whether edits rebake off the main thread (the app) or wait for updateLiveMaps (batch). Main thread. */
LILA_UI_API void setLiveMapsAsync(bool on);
/** True when no set is dirty, waiting or rebaking. */
LILA_UI_API bool liveMapsIdle();
/** Drops every set and stops the worker. Main thread, before the renderer stops. */
LILA_UI_API void shutdownLiveMaps();

LILA_UI_API void registerLiveMapCommands();
} // namespace lila::render
