#include "scene/Isolation.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <atomic>

namespace lila::scene::isolation
{
namespace
{
std::function<const Set*()>& provider()
{
    static std::function<const Set*()> p;
    return p;
}

Ids sortedUnique(Ids v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

MeshPtr cageOf(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    return n && n->type().isA("mesh") ? modifiers::cage(g, id) : nullptr;
}

/** Folds faces made since the ids were taken into them, so a later capacity doesn't hide them. */
void settle(Set::Faces& f, const geo::Mesh& m)
{
    for (geo::Index i = f.capacity; i < m.faceCapacity(); ++i)
        if (m.faceValid(i))
            f.ids.push_back(i);
    f.capacity = uint32_t(std::max<geo::Index>(f.capacity, m.faceCapacity()));
}

void addParents(Set& set, const Graph& g, NodeId id)
{
    for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
        set.nodes.insert(p);
}

/** The selection as isolation sees it: meshes with components of the current mode, and objects without. */
struct Picked
{
    std::map<NodeId, Ids> faces;
    std::vector<NodeId> objects;
};

Picked picked(Graph& g)
{
    Picked out;
    ComponentMode mode = components::mode();
    if (mode != ComponentMode::Object)
        for (const auto& [node, ids] : components::selected(mode))
            if (MeshPtr m = cageOf(g, node); m && !ids.empty())
                if (Ids f = facesOf(*m, mode, ids); !f.empty())
                    out.faces[node] = std::move(f);
    for (NodeId id : g.selection())
        if (!out.faces.contains(id))
            out.objects.push_back(id);
    return out;
}
} // namespace

std::vector<uint8_t> Set::shownFaces(NodeId id, const geo::Mesh& mesh) const
{
    auto it = partial.find(id);
    if (it == partial.end())
        return {};
    std::vector<uint8_t> mask(mesh.faceCapacity(), 0);
    for (uint32_t f : it->second.ids)
        if (f < mask.size())
            mask[f] = 1;
    for (geo::Index f = it->second.capacity; f < mesh.faceCapacity(); ++f)
        mask[f] = 1;
    return mask;
}

void Set::clear()
{
    nodes.clear();
    partial.clear();
    touch();
}

void Set::touch()
{
    static std::atomic<uint64_t> next{1};
    version = next++;
}

bool Visibility::shows(ComponentMode kind, uint32_t id) const
{
    switch (kind)
    {
    case ComponentMode::Vertex: return point(id);
    case ComponentMode::Edge: return edge(id);
    case ComponentMode::Face: return face(id);
    default: return true;
    }
}

Ids facesOf(const geo::Mesh& m, ComponentMode kind, const Ids& ids)
{
    Ids out;
    for (uint32_t id : ids)
    {
        if (kind == ComponentMode::Face && id < m.faceCapacity() && m.faceValid(id))
            out.push_back(id);
        else if (kind == ComponentMode::Edge && id < m.edgeCapacity() && m.edgeValid(id))
            m.forEachEdgeCorner(id, [&](geo::Index c) { out.push_back(m.cornerFace(c)); });
        else if (kind == ComponentMode::Vertex && id < m.pointCapacity() && m.pointValid(id))
            for (geo::Index f : m.pointFaces(id))
                out.push_back(f);
    }
    return sortedUnique(std::move(out));
}

Set fromSelection(Graph& g)
{
    Set set;
    addSelection(set, g);
    return set;
}

void addSelection(Set& set, Graph& g)
{
    LILA_ZONE();

    Picked p = picked(g);
    for (auto& [node, faces] : p.faces)
    {
        MeshPtr m = cageOf(g, node);
        // Shown whole already: it stays whole.
        if (set.nodes.contains(node) && !set.partial.contains(node))
            continue;
        Set::Faces& kept = set.partial[node];
        if (!set.nodes.contains(node))
            kept.capacity = m->faceCapacity();
        else
            settle(kept, *m);
        kept.ids.insert(kept.ids.end(), faces.begin(), faces.end());
        kept.ids = sortedUnique(std::move(kept.ids));
        kept.capacity = m->faceCapacity();
        set.nodes.insert(node);
        addParents(set, g, node);
    }
    for (NodeId id : p.objects)
    {
        set.nodes.insert(id);
        set.partial.erase(id);
        for (NodeId d : g.descendants(id))
        {
            set.nodes.insert(d);
            set.partial.erase(d);
        }
        addParents(set, g, id);
    }
    set.touch();
}

void removeSelection(Set& set, Graph& g)
{
    LILA_ZONE();

    if (!set.active())
        return;
    Picked p = picked(g);
    for (auto& [node, faces] : p.faces)
    {
        if (!set.nodes.contains(node))
            continue;
        MeshPtr m = cageOf(g, node);
        const bool whole = !set.partial.contains(node);
        Set::Faces& kept = set.partial[node];
        if (whole)
        {
            // Shown whole until now: every face but these.
            kept.capacity = m->faceCapacity();
            for (geo::Index f = 0; f < m->faceCapacity(); ++f)
                if (m->faceValid(f))
                    kept.ids.push_back(f);
        }
        else
            settle(kept, *m);
        std::erase_if(kept.ids, [&](uint32_t f) { return std::binary_search(faces.begin(), faces.end(), f); });
        if (kept.ids.empty())
        {
            set.partial.erase(node);
            set.nodes.erase(node);
        }
    }
    for (NodeId id : p.objects)
    {
        set.nodes.erase(id);
        set.partial.erase(id);
        for (NodeId d : g.descendants(id))
        {
            set.nodes.erase(d);
            set.partial.erase(d);
        }
    }
    // Isolating nothing would show everything; keep an empty isolation showing nothing instead.
    if (set.nodes.empty())
        set.nodes.insert(kNoNode);
    set.touch();
}

nlohmann::json toJson(const Set& set, const Graph& g)
{
    if (!set.active())
        return nullptr;
    nlohmann::json names = nlohmann::json::array(), faces = nlohmann::json::object();
    for (NodeId id : set.nodes)
        if (const Node* n = g.node(id))
            names.push_back(n->path());
    for (const auto& [id, f] : set.partial)
        if (const Node* n = g.node(id))
            faces[n->path()] = {{"ids", f.ids}, {"capacity", f.capacity}};
    std::sort(names.begin(), names.end());
    return {{"nodes", names}, {"faces", faces}};
}

Set fromJson(const nlohmann::json& j, const Graph& g)
{
    // A name that has since become ambiguous restores nothing rather than failing the load.
    auto find = [&](const std::string& path) -> const Node* {
        try
        {
            return g.find(path);
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    };
    Set set;
    if (!j.is_object())
        return set;
    for (const auto& name : j.value("nodes", nlohmann::json::array()))
        if (name.is_string())
            if (const Node* n = find(name.get<std::string>()))
                set.nodes.insert(n->id());
    if (auto it = j.find("faces"); it != j.end() && it->is_object())
        for (const auto& [name, f] : it->items())
        {
            const Node* n = find(name);
            if (!n || !set.nodes.contains(n->id()) || !f.is_object())
                continue;
            Set::Faces kept;
            for (const auto& id : f.value("ids", nlohmann::json::array()))
                if (id.is_number_unsigned())
                    kept.ids.push_back(id.get<uint32_t>());
            kept.ids = sortedUnique(std::move(kept.ids));
            kept.capacity = f.value("capacity", 0u);
            set.partial[n->id()] = std::move(kept);
        }
    // Saved isolation whose nodes are all gone still isolates (shows nothing), as it did when saved.
    if (set.nodes.empty() && j.contains("nodes"))
        set.nodes.insert(kNoNode);
    set.touch();
    return set;
}

Visibility visibility(const geo::Mesh& m, std::vector<uint8_t> shownFaces)
{
    Visibility v;
    if (shownFaces.empty())
        return v;
    shownFaces.resize(m.faceCapacity(), 0);
    v.edges.assign(m.edgeCapacity(), 0);
    v.points.assign(m.pointCapacity(), 0);
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!shownFaces[f] || !m.faceValid(f))
            continue;
        m.forEachFaceCorner(f, [&](geo::Index c) {
            v.edges[m.cornerEdge(c)] = 1;
            v.points[m.cornerPoint(c)] = 1;
        });
    }
    v.faces = std::move(shownFaces);
    return v;
}

Ids visibleOnly(const Visibility& vis, ComponentMode kind, Ids ids)
{
    if (!vis.all())
        std::erase_if(ids, [&](uint32_t id) { return !vis.shows(kind, id); });
    return ids;
}

Ids clipChain(const Ids& chain, size_t seed, bool closed, const std::function<bool(uint32_t)>& shows)
{
    if (seed >= chain.size() || !shows(chain[seed]))
        return {};
    const size_t n = chain.size();
    size_t hi = seed;
    size_t count = 1;
    while (count < n && (closed || hi + 1 < n) && shows(chain[(hi + 1) % n]))
    {
        hi = (hi + 1) % n;
        ++count;
    }
    if (count == n)
        return chain;
    size_t lo = seed;
    while (count < n && (closed || lo > 0) && shows(chain[(lo + n - 1) % n]))
    {
        lo = (lo + n - 1) % n;
        ++count;
    }
    Ids out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i)
        out.push_back(chain[(lo + i) % n]);
    return out;
}

