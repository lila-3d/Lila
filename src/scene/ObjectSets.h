#pragma once

#include "core/Export.h"

#include "scene/Components.h"
#include "scene/Node.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::geo
{
struct MeshRemap;
}

namespace lila::scene
{
class Graph;

/**
 * Maya's object sets. An `objectSet` node (non-DAG) holds a list of members: whole nodes, or vertices, edges or faces
 * of a mesh by component id (the ids of the mesh's outMesh, which direct edits keep up to date). quickSelect marks it
 * as a quick select set, listed under Edit > Quick Select Sets for one-click selection. Game export and the Unreal
 * link take a set as one exportable unit.
 *
 * Members are held by node id, so they follow renames. A deleted member drops out of every query and is left out when
 * the scene is saved; undoing the delete brings it back. Referenced members are rebound by name when their reference
 * reloads. In files, members are written by name, relative to the set's namespace when the set comes from a reference.
 */
class ObjectSet : public Node
{
public:
    enum Attr
    {
        kQuickSelect,
        kMembers,
        kCount
    };
};

namespace objectSets
{
inline constexpr const char* kTypeName = "objectSet";
inline constexpr const char* kMembersKind = "objectSetMembers";

/** One member: a whole node (kind Object) or some of a mesh's components (ids sorted, unique). */
struct Member
{
    NodeId node = kNoNode;
    ComponentMode kind = ComponentMode::Object;
    components::Ids ids;

    bool operator==(const Member&) const = default;
};

LILA_SCENE_API bool isSet(const Node& n);
/** Every set in creation order; quickSelect filters to quick select sets (true) or plain ones (false). */
LILA_SCENE_API std::vector<Node*> all(const Graph& g, std::optional<bool> quickSelect = {});
/** Live members in the order they joined, one entry per node and kind. */
LILA_SCENE_API std::vector<Member> members(const Graph& g, NodeId set);
/** Distinct nodes the set touches: object members and the meshes its components belong to. */
LILA_SCENE_API std::vector<NodeId> nodesOf(const Graph& g, NodeId set);
/** Sets that have the node as a member, whole or by its components. */
LILA_SCENE_API std::vector<NodeId> setsOf(const Graph& g, NodeId node);

/** The current mode's selected components, or the selected objects when there are none. Sets are left out. */
LILA_SCENE_API std::vector<Member> fromSelection(const Graph& g);
/** Maya-style member strings: "pCube1", "pCube1.vtx[3]", "pCube1.e[0:7]", "pCube1.f[2]". Throws std::invalid_argument. */
LILA_SCENE_API std::vector<Member> parse(const Graph& g, const std::vector<std::string>& items);
/** The members as parse() reads them, consecutive ids folded into ranges. */
LILA_SCENE_API std::vector<std::string> format(const Graph& g, const std::vector<Member>& members);

/** All undoable. */
LILA_SCENE_API Node* create(Graph& g, std::string_view name, const std::vector<Member>& members, bool quickSelect = false);
LILA_SCENE_API void add(Graph& g, NodeId set, const std::vector<Member>& members);
/** An object member removes the node with all its components; a component member removes just those ids. */
LILA_SCENE_API void remove(Graph& g, NodeId set, const std::vector<Member>& members);
/** Selects the members of the sets: their nodes as objects, their components in component mode. */
LILA_SCENE_API void select(Graph& g, const std::vector<NodeId>& sets, bool add = false);

/** A direct edit compacted a mesh: component members follow their elements to the new ids. Records undo. */
LILA_SCENE_API void meshRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap);

/** { name, quickSelect, members (as format()) }. */
LILA_SCENE_API json info(const Graph& g, NodeId set);

LILA_SCENE_API void registerTypes();
/** createSet, addToSet, removeFromSet, selectSetMembers, deleteSet, listSets, setMembers. */
LILA_SCENE_API void registerCommands();
} // namespace objectSets
} // namespace lila::scene
