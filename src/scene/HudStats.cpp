#include "scene/HudStats.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/MeshNodes.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lila::scene::hud
{
namespace
{
struct MeshEntry
{
    std::weak_ptr<const geo::Mesh> mesh;
    std::string uvSet;
    PolyCount count;
    bool carried = false; // uvs taken from the previous snapshot
    std::chrono::steady_clock::time_point since;
};

struct ComponentEntry
{
    std::weak_ptr<const geo::Mesh> mesh;
    uint64_t generation = 0;
    ComponentMode mode = ComponentMode::Object;
    PolyCount count;
};

constexpr std::chrono::milliseconds kSettle(150);

std::unordered_map<NodeId, MeshEntry> gMeshes;
std::unordered_map<NodeId, ComponentEntry> gComponents;
uint64_t gComponentGeneration = 1;

uint64_t componentGeneration()
{
    static int listener = components::addListener([] { ++gComponentGeneration; });
    (void)listener;
    return gComponentGeneration;
}

/** Everything but uvs is O(1): a face of n corners is n - 2 triangles. */
PolyCount elementCounts(const geo::Mesh& m)
{
    PolyCount c;
    c.verts = m.numPoints();
    c.edges = m.numEdges();
    c.faces = m.numFaces();
    c.tris = uint64_t(m.numCorners()) - 2 * uint64_t(m.numFaces());
    return c;
}

bool sameElements(const PolyCount& a, const PolyCount& b)
{
    return a.verts == b.verts && a.edges == b.edges && a.faces == b.faces && a.tris == b.tris;
}

const PolyCount& cachedCount(NodeId id, const MeshPtr& mesh, std::string_view uvSet, bool& pending)
{
    MeshEntry& e = gMeshes[id];
    std::shared_ptr<const geo::Mesh> had = e.mesh.lock();
    if (e.uvSet != uvSet)
        had.reset();
    auto now = std::chrono::steady_clock::now();
    // Every view asks each frame, so "settled" has to mean unchanged for a while rather than asked twice.
    if (had == mesh && (!e.carried || now - e.since < kSettle))
    {
        pending = pending || e.carried;
        return e.count;
    }
    PolyCount next = elementCounts(*mesh);
    // A new snapshot of the same shape is most likely a drag in progress: keep its uvs until the mesh stops changing.
    if (had != mesh && had && sameElements(next, e.count))
    {
        next.uvs = e.count.uvs;
        e = {mesh, std::string(uvSet), next, true, now};
        pending = true;
        return e.count;
    }
    next.uvs = countUvs(*mesh, uvSet);
    e = {mesh, std::string(uvSet), next, false, now};
    return e.count;
}

PolyCount countSelected(const geo::Mesh& mesh, ComponentMode mode, const components::Ids& ids)
{
    PolyCount c;
    for (uint32_t i : ids)
        if (mode == ComponentMode::Vertex)
            c.verts += mesh.pointValid(i) ? 1 : 0;
        else if (mode == ComponentMode::Edge)
            c.edges += mesh.edgeValid(i) ? 1 : 0;
        else if (mesh.faceValid(i))
        {
            ++c.faces;
            c.tris += mesh.faceSize(i) - 2;
        }
    return c;
}

const PolyCount& cachedSelected(NodeId id, const MeshPtr& mesh, ComponentMode mode, const components::Ids& ids)
{
    ComponentEntry& e = gComponents[id];
    uint64_t generation = componentGeneration();
    if (e.mesh.lock() != mesh || e.generation != generation || e.mode != mode)
        e = {mesh, generation, mode, countSelected(*mesh, mode, ids)};
    return e.count;
}

template <class Map> void dropUnseen(Map& map, const std::unordered_set<NodeId>& seen)
{
    std::erase_if(map, [&](const auto& kv) { return !seen.contains(kv.first); });
}
} // namespace

PolyCount& PolyCount::operator+=(const PolyCount& o)
{
    verts += o.verts;
    edges += o.edges;
    faces += o.faces;
    tris += o.tris;
    uvs += o.uvs;
    return *this;
}

uint64_t countUvs(const geo::Mesh& mesh, std::string_view uvSet)
{
    LILA_ZONE();
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    if (uv.empty())
        return 0;
    uint64_t total = 0;
    std::vector<glm::vec2> seen;
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
    {
        if (!mesh.pointValid(p))
            continue;
        seen.clear();
        mesh.forEachPointCorner(p, [&](geo::Index c) {
            if (std::find(seen.begin(), seen.end(), uv[c]) == seen.end())
                seen.push_back(uv[c]);
        });
        total += seen.size();
    }
    return total;
}

PolyCount countMesh(const geo::Mesh& mesh, std::string_view uvSet)
{
    PolyCount c = elementCounts(mesh);
    c.uvs = countUvs(mesh, uvSet);
    return c;
}

bool visibleInHierarchy(const Graph& g, NodeId id) { return effectivelyVisible(g, id); }

PolyStats polyStats(Graph& g, std::string_view uvSet, const std::function<bool(NodeId)>& shown)
{
    LILA_ZONE();
    PolyStats out;
    std::unordered_set<NodeId> selected;
    for (NodeId id : g.selection())
    {
        selected.insert(id);
        for (NodeId d : g.descendants(id))
            selected.insert(d);
    }
    ComponentMode mode = components::mode();
    std::unordered_set<NodeId> seen;
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n->type().isA("mesh") || n->type().isA("lattice"))
            continue;
        // Hidden meshes keep their cache entries: another view, or the next frame, may count them.
        seen.insert(id);
        const MeshPtr& mesh = n->as<MeshPtr>(MeshNode::kOutMesh);
        if (!mesh || !visibleInHierarchy(g, id) || (shown && !shown(id)))
            continue;
        const PolyCount& c = cachedCount(id, mesh, uvSet, out.pending);
        out.scene += c;
        if (selected.contains(id))
            out.objects += c;
        if (mode == ComponentMode::Object)
            continue;
        if (const components::Ids& ids = components::selected(id, mode); !ids.empty())
            out.components += cachedSelected(id, mesh, mode, ids);
    }
    dropUnseen(gMeshes, seen);
    dropUnseen(gComponents, seen);
    return out;
}
} // namespace lila::scene::hud