Ids clipLoop(const geo::Mesh& m, const Visibility& vis, const Ids& chain, uint32_t e, ComponentMode kind, bool ring)
{
    if (vis.all() || chain.empty())
        return chain;
    auto faceHasEdge = [&](geo::Index f, geo::Index x) {
        bool has = false;
        if (m.faceValid(f))
            m.forEachFaceCorner(f, [&](geo::Index c) { has = has || m.cornerEdge(c) == x; });
        return has;
    };
    auto edgesShareFace = [&](geo::Index a, geo::Index b) {
        bool shared = false;
        m.forEachEdgeCorner(a, [&](geo::Index c) { shared = shared || faceHasEdge(m.cornerFace(c), b); });
        return shared;
    };
    const uint32_t first = chain.front(), last = chain.back();
    bool closed = false;
    size_t seed = chain.size();
    if (kind == ComponentMode::Face)
    {
        for (size_t i = 0; i < chain.size() && seed == chain.size(); ++i)
            if (vis.face(chain[i]) && faceHasEdge(chain[i], e))
                seed = i;
        bool adjacent = false;
        if (chain.size() > 2)
            m.forEachFaceCorner(first, [&](geo::Index c) { adjacent = adjacent || faceHasEdge(last, m.cornerEdge(c)); });
        closed = adjacent;
        return clipChain(chain, seed, closed, [&](uint32_t f) { return vis.face(f); });
    }
    seed = size_t(std::find(chain.begin(), chain.end(), e) - chain.begin());
    if (chain.size() > 2)
        closed = ring ? edgesShareFace(first, last)
                      : m.edgePoint(first, 0) == m.edgePoint(last, 0) || m.edgePoint(first, 0) == m.edgePoint(last, 1) ||
                            m.edgePoint(first, 1) == m.edgePoint(last, 0) || m.edgePoint(first, 1) == m.edgePoint(last, 1);
    return clipChain(chain, seed, closed, [&](uint32_t x) { return vis.edge(x); });
}

