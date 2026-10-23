#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <functional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Takes: named clips of animation for a character (a root and everything under it) or the whole scene, each with its
 * own range, fps and loop flag. A take is a node; the active take's animation is simply what is connected. An
 * inactive take keeps its curves (and layer blend chains) disconnected, with the connections they belong on recorded
 * on the take node by plug name, so switching takes is connect/disconnect and undoes like any other edit.
 *
 * Takes with the same root are alternatives: activating one parks whichever of them is active. A whole-scene take
 * conflicts with every other take.
 */
namespace lila::scene::anim
{
class LILA_SCENE_API TakeNode : public Node
{
public:
    enum Attr
    {
        kRoot,
        kStart,
        kEnd,
        kFps,
        kLoop,
        kActive,
        kOrder,
        kParked,
        kFolder,
    };
};

LILA_SCENE_API bool isTake(const Node& n);
/** Take nodes in creation order. */
LILA_SCENE_API std::vector<NodeId> takes(Graph& g);
/** Active takes; at most one per root. */
LILA_SCENE_API std::vector<NodeId> activeTakes(Graph& g);

struct TakeSettings
{
    std::string root; // empty: the whole scene
    double start = 0.0, end = 4.0, fps = 30.0; // seconds
    bool loop = false;
};

/**
 * Makes a take. The first take of a root (or of the scene) adopts the animation that is there now and is active;
 * later ones start empty and inactive. Undoable.
 */
LILA_SCENE_API NodeId createTake(Graph& g, const std::string& name, const TakeSettings& s);
/** Parks the conflicting active takes, connects this one's animation and applies its range and fps. Undoable. */
LILA_SCENE_API void setActiveTake(Graph& g, NodeId take);
/** Deletes the take and its animation. Undoable. */
LILA_SCENE_API void deleteTake(Graph& g, NodeId take);
/** A copy of the take's animation as a new, inactive take. */
LILA_SCENE_API NodeId duplicateTake(Graph& g, NodeId take, const std::string& name);

/** (destination, source) pairs of the take's animation: live for an active take, parked otherwise. */
LILA_SCENE_API std::vector<std::pair<PlugRef, PlugRef>> takeConnections(Graph& g, NodeId take);
/** Every curve node in the take, through layer blends too. */
LILA_SCENE_API std::vector<NodeId> takeCurves(Graph& g, NodeId take);

/**
 * Runs fn with the take active, then puts back whatever was active, recording no undo. For exporters that sample
 * each take in turn.
 */
LILA_SCENE_API void withTake(Graph& g, NodeId take, const std::function<void()>& fn);

/** Take names matching a selector: "all", one name, or a list of names; restricted to takes under or above root. */
LILA_SCENE_API std::vector<NodeId> selectTakes(Graph& g, const nlohmann::json& selector, NodeId root = kNoNode);

LILA_SCENE_API void registerTakeTypes();
LILA_SCENE_API void registerTakeCommands();
} // namespace lila::scene::anim
