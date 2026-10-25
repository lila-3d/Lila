#pragma once

#include "core/Export.h"

#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lila::scene
{
/**
 * Maya's LOD group: a transform whose children, in order, are LOD0, LOD1, ... (each a mesh or a group of meshes). FBX
 * writes it as an FbxLODGroup, which Unreal imports as one mesh with those LODs. screenSizes are Unreal's LOD screen
 * sizes, one per level; levels without one use Unreal's automatic sizes.
 */
class LILA_SCENE_API LodGroup : public Transform
{
public:
    enum Attr
    {
        kScreenSizes = Transform::kCount,
        kLodDisplay,
        kDisplayLevel,
        kCount
    };
};

namespace lod
{
/** Unreal's limit, MAX_STATIC_MESH_LODS. */
inline constexpr int kMaxLevels = 8;

/** How a view picks the level it shows. */
enum class Mode
{
    ScreenSize, // by projected size against the screen sizes, as Unreal does
    Level,      // a fixed level (the last one when the group has fewer)
    All,        // every level at once
};

/** lodDisplay on a group: Viewport follows the view's mode, the rest override it for that group. */
enum class Display
{
    Viewport,
    ScreenSize,
    Level,
    All,
};

LILA_SCENE_API const char* modeName(Mode m);
/** "screenSize", "level", "all". False for anything else. */
LILA_SCENE_API bool parseMode(std::string_view name, Mode* out);

/** What one view needs to choose levels. proj is the view's projection, as Unreal's ComputeBoundsScreenSize takes it. */
struct View
{
    Mode mode = Mode::ScreenSize;
    int level = 0;
    glm::dvec3 eye{0.0};
    glm::dmat4 proj{1.0};
    bool ortho = false;
};

LILA_SCENE_API bool isGroup(const Node& n);
/** The group's levels: its transform children in order. */
LILA_SCENE_API std::vector<NodeId> levels(const Graph& g, NodeId group);

/** Space or comma separated numbers; anything unreadable is dropped. */
LILA_SCENE_API std::vector<double> parseSizes(std::string_view text);
LILA_SCENE_API std::string formatSizes(const std::vector<double>& sizes);
/** UStaticMesh::ComputeLODScreenSize: 1 for LOD0, then 0.75^i kept below the level before. */
LILA_SCENE_API double autoScreenSize(int level, double previous);
/** The group's screen sizes for count levels: its own where set, automatic for the rest. */
LILA_SCENE_API std::vector<double> screenSizes(const Node& group, size_t count);

/** ComputeBoundsScreenSize: the bounding sphere's projected diameter as a fraction of the view. */
LILA_SCENE_API double boundsScreenSize(const glm::dvec3& center, double radius, const View& view);
/** The level Unreal draws at that screen size: the last whose screen size is above it, else LOD0. */
LILA_SCENE_API int levelForScreenSize(const std::vector<double>& sizes, double screenSize);

/** The level the view shows of the group, or -1 for all of them. */
LILA_SCENE_API int shownLevel(Graph& g, NodeId group, const View& view);
/** Every node the view hides: the levels each group does not show, with their descendants. Reads evaluated values. */
LILA_SCENE_API std::unordered_set<NodeId> hidden(Graph& g, const View& view);

/** For Send to Unreal: the screen sizes of the first group at or under roots that sets any, one per level; empty for none. */
LILA_SCENE_API std::vector<double> exportScreenSizes(const Graph& g, const std::vector<NodeId>& roots);

LILA_SCENE_API void registerTypes();
/** lodGroup. */
LILA_SCENE_API void registerCommands();
} // namespace lod
} // namespace lila::scene
