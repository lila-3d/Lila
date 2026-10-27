#pragma once

#include "core/Export.h"

#include "scene/Node.h"

namespace lila::geo
{
class Mesh;
struct MeshRemap;
}

namespace lila::scene
{
class Graph;

namespace splitRingsModifier
{
/** A direct edit compacted the base mesh: the components of a splitRingsModifier first in the stack follow to the new ids. */
LILA_SCENE_API void baseRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap, const geo::Mesh& base);

LILA_SCENE_API void registerTypes();
} // namespace splitRingsModifier
} // namespace lila::scene
