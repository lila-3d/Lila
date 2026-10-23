#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"
#include "scene/Plug.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Animated mesh export: any mesh whose shape changes over time (geometry nodes, deformers, skins, keyed modifiers) is
 * sampled frame by frame. Either one file per frame (OBJ or FBX, the static meshes an Unreal Niagara mesh flipbook
 * wants) or one Alembic file holding every frame (a Geometry Cache).
 *
 * Every frame goes through the same steps: set the time, evaluate, take the meshes (evaluated, or the cage without
 * modifiers), bake world space, recentre, scale, then the axis profile, the attribute filter and the file.
 */
namespace lila::seq
{
namespace fs = std::filesystem;

enum class Recenter
{
    None,
    /** The first frame's bounding box centre moves to the origin; every frame takes the same offset. */
    First,
    /** Each frame is centred on its own bounding box. */
    Each,
};

struct SequenceOptions
{
    double start = 1.0;
    double end = 24.0;
    double step = 1.0;
    /** Use the scene's playback range instead of start and end. */
    bool playbackRange = false;
    /** Frames per second for frame-to-time; 0 means the scene's. */
    double fps = 0.0;

    /** obj | fbx | abc. */
    std::string format = "fbx";
    std::string folder;
    /** File name without extension. {name} is the node (or the first one), {index} counts from 0, {frame} is the frame number. */
    std::string pattern = "{name}_{index}";
    /** Zero padding for {index} and {frame}. */
    int padding = 3;

    /** Multiplies every length, after recentring (0.1 turns 1 m into 10 cm of file units). */
    double scale = 1.0;
    /** ue5 | ue6 | generic, the FBX writer's axis profiles; Alembic and OBJ get the Y-up orientation that lands the same in Unreal. */
    std::string profile = "ue5";
    /** The evaluated mesh; off exports the base mesh without its modifier stack. */
    bool applyModifiers = true;
    bool worldSpace = true;
    Recenter recenter = Recenter::None;
    bool normals = true;
    bool uvs = true;
    bool colors = true;
    bool triangulate = true;
};

/** Frame numbers the options sample: start to end inclusive by step. Throws std::invalid_argument for a bad range. */
LILA_SCENE_API std::vector<double> frames(const SequenceOptions& o);

/** The file name for one frame (no folder, no extension). */
LILA_SCENE_API std::string frameName(const std::string& pattern, const std::string& name, int index, double frame, int padding);

/** "none" | "first" | "each". False for anything else. */
LILA_SCENE_API bool parseRecenter(std::string_view s, Recenter* out);

/** Mesh nodes for `roots`: each root that is a mesh, plus mesh descendants of the rest. */
LILA_SCENE_API std::vector<scene::NodeId> meshNodes(scene::Graph& g, const std::vector<scene::NodeId>& roots);

/** One sampled frame: each node's mesh in export space (Lila axes; the writer applies the profile). */
struct FrameMeshes
{
    int index = 0;
    double frame = 0.0;
    std::vector<std::string> names;
    std::vector<geo::Mesh> meshes;
};

/**
 * Main thread. Samples every frame, calling `sink` for each; the scene time is restored afterwards, also on a throw.
 * Opens a cancellable progress scope titled `title` (progress::Cancelled when cancelled).
 */
LILA_SCENE_API void sample(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const SequenceOptions& o,
                           const std::function<void(FrameMeshes&)>& sink, const std::string& title = "Exporting mesh sequence");

/** Writes one file per frame (obj, fbx) or one .abc. Returns the paths written. Throws std::runtime_error. */
LILA_SCENE_API std::vector<fs::path> exportSequence(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const SequenceOptions& o);

/** The files exportSequence would write, without evaluating anything. */
LILA_SCENE_API std::vector<fs::path> plannedFiles(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const SequenceOptions& o);

/** Options from a json object of the exportMeshSequence flags (unknown keys ignored). Throws std::invalid_argument. */
LILA_SCENE_API SequenceOptions fromJson(const nlohmann::json& j);
LILA_SCENE_API nlohmann::json toJson(const SequenceOptions& o);

/** exportMeshSequence and exportAlembic. */
LILA_SCENE_API void registerCommands();
} // namespace lila::seq
