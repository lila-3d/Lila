#pragma once

#include "core/Export.h"

#include "bake/Bake.h"
#include "scene/Node.h"

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Live preview bakes for cage work: one group's tangent normal map and error map at a low resolution, kept in memory and
 * never written. One preview runs at a time off the main thread; a new request cancels the running one and runs next,
 * so painting only ever waits for the latest state. Listeners hear each finished preview on the main thread.
 */
namespace lila::bake
{
struct CagePreviewTexture
{
    /** The low it was baked for (its node path). */
    std::string mesh;
    /** normal or error. */
    std::string map;
    int udim = 1001;
    int width = 0;
    int height = 0;
    /** Row 0 at the top (V = 1), as written textures are. */
    std::vector<glm::vec4> value;
    /** The normal map's convention: true for OpenGL (green up). */
    bool greenUp = false;
};

struct LILA_SCENE_API CagePreview
{
    scene::NodeId group = scene::kNoNode;
    uint64_t generation = 0;
    std::vector<CagePreviewTexture> textures;
    GroupStats stats;
    double seconds = 0.0;
    std::string backend;

    const CagePreviewTexture* find(const std::string& mesh, const std::string& map) const;
};

struct CagePreviewOptions
{
    int resolution = 512;
    /** Empty picks the GPU backend when there is one. */
    std::string backend;
    /** bakeOffset values to bake with instead of the scene's, per low node path (one per point slot): a stroke in progress. */
    std::map<std::string, std::vector<float>> offsets;
};

/** Snapshots the group now and queues its preview bake (cancelling one that runs). Main thread. Throws std::runtime_error. */
LILA_SCENE_API void requestCagePreview(scene::Graph& g, scene::NodeId group, const CagePreviewOptions& options = {});
/** Cancels the running and queued previews; wait joins the worker. */
LILA_SCENE_API void cancelCagePreview(bool wait);
LILA_SCENE_API bool cagePreviewBusy();
/** The last finished preview of a group, null when none. */
LILA_SCENE_API std::shared_ptr<const CagePreview> latestCagePreview(scene::NodeId group);
/** Forgets a group's preview (the display went away). */
LILA_SCENE_API void clearCagePreview(scene::NodeId group);
LILA_SCENE_API int addCagePreviewListener(std::function<void(const std::shared_ptr<const CagePreview>&)> fn);
LILA_SCENE_API void removeCagePreviewListener(int id);
} // namespace lila::bake
