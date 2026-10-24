#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <functional>
#include <map>
#include <vector>

namespace lila::scene
{
enum class ComponentMode : uint8_t
{
    Object,
    Vertex,
    Edge,
    Face,
};

/**
 * Selected mesh components per node, Maya style: the object selection decides which meshes are live (hilited) and
 * the mode decides which component kind clicks pick. Ids are those of the mesh's outMesh. Changes record undo.
 */
namespace components
{
using Ids = std::vector<uint32_t>;

LILA_SCENE_API ComponentMode mode();
/** Not undoable, like Maya's selectMode. */
LILA_SCENE_API void setMode(ComponentMode m);
/** setMode recorded on the undo stack, for commands that switch mode as part of an edit. */
LILA_SCENE_API void setModeUndoable(ComponentMode m);
LILA_SCENE_API const char* modeName(ComponentMode m);
LILA_SCENE_API ComponentMode modeFromName(const std::string& name); // throws std::invalid_argument

LILA_SCENE_API const std::map<NodeId, Ids>& selected(ComponentMode kind);
LILA_SCENE_API const Ids& selected(NodeId node, ComponentMode kind);

enum class Op
{
    Replace,
    Add,
    Remove,
    Toggle,
};
/** Changes one node's selection of one kind. Replace clears that kind on every other node first. */
LILA_SCENE_API void select(NodeId node, ComponentMode kind, const Ids& ids, Op op = Op::Replace);
LILA_SCENE_API void clear();
/** Replaces everything at once, as one undo step. */
LILA_SCENE_API void setAll(std::map<NodeId, Ids> points, std::map<NodeId, Ids> edges, std::map<NodeId, Ids> faces);

/** Points touched by the node's selection of the current mode (verts as is, edge ends, face corners). */
LILA_SCENE_API Ids pointsOf(NodeId node);

/**
 * Maya's polyListComponentConversion: edges/faces to their points, points to edges/faces they fully cover, or with
 * touching to every edge/face that shares a point with them (Edge to Face: every face bordering an edge).
 */
LILA_SCENE_API Ids convert(const geo::Mesh& mesh, ComponentMode from, const Ids& ids, ComponentMode to, bool touching = false);

LILA_SCENE_API int addListener(std::function<void()> fn);
LILA_SCENE_API void removeListener(int id);

LILA_SCENE_API void registerCommands();
/** selectLoopThrough, selectPathBetween and selectContainedFaces (scene/SelectPaths.cpp); called by registerCommands. */
LILA_SCENE_API void registerPathCommands();
} // namespace components

/**
 * Soft-selection weight per point (1 for selected points, then the falloff curve out to 0 at the radius), from the general
 * prefs /softSelect/enabled, /softSelect/radius (cm, world space), /softSelect/curve and /softSelect/falloffMode (see
 * scene/SoftSelect.h). All zero but the selection when it's off.
 */
LILA_SCENE_API std::vector<float> softWeights(const geo::Mesh& mesh, const glm::dmat4& world, const components::Ids& selectedPoints);

/** World-space centre of the selected components' points. */
LILA_SCENE_API glm::dvec3 selectionCenter();
/** Applies a world-space transform to the selected components' points (with symmetry), as one undo step. */
LILA_SCENE_API void transformSelectedComponents(const char* label, const glm::dmat4& worldXform);

/**
 * Object-space unit normal per point (zero where it has no faces): the area-weighted sum of its faces' normals. With a face
 * selection, points on the selected faces sum only those, so the region moves as one surface.
 */
LILA_SCENE_API std::vector<glm::dvec3> componentNormals(const geo::Mesh& mesh, ComponentMode kind, const components::Ids& ids);
/** World-space average normal of the selected components, for orienting the manipulator; +Y when there is none. */
LILA_SCENE_API glm::dvec3 selectionNormal();
/** Moves each selected point (with soft selection and symmetry) by a world distance along its own normal, as one undo step. */
LILA_SCENE_API void offsetSelectedComponents(const char* label, double distance);
} // namespace lila::scene
