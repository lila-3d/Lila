#pragma once

#include "scene/Transform.h"

#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace lila::scene
{
class Graph;

enum class Space : uint8_t
{
    World,
    Object,
    Parent,
};

/** Transform edits shared by the manipulators and the move/rotate/scale commands. All record undo through Graph. */
namespace xformops
{
/** Drops ids whose ancestor is also in the list, so a selected parent and child move once. */
std::vector<NodeId> topLevel(Graph& g, const std::vector<NodeId>& ids);

/** World position of the rotate pivot. */
glm::dvec3 worldPivot(Graph& g, NodeId id);

/** Orthonormal world-space axes (columns) for a space. */
glm::dmat3 frame(Graph& g, NodeId id, Space space);

void moveBy(Graph& g, const std::vector<NodeId>& ids, const glm::dvec3& delta, Space space = Space::World);

/** Rotates each object about its own pivot. `rotation` is in `space` (world, or each object's own frame). */
void rotateBy(Graph& g, const std::vector<NodeId>& ids, const glm::dquat& rotation, Space space = Space::World);

/** The rotate channels rotateBy would give one object, before its locked channels are kept. */
glm::dvec3 rotatedEuler(Graph& g, NodeId id, const glm::dquat& rotation, Space space = Space::World);

/** Multiplies local scale; scaling is always along the object's own axes. */
void scaleBy(Graph& g, const std::vector<NodeId>& ids, const glm::dvec3& factor);

/** Moves the rotate and scale pivots to a world point without moving the object. */
void setWorldPivot(Graph& g, NodeId id, const glm::dvec3& world);

/** World axes (columns) of the pivot: the object's axes turned by its pivotOrient. */
glm::dmat3 pivotFrame(Graph& g, NodeId id);

/** Sets pivotOrient so the pivot's world axes become `world` (orthonormal columns), without moving the object. */
void setWorldPivotFrame(Graph& g, NodeId id, const glm::dmat3& world);

/** Zeroes pivots, their compensation and the pivot orientation, keeping the object where it is. */
void resetPivot(Graph& g, NodeId id);

/** Euler angles for a rotation that are closest to `hint`, so keys don't flip by 360. */
glm::dvec3 closestEuler(const glm::dmat3& rotation, RotateOrder order, const glm::dvec3& hint);
} // namespace xformops
} // namespace lila::scene