std::vector<uint8_t> subdividedFaces(const geo::Mesh& base, int levels, const std::vector<uint8_t>& shownFaces, uint32_t subdividedCapacity)
{
    if (levels <= 0)
        return shownFaces;
    // Smoothing splits each live face, in id order, into one quad per corner, then every quad into four per level after.
    std::vector<uint8_t> out;
    out.reserve(subdividedCapacity);
    for (geo::Index f = 0; f < base.faceCapacity(); ++f)
        if (base.faceValid(f))
            out.insert(out.end(), base.faceSize(f), f < shownFaces.size() ? shownFaces[f] : uint8_t(1));
    for (int l = 1; l < levels; ++l)
    {
        std::vector<uint8_t> next;
        next.reserve(out.size() * 4);
        for (uint8_t s : out)
            next.insert(next.end(), 4, s);
        out = std::move(next);
    }
    if (out.size() != subdividedCapacity)
        return {};
    return out;
}

void setActiveProvider(std::function<const Set*()> p) { provider() = std::move(p); }

const Set* active()
{
    const Set* s = provider() ? provider()() : nullptr;
    return s && s->active() ? s : nullptr;
}

Visibility activeVisibility(NodeId id, const geo::Mesh& mesh)
{
    const Set* s = active();
    if (!s || !s->isPartial(id))
        return {};
    return visibility(mesh, s->shownFaces(id, mesh));
}
} // namespace lila::scene::isolation
