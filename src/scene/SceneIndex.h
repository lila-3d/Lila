#pragma once

#include "core/Export.h"

#include "scene/Graph.h"

#include <functional>
#include <string_view>
#include <vector>

/**
 * Whole-scene lookups the viewport makes every frame, cached so a frame that changed nothing pays nothing. Main thread
 * only, like the graph.
 */
namespace lila::scene::sceneIndex
{
/** Every node whose type isA(typeName), in id order. Valid until the graph's next structure() change. */
LILA_SCENE_API const std::vector<Node*>& ofType(const Graph& g, std::string_view typeName);

/**
 * Every node `keep` accepts, in id order, cached under `key` until the next structure() change. keep must depend only on
 * the node's type; put anything that changes its answers (a registry version) in the key.
 */
LILA_SCENE_API const std::vector<Node*>& matching(const Graph& g, std::string_view key, const std::function<bool(const Node&)>& keep);

/**
 * Changes whenever a node of the type is created, deleted or recomputes: everything they show is up to date until it
 * does. Cheap (one pass over the type's nodes), for keying caches finer than Graph::version(), which an edit anywhere
 * bumps. Call after evaluating.
 */
LILA_SCENE_API uint64_t passStamp(const Graph& g, std::string_view typeName);

/** effectivelyVisible() and arrays::arrayAbove() for every node at once, from one walk down the hierarchy. */
struct Hierarchy
{
    bool visible(NodeId id) const { return id < shown.size() ? shown[id] != 0 : true; }
    /** The nearest array above the node, kNoNode for none. */
    NodeId arrayAbove(NodeId id) const { return id < arrays.size() ? arrays[id] : kNoNode; }

    std::vector<uint8_t> shown;  // by node id
    std::vector<NodeId> arrays;  // by node id, empty when no node sits under an array
};

/**
 * Recomputed only when Graph::visibilityVersion(), the structure, the playback direction (layers hiding on playback) or a
 * node whose hiding evaluates (driven visibility, a display layer) changed; moving cameras and objects reuses the walk.
 */
LILA_SCENE_API const Hierarchy& hierarchy(const Graph& g, bool inherit);
} // namespace lila::scene::sceneIndex
