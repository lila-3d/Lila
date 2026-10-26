#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::scene
{
class Graph;

/**
 * Instances and references, built from connections rather than DAG multi-parenting:
 *
 *   instance:   Source.outMesh -> Instance.inMesh          (shares everything, no stack of its own)
 *   reference:  base -> [own modifiers] -> Reference.inMesh (shares the base under the source's stack)
 *
 * where base is the generator or meshSource at the bottom of the source's stack. Component edits on any member land on
 * the shared base (modifiers::editPlug and cage resolve an instance to its root), so editing one edits all.
 */
namespace instancing
{
enum class Kind : uint8_t
{
    None,      // shares nothing
    Source,    // owns data other meshes instance or reference
    Instance,  // inMesh driven by another mesh's outMesh
    Reference, // shares the source's base mesh, with a modifier stack of its own
};

LILA_SCENE_API const char* kindName(Kind k);

/** The mesh whose outMesh drives this mesh's inMesh directly, or kNoNode. */
LILA_SCENE_API NodeId instanceParent(const Graph& g, NodeId mesh);

/** The mesh owning the data: instanceParent followed to its end. The mesh itself when it isn't an instance. */
LILA_SCENE_API NodeId root(const Graph& g, NodeId mesh);

/** The plug feeding the bottom of a mesh's modifier stack (a generator's or meshSource's output), when there is one. */
LILA_SCENE_API std::optional<PlugRef> basePlug(const Graph& g, NodeId mesh);

/** Mesh nodes whose stack bottom is fed by `plug`, directly or through a chain of modifiers. */
LILA_SCENE_API std::vector<NodeId> meshesFedBy(const Graph& g, const PlugRef& plug);

/** Every mesh sharing data with `mesh` through instance links or a shared base, itself included, by id. */
LILA_SCENE_API std::vector<NodeId> members(const Graph& g, NodeId mesh);

LILA_SCENE_API Kind kind(const Graph& g, NodeId mesh);

/** What a member shares from: an instance's root, a reference's source, the mesh itself for a source, else kNoNode. */
LILA_SCENE_API NodeId source(const Graph& g, NodeId mesh);

/** Meshes driven directly by `mesh`'s outMesh. */
LILA_SCENE_API std::vector<NodeId> directInstances(const Graph& g, NodeId mesh);

/**
 * Makes `target` (a mesh node with no history of its own) an instance of `src`'s root: its inMesh is driven by the
 * root's outMesh.
 */
LILA_SCENE_API void connectInstance(Graph& g, NodeId src, NodeId target);

/**
 * Makes `target` (a mesh node with no history of its own) a reference of `src`'s root: the root's base moves into a
 * meshSource when it stores it itself, and `target` gets copies of the root's modifiers on top of that shared base.
 */
LILA_SCENE_API void connectReference(Graph& g, NodeId src, NodeId target);

/**
 * Copies the non-DAG history feeding srcMesh (generator, modifiers, base source) onto dstMesh, or its stored base
 * when it has none, so dstMesh gets a live stack of its own. `copies` maps history nodes already copied, so upstream
 * nodes shared within one operation are copied once. A DAG node feeding the stack is shared, not copied.
 */
LILA_SCENE_API void copyHistory(Graph& g, NodeId srcMesh, NodeId dstMesh, std::unordered_map<NodeId, NodeId>& copies);

/**
 * Moves `from`'s data (its inMesh connection or stored base) to `to`, one of its direct instances, and re-points its
 * other direct instances at `to`. `from` is left without a mesh.
 */
LILA_SCENE_API void promote(Graph& g, NodeId from, NodeId to);

/**
 * Gives a member its own data: an instance gets a copy of its root's history, a reference its own copy of the shared
 * base, and a source hands its data to its first instance and keeps a copy. False when it shares nothing.
 */
LILA_SCENE_API bool makeUnique(Graph& g, NodeId mesh);

/** Same topology (point and face order) and points within tolerance, object space. */
LILA_SCENE_API bool sameGeometry(const geo::Mesh& a, const geo::Mesh& b, double tolerance);

/** Drops target's own history and makes it an instance of src's root. */
LILA_SCENE_API void replaceWithInstance(Graph& g, NodeId src, NodeId target);

/** Before nodes are deleted: each doomed source hands its data to its first surviving instance. Graph's delete hook. */
LILA_SCENE_API void beforeDelete(Graph& g, const std::vector<NodeId>& doomed);

/** makeUnique, selectInstances, selectInstanceSource, convertToInstance, instanceInfo; installs beforeDelete as the delete hook. */
LILA_SCENE_API void registerCommands();
} // namespace instancing
} // namespace lila::scene
