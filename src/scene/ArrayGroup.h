#pragma once

#include "core/Export.h"

#include "geo/ArrayTransform.h"
#include "scene/Duplicate.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <vector>

namespace lila::scene
{
/**
 * A live array, Cinema 4D's Cloner: its children are the sources, and it shows each of them repeated in a row, around a
 * circle or in a grid about its own pivot. The copies are generated, never nodes: they are drawn instanced from the
 * sources' meshes, pick as the array, export as shared geometry and Unreal placements, and are rebuilt from the
 * settings rather than saved. Editing a source changes every copy. bakeArray turns them into real objects.
 */
class LILA_SCENE_API ArrayGroup : public Transform
{
public:
    enum Attr
    {
        kMode = Transform::kCount,
        kCopies,
        kOffset,
        kCopyRotate,
        kCopyScale,
        kAxis,
        kAngle,
        kAngleMode,
        kRise,
        kCountY,
        kCountZ,
        kSpacing,
        kCount
    };
};

namespace arrays
{
/** Copies including the original, per array, and in all from one source: past these the rest are dropped. */
inline constexpr int kMaxCount = 10000;
inline constexpr int kMaxTotal = 100000;

LILA_SCENE_API bool isArray(const Node& n);

/** The array's sources: its transform children. */
LILA_SCENE_API std::vector<NodeId> sources(const Graph& g, NodeId array);

/** The layout the array's settings describe, in its object space, centred on its rotate pivot. */
LILA_SCENE_API geo::ArrayLayout layout(const Node& array);

/**
 * World-space transforms of every copy, the original (identity) first: copy k of a source with world matrix W sits at
 * result[k] * W. Reads evaluated values.
 */
LILA_SCENE_API std::vector<glm::dmat4> copyTransforms(const Node& array);

/** One generated copy of a node. */
struct Copy
{
    /** Left-multiplies the node's world matrix to place the copy. */
    glm::dmat4 transform{1.0};
    /** The array the copy picks as: the outermost one whose copy, not original, it is. */
    NodeId array = kNoNode;
};

/**
 * Every generated copy of a node from the arrays above it, the node itself left out; nested arrays multiply. Empty for a
 * node no array repeats. Reads evaluated values.
 */
LILA_SCENE_API std::vector<Copy> copiesOf(const Graph& g, NodeId id);

/** The nearest array above the node, or kNoNode. */
LILA_SCENE_API NodeId arrayAbove(const Graph& g, NodeId id);

/**
 * Replaces the array with a plain group holding its sources and a real object per copy (instances, copies or
 * references of the sources, per mode), placed where the copies were. Returns the group. Undoable.
 */
LILA_SCENE_API NodeId bake(Graph& g, NodeId array, duplicate::Mode mode);

LILA_SCENE_API void registerTypes();
/** array, bakeArray, arrayInfo. */
LILA_SCENE_API void registerCommands();
} // namespace arrays
} // namespace lila::scene
