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
/** The running command's new nodes keep the component mode instead of landing in object mode. */
LILA_SCENE_API void keepModeAfterCreate();
LILA_SCENE_API const char* modeName(ComponentMode m);
LILA_SCENE_API ComponentMode modeFromName(const std::string& name); // throws std::invalid_argument

LILA_SCENE_API const std::map<NodeId, Ids>& selected(ComponentMode kind);
LILA_SCENE_API const Ids& selected(NodeId node, ComponentMode kind);
/** The selection of a kind in pick order, each component where it was last added; ones with no known pick follow in id order. */
LILA_SCENE_API std::vector<std::pair<NodeId, uint32_t>> ordered(ComponentMode kind);
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
/** ids with their mirror images added when the mesh node's symmetry is on; ids unchanged otherwise. */
LILA_SCENE_API Ids withSymmetry(NodeId node, ComponentMode kind, const Ids& ids);
/**
 * The symmetry wedge (side) the ids last given to withSymmetry on this node mostly sat in, -1 when none. A selection with
 * as much on every side drives from it, so the side clicked leads.
 */
LILA_SCENE_API int pickedWedge(NodeId node);
/**
 * UV vertices picked in the UV Editor: corner ids of the node's cage, whole uv vertices, empty when the node's vertex
 * selection did not come from there. While set, the vertex selection holds exactly their points, so viewport tools see
 * points as usual and uv tools move just these corners. Any other change to the node's vertex selection clears them.
 */
LILA_SCENE_API const Ids& uvCorners(NodeId node);
LILA_SCENE_API const std::map<NodeId, Ids>& uvCorners();
/**
 * Changes the node's picked uv corners and sets its vertex selection to their points. A node whose vertex selection
 * came from elsewhere starts from every corner of its selected points. Replace clears the other nodes' vertices.
 */
LILA_SCENE_API void selectUvCorners(NodeId node, const Ids& corners, Op op = Op::Replace);
/** uvCorners as face-vertex rows (the rows of Python's Node.uvs_array), sorted. */
LILA_SCENE_API Ids uvRows(NodeId node);
/** Corners of the node's cage as face-vertex rows, sorted; dead corners dropped. */
LILA_SCENE_API Ids uvRows(NodeId node, const Ids& corners);

/** Replaces everything at once, as one undo step. uvCorners must lie on the given points (see uvCorners). */
LILA_SCENE_API void setAll(std::map<NodeId, Ids> points, std::map<NodeId, Ids> edges, std::map<NodeId, Ids> faces,
                           std::map<NodeId, Ids> uvCorners = {});

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
    /** Optional: whether soft selection may pull this component along; without it every component may be. */
    std::function<bool(uint32_t)> soft;
};
LILA_SCENE_API void registerPointProvider(PointProvider p);
/** The provider for the node's type or a base type; null for meshes and anything else. */
LILA_SCENE_API const PointProvider* pointProvider(const Node& n);
/** True when a selected node has points of its own (a curve), so component modes edit it rather than every mesh. */
LILA_SCENE_API bool selectionHasPointNodes(const Graph& g);

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
/** softWeights for loose world-space points with no surface between them (a curve's points), by straight distance. */
LILA_SCENE_API std::vector<float> softWeights(const std::vector<glm::dvec3>& worldPoints, const std::vector<char>& selected);

/** World-space centre of the selected components' points. */
LILA_SCENE_API glm::dvec3 selectionCenter();
/**
 * selectionCenter, except that with symmetry a selection clear of the mirror plane (a pair of legs) gives the driving
 * side's own centre, where it turns and scales in place.
 */
LILA_SCENE_API glm::dvec3 symmetricSelectionCenter();
/**
 * ids cut to the side that drives a symmetric edit when the mesh's symmetry is on and they lie clear of the mirror plane
 * (a pair of legs); ids otherwise. What gizmos centre and orient on. mesh is the one the ids index, the cage when null.
 */
LILA_SCENE_API components::Ids drivingSide(NodeId node, ComponentMode kind, const components::Ids& ids, MeshPtr mesh = nullptr);
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
/**
 * World-space average normal of the selected components (their drivingSide), for orienting the manipulator; +Y when there
 * is none.
 */
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
 * symmetry driving the other, as one undo step. In object mode it throws, or with allowWhole moves every point of the
 * selected meshes. Returns how many meshes it changed.
 */
LILA_SCENE_API int deformSelectedComponents(const char* label, const PointTargetSetup& setup, bool allowWhole = false);
} // namespace lila::scene
