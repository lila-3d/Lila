#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Relocating a scene: one rigid transform M applied so every world-space result becomes M times what it was. The roots
 * (or the roots of chosen hierarchies) take M through their own channels and keys; nodes that store world-space data
 * (bind matrices, world up vectors, world-space spaces) are rewritten by a handler registered for their type. A node
 * type with no entry here may hold world-space data nobody knows how to move, so relocateScene refuses to run while
 * one is in the scene, and names it.
 *
 * The display origin is separate and changes nothing in the scene: a point the grid, world axes and world readouts are
 * drawn relative to, saved with the scene.
 */
namespace lila::scene::relocate
{
/** The move a relocation makes, as handlers see it. */
struct Move
{
    Graph* graph = nullptr;
    /** World before to world after. Rigid: a rotation and a translation. */
    glm::dmat4 matrix{1.0};
    /** Every DAG node moves (a whole-scene relocation) rather than the chosen hierarchies only. */
    bool wholeScene = true;
    /** Whether a DAG node moves with the matrix. */
    std::function<bool(NodeId)> moves;
    /**
     * For a node that takes the move through its own channels (a root): the move in its parent's space, which its
     * translate and rotate go through. nullopt for every other node, which follows its parent. Handlers that keep
     * copies of a node's channels (a constraint's rest translate) rewrite them with it.
     */
    std::function<std::optional<glm::dmat4>(NodeId)> rootMove;

    glm::dmat3 rotation() const { return glm::dmat3(matrix); }
    glm::dvec3 point(const glm::dvec3& p) const { return glm::dvec3(matrix * glm::dvec4(p, 1.0)); }
    glm::dvec3 direction(const glm::dvec3& d) const { return rotation() * d; }
};

/** A node's world-space edits, made once every node has been planned and nothing refused. */
using Edit = std::function<void()>;

/**
 * How one node type's world-space data follows a relocation. plan runs before anything changes: it returns the edit
 * that rewrites the node's data through the graph (undoable), or an empty one when there is nothing to do, and throws
 * CommandError with the reason when the node can't follow this move. plan is only asked when every DAG node the node
 * links (through matrix, mesh, data and vector connections) moves; a node linking some that move and some that don't
 * is refused before plan is asked.
 */
struct Handler
{
    std::function<Edit(const Move& move, NodeId node)> plan;
    /**
     * The node makes world-space geometry (curve history: sweeps, lofts) that a mesh or curve takes as its shape. That
     * shape follows the moved inputs, so the mesh's own transform is not moved again; its children are moved instead.
     */
    bool worldOutput = false;
};

/**
 * Stored world-space values of a type, rewritten when they are not connected (a connection brings its own new value):
 * matrices taken at bind time become M * X, inverse ones X * M^-1, world points M * p, world directions turned by M's
 * rotation, and String attrs holding a JSON list of world [x, y, z] points have each point moved. A Vec3 keyed by curves
 * has its keys rewritten. Names may be grouped array children ("spaces.offset"); every element is rewritten.
 *
 * worldData are Data attrs holding world-space curves, moved when unconnected and refused when they hold any other kind
 * of data. channelCopies are checked rather than rewritten: String attrs naming a node whose channels the type keeps a
 * copy of (a joint's translate at setup), refused when that node is one of the roots taking the move.
 */
struct WorldAttrs
{
    std::vector<std::string> matrices;
    std::vector<std::string> inverseMatrices;
    std::vector<std::string> points;
    std::vector<std::string> directions;
    std::vector<std::string> pointLists;
    std::vector<std::string> channelCopies;
    std::vector<std::string> worldData;
    /** See Handler::worldOutput. */
    bool worldOutput = false;
};

/** Registers or replaces the handler for a node type (its registered name). Extensions' entries go when they unload. */
LILA_SCENE_API void registerHandler(const std::string& type, Handler handler);
/** A handler that rewrites the listed attrs. */
LILA_SCENE_API void registerWorldAttrs(const std::string& type, WorldAttrs attrs);
/**
 * Declares that a type holds no world-space data: its values are relative to its own transform, its parent or what
 * it is connected to, so moving the roots moves it correctly.
 */
LILA_SCENE_API void registerNeutral(const std::string& type);
/**
 * Whether the type (by registered name) has a handler or is neutral. Geometry-nodes modifier types, made per group, are
 * neutral without Object Info nodes; with them, every Object Info node must read its object relative to the owner.
 */
LILA_SCENE_API bool handles(const std::string& type);
/** Built-in handlers and neutral types; called by registerCommands. */
LILA_SCENE_API void registerBuiltins();

/** The display origin: where the grid, axes and world readouts put zero. The world origin when none is set. */
LILA_SCENE_API glm::dvec3 displayOrigin();
LILA_SCENE_API bool hasDisplayOrigin();
/** Undoable; saved with the scene. nullopt clears it. */
LILA_SCENE_API void setDisplayOrigin(const std::optional<glm::dvec3>& origin);

/** relocateScene, setDisplayOrigin. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::relocate
