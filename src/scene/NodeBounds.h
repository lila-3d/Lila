#pragma once

#include "core/Export.h"
#include "geo/Mesh.h"
#include "scene/Plug.h"

namespace lila::scene
{
class Graph;

/**
 * Object-space bounds of a node's own geometry: a mesh's points, or a point provider's components (a curve's points and
 * handles). Invalid for nodes with none, such as groups and locators. Reads evaluated values.
 */
LILA_SCENE_API geo::Bounds objectBounds(Graph& g, NodeId id);
/** objectBounds' eight corners through the node's world matrix. */
LILA_SCENE_API geo::Bounds worldBounds(Graph& g, NodeId id);
} // namespace lila::scene
