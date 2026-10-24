#pragma once

#include "core/Export.h"

#include "scene/Components.h"

#include <functional>
#include <map>
#include <unordered_set>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::scene
{
class Graph;
}

/**
 * Isolate Select, one set per viewport: the objects a view shows, and for meshes isolated by their components, the
 * faces of them it shows. Display only: the scene is untouched. Face masks hold 1 per face that shows.
 */
namespace lila::scene::isolation
{
using components::Ids;

struct LILA_SCENE_API Set
{
    /** Faces kept on a partly shown mesh: sorted ids, and the face capacity when they were taken. Faces at or past it were made since and show too. */
    struct Faces
    {
        Ids ids;
        uint32_t capacity = 0;
    };
    /** Every node the view shows, whole or in part, plus the parents of isolated nodes for context. Empty when not isolating. */
    std::unordered_set<NodeId> nodes;
    std::map<NodeId, Faces> partial;
    /** New and unique across the process on every change, so caches can key on it. */
    uint64_t version = 0;

    bool active() const { return !nodes.empty(); }
    /** Whether the node shows at all: always when not isolating. */
    bool shows(NodeId id) const { return nodes.empty() || nodes.contains(id); }
    bool isPartial(NodeId id) const { return partial.contains(id); }
    /** The face mask for the node's mesh, or empty when every face shows. */
    std::vector<uint8_t> shownFaces(NodeId id, const geo::Mesh& mesh) const;
    void clear();
    void touch();
};

/** Faces with any corner on the components: faces as they are, an edge's faces, a point's faces. Sorted. */
LILA_SCENE_API Ids facesOf(const geo::Mesh& mesh, ComponentMode kind, const Ids& ids);

/**
 * Isolates the selection: meshes with components selected in the current mode show only the faces those touch;
 * other selected objects show whole, with their children. Parents of either stay for context.
 */
LILA_SCENE_API Set fromSelection(Graph& g);
/** Adds the selection the way fromSelection takes it. A mesh shown whole stays whole. */
LILA_SCENE_API void addSelection(Set& set, Graph& g);
/** Removes the selection: selected components' faces, or selected objects with their children. */
LILA_SCENE_API void removeSelection(Set& set, Graph& g);

/** {"nodes": [names], "faces": {name: {"ids": [...], "capacity": n}}}, or null when not isolating. */
LILA_SCENE_API nlohmann::json toJson(const Set& set, const Graph& g);
LILA_SCENE_API Set fromJson(const nlohmann::json& j, const Graph& g);

/** Per-kind masks derived from a face mask: an edge or point shows when any face on it does. Empty masks mean all show. */
struct LILA_SCENE_API Visibility
{
    std::vector<uint8_t> faces, edges, points;

    bool all() const { return faces.empty(); }
    bool face(uint32_t f) const { return faces.empty() || (f < faces.size() && faces[f]); }
    bool edge(uint32_t e) const { return edges.empty() || (e < edges.size() && edges[e]); }
    bool point(uint32_t p) const { return points.empty() || (p < points.size() && points[p]); }
    bool shows(ComponentMode kind, uint32_t id) const;
};
LILA_SCENE_API Visibility visibility(const geo::Mesh& mesh, std::vector<uint8_t> shownFaces);

/** The ids that show; all of them when the mask is empty. */
LILA_SCENE_API Ids visibleOnly(const Visibility& vis, ComponentMode kind, Ids ids);

/**
 * The run of an ordered chain (a loop, ring or face loop) through chain[seed] that shows, stopping at the first hidden
 * element either way; wrapping around the ends when the chain is closed. Empty when the seed itself is hidden.
 */
LILA_SCENE_API Ids clipChain(const Ids& chain, size_t seed, bool closed, const std::function<bool(uint32_t)>& shows);

/**
 * clipChain for what geo::edgeLoop, edgeRing (ring) or faceLoop (kind Face) returned through edge e, seeded at e or at a
 * shown face on it, closed when its ends meet on the mesh. The chain as is when everything shows.
 */
LILA_SCENE_API Ids clipLoop(const geo::Mesh& mesh, const Visibility& vis, const Ids& chain, uint32_t e, ComponentMode kind, bool ring);

/** Face mask of mesh after levels of smooth preview, from the base mesh's mask; empty if the mapping can't be made. */
LILA_SCENE_API std::vector<uint8_t> subdividedFaces(const geo::Mesh& base, int levels, const std::vector<uint8_t>& shownFaces, uint32_t subdividedCapacity);

/** Where component tools find the view they act in (the active viewport); unset in batch, where nothing is hidden. */
LILA_SCENE_API void setActiveProvider(std::function<const Set*()> provider);
LILA_SCENE_API const Set* active();
/** What the active view shows of the node's mesh (its cage, which component ids index). */
LILA_SCENE_API Visibility activeVisibility(NodeId id, const geo::Mesh& mesh);

/** Commands that make objects out of others (combine, extract) report it, so views isolating any source show the result. */
LILA_SCENE_API void inherit(const std::vector<NodeId>& made, const std::vector<NodeId>& from);
/** Called by inherit; returns an id for removeInheritListener. */
LILA_SCENE_API int addInheritListener(std::function<void(const std::vector<NodeId>& made, const std::vector<NodeId>& from)> fn);
LILA_SCENE_API void removeInheritListener(int id);
/** Adds made to the set when it shows any of from whole or in part. True when it changed. */
LILA_SCENE_API bool inheritInto(Set& set, const std::vector<NodeId>& made, const std::vector<NodeId>& from);
} // namespace lila::scene::isolation
