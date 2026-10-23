#pragma once

#include "core/Export.h"

#include "scene/AnimLayer.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Non-destructive offsets for polish: an offset is an additive animation layer whose members are offset blends. Each
 * offset joint's blend holds one control value (inputB, never keyed: a rotation and a translation in the offset's
 * space) and turns it into the per-frame additive change itself, so posing on the layer at any frame moves that one
 * value and the whole take follows. A shared envelope curve eases it in and out over a range; the layer's weight and
 * mute work as on any layer, and several offsets stack.
 *
 * Spaces, about the joint's pivot: local (the joint's own axes), parent (its parent's axes), world, or a node's axes.
 * Translation offsets are in the parent's frame for local and parent.
 *
 * Compensation: with keepChildren, the joints just under the offset ones join the layer as compensating blends that
 * keep the world rotation they had without the layer (offset the torso, the head keeps looking where it looked).
 * Locked or IK-held hands and feet keep their contact because the IK solve runs after the layers, from the offset
 * parent: the chain re-solves under the offset.
 */
namespace lila::scene::offsets
{
inline constexpr const char* kRotationBlendType = "animBlendOffsetRotation";
inline constexpr const char* kVectorBlendType = "animBlendOffsetVector";

enum class Space : int64_t
{
    Local,
    Parent,
    World,
    Node,
};

LILA_SCENE_API const char* spaceName(Space s);
/** "local", "parent", "world"; anything else is a node name (Space::Node). */
LILA_SCENE_API Space spaceFromName(const std::string& s);

struct Range
{
    double start = 0.0; // seconds
    double end = 0.0;
    double ease = 0.0; // seconds of ease at each end
};

struct Options
{
    Space space = Space::Parent;
    NodeId spaceNode = kNoNode;
    /** Unset: constant over the whole take. */
    std::optional<Range> range;
    bool rotate = true;
    bool translate = true;
    /** Children of the offset joints keep their world rotation. Joints between offset ones join the offset too. */
    bool keepChildren = false;
    /** Layer name; empty names it for the parts ("Offset_head"). */
    std::string name;
};

/**
 * Creates the offset layer above the others over joints and makes it the active layer, so posing goes to it.
 * Returns the layer. Undoable. Throws std::invalid_argument for no joints or a space node that depends on them.
 */
LILA_SCENE_API NodeId create(Graph& g, const std::vector<NodeId>& joints, const Options& opts);

LILA_SCENE_API bool isOffsetBlend(const Node& n);
LILA_SCENE_API bool isOffsetLayer(Graph& g, NodeId layer);
/** Offset layers, bottom of the stack first. */
LILA_SCENE_API std::vector<NodeId> list(Graph& g);

struct Info
{
    NodeId layer = kNoNode;
    std::vector<NodeId> joints;      // the ones with a control value
    std::vector<NodeId> compensated; // children keeping their world rotation
    Space space = Space::Parent;
    NodeId spaceNode = kNoNode;
    std::optional<Range> range;
    double weight = 1.0;
    bool mute = false;
};
LILA_SCENE_API Info info(Graph& g, NodeId layer);

/** The offset blend on joint's rotate (or translate) in layer, kNoNode when there is none. */
LILA_SCENE_API NodeId blendOf(Graph& g, NodeId layer, NodeId joint, bool translate = false);
/** Sets a joint's control values directly (degrees, xyz in the rotate order; cm). Undoable. */
LILA_SCENE_API void setControl(Graph& g, NodeId layer, NodeId joint, std::optional<glm::dvec3> rotate, std::optional<glm::dvec3> translate);
/** Changes the envelope: a range eases in and out, none makes the offset constant over the take. Undoable. */
LILA_SCENE_API void setRange(Graph& g, NodeId layer, std::optional<Range> range);
/** Deletes the layer, its blends and envelope; the animation underneath is untouched. Undoable. */
LILA_SCENE_API void remove(Graph& g, NodeId layer);
/** Merges the offset down into the layer below (or the base animation), sampled every frame. Undoable. */
LILA_SCENE_API void bake(Graph& g, NodeId layer);

/** Offset blend types. Called by registerLayerTypes' caller. Idempotent. */
LILA_SCENE_API void registerTypes();
/** createOffset, listOffsets, editOffset, deleteOffset, bakeOffset. Idempotent. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::offsets
