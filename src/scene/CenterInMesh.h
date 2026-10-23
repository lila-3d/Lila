#pragma once

#include "core/Export.h"

#include "geo/CrossSection.h"
#include "scene/Plug.h"

#include <optional>
#include <vector>

namespace lila::scene
{
class Graph;
class Node;

/**
 * Centring joints (and anything else with a `centerInMesh` bool and a `centerMeshes` string attr) in the cross-section
 * of the meshes around them. Each node is cut by the plane across its bone through its position, and moved to the
 * area centroid of the loop it sits in.
 */
namespace rig
{
/** Kind of the centerMeshes Data attr: meshes held by node id, so they follow renames; files store their names. */
inline constexpr const char* kCenterMeshListKind = "centerMeshList";
/** A centerMeshes value holding these meshes. */
LILA_SCENE_API DataPtr makeCenterMeshList(const Graph& g, const std::vector<NodeId>& meshes);
/** Lets scene files load centerMeshes, including the space-separated name strings older files hold. Idempotent. */
LILA_SCENE_API void registerCenterMeshListKind();

/** The node has a centerInMesh attr and it is on. */
LILA_SCENE_API bool centerEnabled(const Node& n);

/** Meshes in the node's centerMeshes attr; when that is empty, the meshes whose skinCluster it influences. */
LILA_SCENE_API std::vector<NodeId> centerMeshes(Graph& g, NodeId node);

/** Meshes as sliced for centring; owns the mesh data the spans point into. */
struct CenterSource
{
    std::vector<MeshPtr> keep;
    std::vector<geo::SectionMesh> meshes;
};
/** A skinned mesh is cut undeformed in its bind placement, so a joint never chases the skin it is moving. */
LILA_SCENE_API CenterSource centerSource(Graph& g, const std::vector<NodeId>& meshes);

/**
 * World direction across which a joint is cut: parent joint to joint, else joint to its first child joint. Other nodes
 * (rig template markers) use their parent or first child of the same type. Zero if neither.
 */
LILA_SCENE_API glm::dvec3 boneDirection(Graph& g, NodeId joint);

/** Where `point` centres in the node's meshes across `direction` (its bone direction when zero). Empty if nothing to cut. */
LILA_SCENE_API std::optional<glm::dvec3> centeredPoint(Graph& g, NodeId node, const glm::dvec3& point,
                                                       const glm::dvec3& direction = glm::dvec3(0.0));

/**
 * Moves the node to a world point through its translate, optionally leaving its children where they are. Records
 * undo unless undo is disabled.
 */
LILA_SCENE_API void moveNodeTo(Graph& g, NodeId node, const glm::dvec3& world, bool keepChildren);

/** Centres the node at its current position. False when there is nothing to centre in. */
LILA_SCENE_API bool centerNode(Graph& g, NodeId node, bool keepChildren = false, const glm::dvec3& direction = glm::dvec3(0.0));
} // namespace rig

/** centerJoint, centerMeshes, snapToComponentCenter. Called by registerRigCommands. */
LILA_SCENE_API void registerCenterCommands();
} // namespace lila::scene
