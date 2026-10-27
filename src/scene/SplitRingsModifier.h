#pragma once

#include "core/Export.h"

#include "scene/Components.h"
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

LILA_SCENE_API bool isSplitRings(const Node& n);
/** Edit Selection is in progress: the modifier shows its input with its components selected. */
LILA_SCENE_API bool editing(const Node& modifier);

/** The edges or faces a splitRingsModifier splits, found on its current input. */
struct Selection
{
    ComponentMode kind = ComponentMode::Face;
    components::Ids ids;
    /** Nothing stored: it splits every face. */
    bool whole = true;
    int lost = 0;
};
LILA_SCENE_API Selection selection(Graph& g, NodeId modifier);

LILA_SCENE_API void registerTypes();
/** splitRingsEditSelection, splitRingsApplySelection, splitRingsCancelSelection. */
LILA_SCENE_API void registerCommands();
} // namespace splitRingsModifier
} // namespace lila::scene
