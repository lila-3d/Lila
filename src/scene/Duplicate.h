#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lila::scene
{
class Graph;
class Node;

/** Object duplication shared by duplicate, Duplicate with Transform, instance, the gizmo's clone drag and the array tools. */
namespace duplicate
{
enum class Mode : uint8_t
{
    Copy,      // a mesh gets its own live copy of the stack
    Instance,  // a mesh shares its source's data (instancing::connectInstance)
    Reference, // a mesh shares its source's base and gets its own modifiers (instancing::connectReference)
};

LILA_SCENE_API const char* modeName(Mode m);
/** "copy", "instance" or "reference". Throws CommandError for anything else. */
LILA_SCENE_API Mode parseMode(std::string_view s);

/** Copies every storable input value that isn't driven by a connection, then the locks, as Maya's duplicate keeps them. */
LILA_SCENE_API void copyValues(Graph& g, const Node& from, Node& to);

/**
 * Copies `src` and its children under `parent` and returns the copy. Meshes follow `mode`; other shapes copy their
 * values. `name` names the top copy (made unique), empty keeps the source's. `copies` maps history nodes already
 * copied in this operation, so shared upstream nodes are copied once.
 */
LILA_SCENE_API NodeId copyTree(Graph& g, NodeId src, NodeId parent, Mode mode, std::string_view name,
                               std::unordered_map<NodeId, NodeId>& copies);

/**
 * Runs after a duplicate with every node it copied, source to copy, so a node type can rewire what copying values
 * alone misses (connections between copied nodes, a container to resync). Once per copy for a run of copies.
 */
using Fixup = std::function<void(Graph& g, const std::unordered_map<NodeId, NodeId>& copied)>;
LILA_SCENE_API void addFixup(Fixup fixup);

/** A source and its newest copy, as Duplicate with Transform repeats them. */
struct Pair
{
    NodeId source = kNoNode;
    NodeId copy = kNoNode;
    Mode mode = Mode::Copy;
};

/** The pairs the last duplicate made, one per top-level object; dropped as soon as either node of any pair is deleted. */
LILA_SCENE_API const std::vector<Pair>& lastPairs();
LILA_SCENE_API void setLastPairs(std::vector<Pair> pairs);
LILA_SCENE_API void clearLastPairs();

/** duplicate, duplicateWithTransform, instance. */
LILA_SCENE_API void registerCommands();
} // namespace duplicate
} // namespace lila::scene
