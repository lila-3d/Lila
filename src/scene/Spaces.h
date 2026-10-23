#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Space switching for any transform. A node with spaces has a spaceSwitch node feeding its spaceFrames: one world frame
 * per space (the parent, the world, or another node, each with the offset it was added with). The node's keyable space
 * channel picks one, and evaluation puts that frame in parentMatrix, so translate, rotate and scale are the animator's
 * values in the active space and every tool that works from parentMatrix (gizmos, channel box, keys, poses, IK goals,
 * exporters sampling worldMatrix) works unchanged. Space 0 is always the parent.
 */
namespace lila::scene::spaces
{
inline constexpr const char* kSwitchType = "spaceSwitch";
inline constexpr const char* kFramesKind = "spaceFrames";
inline constexpr int kMaxSpaces = 16;

enum class Kind
{
    Parent,
    World,
    Node,
};

struct Space
{
    int index = 0;
    std::string name; // "parent", "world", or the target node's name ("" when the target was deleted)
    Kind kind = Kind::Parent;
    NodeId target = kNoNode;
};

/** The frames a spaceSwitch hands its transform: frames[i] is space i's world frame, unless parent[i] is set. */
struct LILA_SCENE_API Frames : Data
{
    std::vector<glm::dmat4> frames;
    std::vector<uint8_t> parent;

    const char* kind() const override { return kFramesKind; }
    json toJson() const override;
    size_t bytes() const override { return frames.size() * sizeof(glm::dmat4) + 64; }
};

class LILA_SCENE_API SwitchNode : public Node
{
public:
    enum Attr
    {
        kKinds, // one word per space: parent, world or node
        kFrames,
        kTarget0,
        kOffset0 = kTarget0 + kMaxSpaces,
        kCount = kOffset0 + kMaxSpaces,
    };

    void compute(const EvalContext& ctx) override;
};

/** The frame the node's local matrix lives in when its active space isn't the parent; null otherwise. For evaluation. */
LILA_SCENE_API const glm::dmat4* activeFrame(const Node& n);

LILA_SCENE_API bool isSwitch(const Node& n);
/** The node's spaceSwitch, or kNoNode. */
LILA_SCENE_API NodeId switchOf(const Graph& g, NodeId node);
inline bool hasSpaces(const Graph& g, NodeId node) { return switchOf(g, node) != kNoNode; }
/** The node's spaces, parent first; just the parent for a node without any. */
LILA_SCENE_API std::vector<Space> list(const Graph& g, NodeId node);
/** Space names for the channel box and menus, in index order. */
LILA_SCENE_API std::vector<std::string> names(const Graph& g, NodeId node);
/** The active space index as evaluated now. */
LILA_SCENE_API int active(Graph& g, NodeId node);
/** A space by "parent", "world", a target's name, or its index as text; -1 when the node has no such space. */
LILA_SCENE_API int find(const Graph& g, NodeId node, const std::string& space);

/**
 * Adds a space ("world", or a node name; "parent" is always there) and returns its index; one already there returns
 * its index. With maintainOffset the new space's frame equals the parent's at the current pose, so switching there
 * changes nothing until the target moves; without it the frame is the target's world matrix itself (the world origin
 * for "world"). Undoable. Throws std::invalid_argument for a target that would make a cycle, or past kMaxSpaces.
 */
LILA_SCENE_API int addSpace(Graph& g, NodeId node, const std::string& space, bool maintainOffset = true);

/**
 * Removes space index (not the parent). Times spent in it switch to the parent keeping the world pose, and higher
 * indices on the space channel shift down. Removing the last one deletes the spaceSwitch. Undoable.
 */
LILA_SCENE_API void removeSpace(Graph& g, NodeId node, int index);

struct SwitchOptions
{
    /** Seconds; the current time when unset. Ignored with a range. */
    std::optional<double> time;
    /** Switch over [first, second] seconds: every key in it (every frame with bake) keeps its world pose. */
    std::optional<std::pair<double, double>> range;
    bool bake = false;
    /** Re-key translate/rotate/scale so the world pose is unchanged. Off just sets or keys the space. */
    bool preserve = true;
    /** Key rather than set. Unset: key when the node's transform or space channels are animated. */
    std::optional<bool> key;
    /**
     * Also key the old space and pose a frame before the switch (and a frame after a range) where nothing is keyed,
     * so the frames outside the switch keep their pose while the curves interpolate into it.
     */
    bool hold = true;
};

/**
 * Switches node to space index, keeping its world pose: at the switch time it keys (or sets) the space channel stepped
 * and re-keys translate, rotate and scale in the new frame, on the active animation layer. Rotation curves are keyed
 * with the exact rotation. Undoable as one step when run inside a command.
 */
LILA_SCENE_API void switchSpace(Graph& g, NodeId node, int index, const SwitchOptions& opts = {});

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::spaces
