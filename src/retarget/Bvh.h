#pragma once

#include "core/Export.h"
#include "scene/Node.h"

#include <filesystem>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/** BVH motion capture files: a joint hierarchy with per-frame channels. */
namespace lila::retarget::bvh
{
struct Joint
{
    std::string name;
    int parent = -1;
    glm::dvec3 offset{0.0};
    /** Xposition ... Zrotation, in file order; empty for an end site. */
    std::vector<std::string> channels;
    bool endSite = false;
};

struct LILA_SCENE_API File
{
    std::vector<Joint> joints; // parents first
    int frames = 0;
    double frameTime = 1.0 / 30.0;
    /** frames x channel count, in joint then channel order. */
    std::vector<double> values;
    int channelCount() const;
};

/** Throws std::invalid_argument with the line that went wrong. */
LILA_SCENE_API File parse(const std::string& text);

struct ImportOptions
{
    /** Multiplies every offset and position (BVH has no units; cm files use 1). */
    double scale = 1.0;
    scene::NodeId parent = scene::kNoNode;
    /** Set the scene fps and playback range from the file. */
    bool setPlayback = true;
    /** Make end sites as small non-deform joints, so the last bones keep their direction. */
    bool endSites = true;
};

struct ImportResult
{
    scene::NodeId root = scene::kNoNode;
    std::vector<scene::NodeId> joints;
    int frames = 0;
    double fps = 30.0;
    double start = 0.0, end = 0.0;
};

/** Makes the skeleton as joints keyed every frame. Undoable. Throws std::invalid_argument on a bad file. */
LILA_SCENE_API ImportResult import(scene::Graph& g, const std::filesystem::path& path, const ImportOptions& opts = {});
} // namespace lila::retarget::bvh
