#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lila::scene
{
class Graph;
class Node;

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
/** Changes whenever a component selection or the mode does, so drawings of it can be cached. */
LILA_SCENE_API uint64_t serial();

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

/**
 * Point-like components of nodes that aren't meshes (curve points and handles): vertex mode picks, centres and moves them
 * through these, by node type. Ids are the provider's own.
 */
struct PointProvider
{
    std::string nodeType;
    /** Object-space position of every component, by id. */
    std::function<std::vector<std::pair<uint32_t, glm::dvec3>>(Graph&, NodeId)> points;
    /** Moves components to object-space positions, as an undoable edit of the node. */
    std::function<void(Graph&, NodeId, const std::vector<std::pair<uint32_t, glm::dvec3>>&)> setPoints;
    /** The plug a live drag writes and restores, any history feeding it frozen first. */
    std::function<PlugRef(Graph&, NodeId)> editPlug;
    /**
     * Optional object-space frame of each given component, columns U, V and N (zero where it has none): the axes the move
     * tool's Normal orientation drags each one along, averaged for the manipulator's Normal and Component orientations.
     */
    std::function<std::vector<glm::dmat3>(Graph&, NodeId, const Ids&)> frames;
    /** Applies an object-space matrix to all of the node's geometry, as an undoable edit (Freeze Transformations). */
    std::function<void(Graph&, NodeId, const glm::dmat4&)> bake;
};
LILA_SCENE_API void registerPointProvider(PointProvider p);
/** The provider for the node's type or a base type; null for meshes and anything else. */
LILA_SCENE_API const PointProvider* pointProvider(const Node& n);

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
/** Where a world point lands on a surface, for conforming moved components to it. */
using SurfaceProjector = std::function<glm::dvec3(const glm::dvec3& world)>;
/** transformSelectedComponents with each moved point then placed by onto (soft selection blends toward it), as one undo step. */
LILA_SCENE_API void transformSelectedComponentsOnto(const char* label, const glm::dmat4& worldXform, const SurfaceProjector& onto);
/** Closest points on the evaluated surfaces of the given meshes, each BVH built once; returns the point unchanged when none has faces. */
LILA_SCENE_API SurfaceProjector closestOnMeshes(const std::vector<NodeId>& meshes);

/**
 * Object-space unit normal per point (zero where it has no faces): the area-weighted sum of its faces' normals. With a face
 * selection, points on the selected faces sum only those, so the region moves as one surface.
 */
LILA_SCENE_API std::vector<glm::dvec3> componentNormals(const geo::Mesh& mesh, ComponentMode kind, const components::Ids& ids);
/** World-space average normal of the selected components, for orienting the manipulator; +Y when there is none. */
LILA_SCENE_API glm::dvec3 selectionNormal();
/** Moves each selected point (with soft selection and symmetry) by a world distance along its own normal, as one undo step. */
LILA_SCENE_API void offsetSelectedComponents(const char* label, double distance);
/**
 * Object-space frame per point, columns U tangent, V tangent and normal (zero where it has no faces): the axes the move tool's
 * Normal orientation drags each point along. The normal is componentNormals'. U is the uv0 u direction (dP/du of its faces,
 * area-weighted) projected into the normal's plane; where there are no uvs, or mirrored islands cancel at the point, it is
 * reference projected instead. In edge mode a point on selected edges takes U along them. V = N x U.
 */
LILA_SCENE_API std::vector<glm::dmat3> componentFrames(const geo::Mesh& mesh, ComponentMode kind, const components::Ids& ids,
                                                       const glm::dvec3& reference);
/**
 * World-space manipulator frame for the Normal orientation: Z the selection's average normal, X the average of its points' U
 * tangents. The tangent reference is the world axis least aligned with that normal. Provided points (curves) use their
 * provider's frames.
 */
LILA_SCENE_API glm::dmat3 selectionNormalFrame();
/** Moves each selected point by uvn along its own world U, V and N (componentFrames, or its provider's frames), as one undo step. */
LILA_SCENE_API void offsetSelectedComponents(const char* label, const glm::dvec3& uvn);

/** Where a point goes at full weight, given its rest position (object space). */
using PointTarget = std::function<glm::dvec3(uint32_t point, const glm::dvec3& rest)>;
/** Sets up a mesh's PointTarget from its base mesh, world matrix and component selection; an empty one leaves the mesh alone. */
using PointTargetSetup = std::function<PointTarget(const geo::Mesh&, const glm::dmat4& world, ComponentMode kind, const components::Ids& ids)>;
/**
 * Moves the selected components' points toward their targets, blended by soft selection, with the selection's side of the
 * symmetry driving the other, as one undo step. Throws in object mode; returns how many meshes it changed.
 */
LILA_SCENE_API int deformSelectedComponents(const char* label, const PointTargetSetup& setup);
} // namespace lila::scene
