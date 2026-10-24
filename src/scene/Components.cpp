#include "scene/Components.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/ComponentPaths.h"
#include "geo/Mesh.h"
#include "geo/Topology.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace lila::scene::components
{
namespace
{
struct State
{
    ComponentMode mode = ComponentMode::Object;
    std::map<NodeId, Ids> sets[3]; // vertex, edge, face
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
    int graphListener = 0;
    bool commandRunning = false;
    std::set<NodeId> created; // by the top-level command now running
};

State& st()
{
    static State s;
    return s;
}

int slot(ComponentMode kind)
{
    switch (kind)
    {
    case ComponentMode::Vertex: return 0;
    case ComponentMode::Edge: return 1;
    case ComponentMode::Face: return 2;
    default: throw std::invalid_argument("object mode has no components");
    }
}

void notify()
{
    auto copy = st().listeners;
    for (auto& [id, fn] : copy)
        fn();
}

struct Snapshot
{
    std::map<NodeId, Ids> sets[3];

    size_t bytes() const
    {
        size_t n = 0;
        for (const auto& set : sets)
            for (const auto& [id, ids] : set)
                n += 64 + ids.capacity() * sizeof(uint32_t);
        return n;
    }
};

Snapshot snapshot()
{
    Snapshot s;
    for (int i = 0; i < 3; ++i)
        s.sets[i] = st().sets[i];
    return s;
}

void restore(const Snapshot& s)
{
    for (int i = 0; i < 3; ++i)
        st().sets[i] = s.sets[i];
    notify();
}

struct SelectionAction final : UndoAction
{
    Snapshot before, after;
    void undo() override { restore(before); }
    void redo() override { restore(after); }
    bool selectionOnly() const override { return true; }
    size_t bytes() const override { return sizeof(*this) + before.bytes() + after.bytes(); }
};

/** A mode switch made inside an undoable command, so undoing it lands back in the mode it started from. */
struct ModeAction final : UndoAction
{
    ComponentMode before, after;
    ModeAction(ComponentMode b, ComponentMode a) : before(b), after(a) {}
    void undo() override { setMode(before); }
    void redo() override { setMode(after); }
    bool selectionOnly() const override { return true; }
};

void record(Snapshot before)
{
    auto a = std::make_unique<SelectionAction>();
    a->before = std::move(before);
    a->after = snapshot();
    UndoStack::get().record(std::move(a));
}

void ensureGraphListener()
{
    if (st().graphListener)
        return;
    // Deleted nodes and new scenes drop their components.
    st().graphListener = Graph::current().addListener([](Change c, NodeId id) {
        if (c == Change::Cleared)
        {
            for (auto& s : st().sets)
                s.clear();
            notify();
        }
        else if (c == Change::Created && st().commandRunning)
            st().created.insert(id);
        else if (c == Change::Deleted)
        {
            bool any = false;
            for (auto& s : st().sets)
                any |= s.erase(id) > 0;
            if (any)
                notify();
        }
    }, true);
}

/** Whatever command creates something and selects it, the result lands in object mode so the new node shows as selected. */
void watchCreations()
{
    ops::addExecuteListener([](const ExecuteEvent& e) {
        State& s = st();
        if (!e.done)
        {
            ensureGraphListener();
            s.created.clear();
            s.commandRunning = true;
            return;
        }
        s.commandRunning = false;
        if (e.ok && s.mode != ComponentMode::Object)
            for (NodeId id : Graph::current().selection())
                if (s.created.contains(id))
                {
                    setMode(ComponentMode::Object);
                    break;
                }
        s.created.clear();
    });
}

MeshPtr meshOf(NodeId node)
{
    Graph& g = Graph::current();
    Node* n = g.node(node);
    if (!n || !n->type().isA("mesh"))
        return nullptr;
    // Components index the cage: the base mesh under any modifiers.
    return modifiers::cage(g, node);
}

Ids sortedUnique(Ids v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
} // namespace

ComponentMode mode() { return st().mode; }

void setMode(ComponentMode m)
{
    ensureGraphListener();
    if (st().mode == m)
        return;
    st().mode = m;
    notify();
}

void setModeUndoable(ComponentMode m)
{
    if (mode() != m)
        UndoStack::get().record(std::make_unique<ModeAction>(mode(), m));
    setMode(m);
}

const char* modeName(ComponentMode m)
{
    switch (m)
    {
    case ComponentMode::Object: return "object";
    case ComponentMode::Vertex: return "vertex";
    case ComponentMode::Edge: return "edge";
    case ComponentMode::Face: return "face";
    }
    return "object";
}

ComponentMode modeFromName(const std::string& name)
{
    if (name == "object")
        return ComponentMode::Object;
    if (name == "vertex" || name == "point" || name == "points" || name == "vertices")
        return ComponentMode::Vertex;
    if (name == "edge" || name == "edges")
        return ComponentMode::Edge;
    if (name == "face" || name == "faces")
        return ComponentMode::Face;
    throw std::invalid_argument("mode must be object, vertex, edge or face");
}

const std::map<NodeId, Ids>& selected(ComponentMode kind) { return st().sets[slot(kind)]; }

const Ids& selected(NodeId node, ComponentMode kind)
{
    static const Ids empty;
    const auto& m = st().sets[slot(kind)];
    auto it = m.find(node);
    return it == m.end() ? empty : it->second;
}

void select(NodeId node, ComponentMode kind, const Ids& ids, Op op)
{
    ensureGraphListener();
    Snapshot before = snapshot();
    auto& sets = st().sets[slot(kind)];
    Ids& cur = sets[node];
    Ids in = sortedUnique(ids);
    switch (op)
    {
    case Op::Replace:
        sets.clear();
        sets[node] = in;
        break;
    case Op::Add:
    {
        Ids out;
        std::set_union(cur.begin(), cur.end(), in.begin(), in.end(), std::back_inserter(out));
        cur = std::move(out);
        break;
    }
    case Op::Remove:
    {
        Ids out;
        std::set_difference(cur.begin(), cur.end(), in.begin(), in.end(), std::back_inserter(out));
        cur = std::move(out);
        break;
    }
    case Op::Toggle:
    {
        Ids out;
        std::set_symmetric_difference(cur.begin(), cur.end(), in.begin(), in.end(), std::back_inserter(out));
        cur = std::move(out);
        break;
    }
    }
    if (auto it = sets.find(node); it != sets.end() && it->second.empty())
        sets.erase(it);
    record(std::move(before));
    notify();
}

void clear()
{
    Snapshot before = snapshot();
    for (auto& s : st().sets)
        s.clear();
    record(std::move(before));
    notify();
}

void setAll(std::map<NodeId, Ids> points, std::map<NodeId, Ids> edges, std::map<NodeId, Ids> faces)
{
    ensureGraphListener();
    Snapshot before = snapshot();
    st().sets[0] = std::move(points);
    st().sets[1] = std::move(edges);
    st().sets[2] = std::move(faces);
    record(std::move(before));
    notify();
}

Ids pointsOf(NodeId node)
{
    MeshPtr m = meshOf(node);
    if (!m)
        return {};
    Ids out;
    switch (mode())
    {
    case ComponentMode::Vertex:
        for (uint32_t p : selected(node, ComponentMode::Vertex))
            if (m->pointValid(p))
                out.push_back(p);
        break;
    case ComponentMode::Edge:
        for (uint32_t e : selected(node, ComponentMode::Edge))
            if (m->edgeValid(e))
            {
                out.push_back(m->edgePoint(e, 0));
                out.push_back(m->edgePoint(e, 1));
            }
        break;
    case ComponentMode::Face:
        for (uint32_t f : selected(node, ComponentMode::Face))
            if (m->faceValid(f))
                m->forEachFaceCorner(f, [&](geo::Index c) { out.push_back(m->cornerPoint(c)); });
        break;
    default: break;
    }
    return sortedUnique(std::move(out));
}

int addListener(std::function<void()> fn)
{
    ensureGraphListener();
    st().listeners[st().nextListener] = std::move(fn);
    return st().nextListener++;
}

void removeListener(int id) { st().listeners.erase(id); }

namespace
{
Node& meshNode(const std::string& name, const char* cmd)
{
    Node* n = Graph::current().find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

Ids idsArg(const json& j)
{
    Ids out;
    for (const json& v : j)
    {
        if (v.is_array() && v.size() == 2) // [first, last] range
            for (uint32_t i = v[0].get<uint32_t>(); i <= v[1].get<uint32_t>(); ++i)
                out.push_back(i);
        else
            out.push_back(v.get<uint32_t>());
    }
    return out;
}

} // namespace

Ids convert(const geo::Mesh& m, ComponentMode from, const Ids& ids, ComponentMode to, bool touching)
{
    Ids out;
    if (from == to)
        return ids;
    auto pointsOfIds = [&] {
        Ids pts;
        if (from == ComponentMode::Edge)
            for (uint32_t e : ids)
            {
                if (!m.edgeValid(e))
                    continue;
                pts.push_back(m.edgePoint(e, 0));
                pts.push_back(m.edgePoint(e, 1));
            }
        else if (from == ComponentMode::Face)
            for (uint32_t f : ids)
                if (m.faceValid(f))
                    m.forEachFaceCorner(f, [&](geo::Index c) { pts.push_back(m.cornerPoint(c)); });
        else
            pts = ids;
        return sortedUnique(pts);
    };
    if (to == ComponentMode::Vertex)
        return pointsOfIds();
    if (to == ComponentMode::Edge)
    {
        if (from == ComponentMode::Face)
        {
            for (uint32_t f : ids)
                if (m.faceValid(f))
                    m.forEachFaceCorner(f, [&](geo::Index c) { out.push_back(m.cornerEdge(c)); });
            return sortedUnique(out);
        }
        // Edges with both ends selected, or either end when touching.
        Ids pts = pointsOfIds();
        std::set<uint32_t> in(pts.begin(), pts.end());
        for (uint32_t p : pts)
            m.forEachPointEdge(p, [&](geo::Index e) {
                if (touching || in.contains(m.edgeOtherPoint(e, p)))
                    out.push_back(e);
            });
        return sortedUnique(out);
    }
    if (touching && from == ComponentMode::Edge)
    {
        for (uint32_t e : ids)
            if (m.edgeValid(e))
                m.forEachEdgeCorner(e, [&](geo::Index c) { out.push_back(m.cornerFace(c)); });
        return sortedUnique(out);
    }
    // Faces fully covered by the selected points.
    Ids pts = pointsOfIds();
    std::set<uint32_t> in(pts.begin(), pts.end());
    for (uint32_t p : pts)
        for (geo::Index f : m.pointFaces(p))
        {
            bool all = true;
            if (!touching)
                m.forEachFaceCorner(f, [&](geo::Index c) { all = all && in.contains(m.cornerPoint(c)); });
            if (all)
                out.push_back(f);
        }
    return sortedUnique(out);
}

namespace
{
/** Every mesh in the hilite set: the selected meshes and the meshes under selected groups. */
std::vector<NodeId> hilitedMeshes()
{
    std::vector<NodeId> out;
    Graph& g = Graph::current();
    std::set<NodeId> seen;
    auto add = [&](NodeId id) {
        if (Node* n = g.node(id); n && n->type().isA("mesh") && seen.insert(id).second)
            out.push_back(id);
    };
    for (NodeId id : g.selection())
    {
        add(id);
        for (NodeId d : g.descendants(id))
            add(d);
    }
    return out;
}

Ids allIds(const geo::Mesh& m, ComponentMode k)
{
    Ids all;
    geo::Index cap = k == ComponentMode::Vertex ? m.pointCapacity() : k == ComponentMode::Edge ? m.edgeCapacity() : m.faceCapacity();
    for (geo::Index i = 0; i < cap; ++i)
        if (k == ComponentMode::Vertex ? m.pointValid(i) : k == ComponentMode::Edge ? m.edgeValid(i) : m.faceValid(i))
            all.push_back(i);
    return all;
}
} // namespace

void registerCommands()
{
    using T = ArgType;
    watchCreations();
    registerPathCommands();

    ops::registerCommand({
        .name = "selectMode",
        .doc = "Switch between object, vertex, edge and face selection. With no argument, returns the current mode.",
        .positionalDoc = "[mode]",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.positional.empty())
            {
                try
                {
                    ComponentMode next = modeFromName(a.positional[0].get<std::string>());
                    // Each kind keeps its own selection, so coming back to a mode brings back what was selected in it.
                    setMode(next);
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("selectMode: ") + e.what());
                }
            }
            return modeName(mode());
        },
    });

    ops::registerCommand({
        .name = "selectComponents",
        .doc = "Select vertices, edges or faces of a mesh by id. Ids may be [first, last] ranges. Without add/deselect/toggle "
               "it replaces the selection of that kind.",
        .positionalDoc = "mesh",
        .flags = {
            {"vertices", "", T::List, nullptr, "Point ids."},
            {"edges", "", T::List, nullptr, "Edge ids."},
            {"faces", "", T::List, nullptr, "Face ids."},
            {"add", "", T::Bool, false, "Add to the selection."},
            {"deselect", "", T::Bool, false, "Remove from the selection."},
            {"toggle", "", T::Bool, false, "Toggle."},
        },
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("selectComponents: no mesh");
            Node& n = meshNode(objs.back(), "selectComponents");
            Op op = a.get<bool>("add") ? Op::Add : a.get<bool>("deselect") ? Op::Remove : a.get<bool>("toggle") ? Op::Toggle : Op::Replace;
            UndoChunk chunk("Select Components");
            bool any = false;
            for (auto [flag, kind] : {std::pair{"vertices", ComponentMode::Vertex}, {"edges", ComponentMode::Edge}, {"faces", ComponentMode::Face}})
                if (a.has(flag))
                {
                    select(n.id(), kind, idsArg(a.flags[flag]), op);
                    if (mode() == ComponentMode::Object || !any)
                        setMode(kind);
                    any = true;
                }
            if (!any)
                throw CommandError("selectComponents: give vertices=, edges= or faces=");
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "componentSelection",
        .doc = "Selected components: {mesh: {\"vertices\": [...], \"edges\": [...], \"faces\": [...]}}.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::object();
            Graph& g = Graph::current();
            for (auto [key, kind] : {std::pair{"vertices", ComponentMode::Vertex}, {"edges", ComponentMode::Edge}, {"faces", ComponentMode::Face}})
                for (auto& [node, ids] : selected(kind))
                    if (Node* n = g.node(node))
                        out[n->path()][key] = ids;
            return out;
        },
    });

    ops::registerCommand({
        .name = "convertSelection",
        .doc = "Convert the component selection to another kind and switch to that mode. In object mode the selected meshes "
               "convert as a whole: every component of that kind.",
        .positionalDoc = "to",
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("convertSelection: give vertex, edge or face");
            ComponentMode to = modeFromName(a.positional[0].get<std::string>());
            ComponentMode from = mode();
            if (to == ComponentMode::Object)
                throw CommandError("convertSelection: give vertex, edge or face");
            std::map<NodeId, Ids> sets[3];
            if (from == ComponentMode::Object)
            {
                // A whole object stands for all of its components.
                for (NodeId node : hilitedMeshes())
                    if (MeshPtr m = meshOf(node))
                        sets[slot(to)][node] = isolation::visibleOnly(isolation::activeVisibility(node, *m), to, allIds(*m, to));
                if (sets[slot(to)].empty())
                    throw CommandError("convertSelection: select a mesh or some components");
            }
            else
                for (auto& [node, ids] : selected(from))
                    if (MeshPtr m = meshOf(node))
                        sets[slot(to)][node] = isolation::visibleOnly(isolation::activeVisibility(node, *m), to, convert(*m, from, ids, to, true));
            setAll(sets[0], sets[1], sets[2]);
            if (from != to)
                UndoStack::get().record(std::make_unique<ModeAction>(from, to));
            setMode(to);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "growSelection",
        .doc = "Grow the component selection by one ring of neighbours: points to the points of their faces, edges and "
               "faces to those sharing a point with the selection.",
        .run = [](const Args&) -> json {
            ComponentMode k = mode();
            if (k == ComponentMode::Object)
                return nullptr;
            UndoChunk chunk("Grow Selection");
            for (auto [node, ids] : selected(k))
            {
                MeshPtr m = meshOf(node);
                if (!m)
                    continue;
                Ids pts = convert(*m, k, ids, ComponentMode::Vertex);
                Ids grown;
                for (uint32_t p : pts)
                {
                    if (!m->pointValid(p))
                        continue;
                    if (k == ComponentMode::Edge)
                        m->forEachPointEdge(p, [&](geo::Index e) { grown.push_back(e); });
                    else
                        for (geo::Index f : m->pointFaces(p))
                        {
                            if (k == ComponentMode::Face)
                                grown.push_back(f);
                            else
                                m->forEachFaceCorner(f, [&](geo::Index c) { grown.push_back(m->cornerPoint(c)); });
                        }
                }
                select(node, k, isolation::visibleOnly(isolation::activeVisibility(node, *m), k, std::move(grown)), Op::Add);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "selectLoop",
        .doc = "Extend the selection to full loops (or rings with ring=True): edges along their own loops, vertices along "
               "the loops of the edges joining them, faces along the face loops crossing the edges they share.",
        .flags = {{"ring", "", T::Bool, false, "Select rings instead of loops. Faces have no ring and take their loops."}},
        .run = [](const Args& a) -> json {
            ComponentMode k = mode();
            if (k == ComponentMode::Object)
                return nullptr;
            UndoChunk chunk("Select Loop");
            bool ring = a.get<bool>("ring") && k != ComponentMode::Face;
            for (auto [node, ids] : selected(k))
            {
                MeshPtr m = meshOf(node);
                if (!m)
                    continue;
                const isolation::Visibility vis = isolation::activeVisibility(node, *m);
                Ids out;
                if (k == ComponentMode::Face)
                {
                    std::vector<uint8_t> in = geo::makeMask(m->faceCapacity(), ids);
                    for (geo::Index e : convert(*m, ComponentMode::Face, ids, ComponentMode::Edge))
                    {
                        int faces = 0;
                        m->forEachEdgeCorner(e, [&](geo::Index c) { faces += in[m->cornerFace(c)]; });
                        if (faces >= 2)
                            for (geo::Index f : isolation::clipLoop(*m, vis, geo::faceLoop(*m, e), e, ComponentMode::Face, false))
                                out.push_back(f);
                    }
                }
                else
                {
                    Ids edges = k == ComponentMode::Vertex ? convert(*m, ComponentMode::Vertex, ids, ComponentMode::Edge) : ids;
                    for (uint32_t e : edges)
                        for (geo::Index x : isolation::clipLoop(*m, vis, ring ? geo::edgeRing(*m, e) : geo::edgeLoop(*m, e), e, ComponentMode::Edge, ring))
                            out.push_back(x);
                    if (k == ComponentMode::Vertex)
                        out = convert(*m, ComponentMode::Edge, sortedUnique(out), ComponentMode::Vertex);
                }
                select(node, k, out, Op::Add);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "shrinkSelection",
        .doc = "Shrink the component selection by dropping what lies on its border.",
        .run = [](const Args&) -> json {
            ComponentMode k = mode();
            if (k == ComponentMode::Object)
                return nullptr;
            UndoChunk chunk("Shrink Selection");
            for (auto [node, ids] : selected(k))
            {
                MeshPtr m = meshOf(node);
                if (!m)
                    continue;
                Ids drop;
                if (k == ComponentMode::Face)
                {
                    std::vector<uint8_t> in = geo::makeMask(m->faceCapacity(), ids);
                    // The mesh's own boundary counts as unselected surroundings too.
                    for (uint32_t f : ids)
                        for (geo::Index p : m->facePoints(f))
                            if (m->isBoundaryPoint(p) || std::ranges::any_of(m->pointFaces(p), [&](geo::Index g) { return !in[g]; }))
                            {
                                drop.push_back(f);
                                break;
                            }
                }
                else if (k == ComponentMode::Edge)
                {
                    // The inverse of grow: an edge stays only if every edge at both its ends is selected.
                    std::vector<uint8_t> in = geo::makeMask(m->edgeCapacity(), ids);
                    auto border = [&](geo::Index p) {
                        bool out = m->isBoundaryPoint(p);
                        m->forEachPointEdge(p, [&](geo::Index e) { out = out || !in[e]; });
                        return out;
                    };
                    for (uint32_t e : ids)
                        if (m->edgeValid(e) && (border(m->edgePoint(e, 0)) || border(m->edgePoint(e, 1))))
                            drop.push_back(e);
                }
                else
                {
                    Ids pts = convert(*m, k, ids, ComponentMode::Vertex);
                    std::vector<uint8_t> in = geo::makeMask(m->pointCapacity(), pts);
                    std::vector<uint8_t> border(m->pointCapacity(), 0);
                    for (uint32_t p : pts)
                    {
                        if (m->isBoundaryPoint(p))
                            border[p] = 1;
                        for (geo::Index f : m->pointFaces(p))
                            for (geo::Index q : m->facePoints(f))
                                if (!in[q])
                                    border[p] = 1;
                    }
                    for (uint32_t id : ids)
                        if (border[id])
                            drop.push_back(id);
                }
                if (!drop.empty())
                    select(node, k, drop, Op::Remove);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "selectBorder",
        .doc = "Select border edges: the perimeter of selected faces, or the whole open boundary through selected edges or vertices.",
        .run = [](const Args&) -> json {
            ComponentMode k = mode();
            if (k == ComponentMode::Object)
                return nullptr;
            std::map<NodeId, Ids> out;
            for (auto& [node, ids] : selected(k))
            {
                MeshPtr m = meshOf(node);
                if (!m)
                    continue;
                Ids edges;
                if (k == ComponentMode::Face)
                    edges = geo::regionBoundaryEdges(*m, ids);
                else
                {
                    std::vector<uint8_t> touched = geo::makeMask(m->pointCapacity(), convert(*m, k, ids, ComponentMode::Vertex));
                    if (k == ComponentMode::Edge)
                        touched.assign(m->pointCapacity(), 0);
                    std::vector<uint8_t> edgeIn = k == ComponentMode::Edge ? geo::makeMask(m->edgeCapacity(), ids) : std::vector<uint8_t>();
                    for (const geo::EdgePath& loop : geo::boundaryLoops(*m))
                    {
                        bool hit = false;
                        for (size_t i = 0; i < loop.edges.size() && !hit; ++i)
                            hit = k == ComponentMode::Edge ? bool(edgeIn[loop.edges[i]]) : bool(touched[loop.points[i]]);
                        if (hit)
                            edges.insert(edges.end(), loop.edges.begin(), loop.edges.end());
                    }
                }
                if (!edges.empty())
                    out[node] = sortedUnique(edges);
            }
            setAll(selected(ComponentMode::Vertex), out, selected(ComponentMode::Face));
            setMode(ComponentMode::Edge);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "selectByAngle",
        .doc = "Grow the face selection across edges where neighbouring faces bend less than an angle.",
        .flags = {{"angle", "", T::Float, 30.0, "Largest bend between neighbouring faces, degrees."}},
        .run = [](const Args& a) -> json {
            if (mode() != ComponentMode::Face)
                throw CommandError("selectByAngle: select some faces first");
            double cosLimit = std::cos(a.get<double>("angle") * 3.14159265358979323846 / 180.0);
            UndoChunk chunk("Select by Angle");
            for (auto [node, ids] : selected(ComponentMode::Face))
            {
                MeshPtr m = meshOf(node);
                if (!m)
                    continue;
                std::vector<uint8_t> in = geo::makeMask(m->faceCapacity(), ids);
                const isolation::Visibility vis = isolation::activeVisibility(node, *m);
                std::vector<geo::Index> work(ids.begin(), ids.end());
                Ids grown;
                while (!work.empty())
                {
                    geo::Index f = work.back();
                    work.pop_back();
                    if (!m->faceValid(f))
                        continue;
                    glm::dvec3 nf = m->faceNormal(f);
                    for (geo::Index c : m->faceCorners(f))
                    {
                        geo::Index e = m->cornerEdge(c);
                        if (!m->isManifoldEdge(e))
                            continue;
                        geo::Index g = m->oppositeFace(e, f);
                        if (g == geo::kInvalid || in[g] || !vis.face(g) || glm::dot(nf, m->faceNormal(g)) < cosLimit)
                            continue;
                        in[g] = 1;
                        grown.push_back(g);
                        work.push_back(g);
                    }
                }
                if (!grown.empty())
                    select(node, ComponentMode::Face, grown, Op::Add);
            }
            return nullptr;
        },
    });
    ops::setAdjustable("selectByAngle");

    ops::registerCommand({
        .name = "selectAllComponents",
        .doc = "Select every component of the current kind on the hilited meshes.",
        .run = [](const Args&) -> json {
            ComponentMode k = mode();
            if (k == ComponentMode::Object)
                return nullptr;
            UndoChunk chunk("Select All Components");
            bool first = true;
            for (NodeId node : hilitedMeshes())
            {
                MeshPtr m = meshOf(node);
                if (!m)
                    continue;
                select(node, k, isolation::visibleOnly(isolation::activeVisibility(node, *m), k, allIds(*m, k)), first ? Op::Replace : Op::Add);
                first = false;
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "invertSelection",
        .doc = "Invert the selection: the unselected components of the current kind on the hilited meshes, or in object mode "
               "every unselected object. An isolated view inverts only what it shows.",
        .run = [](const Args&) -> json {
            ComponentMode k = mode();
            if (k == ComponentMode::Object)
            {
                Graph& g = Graph::current();
                const isolation::Set* iso = isolation::active();
                std::vector<NodeId> shown;
                for (NodeId id : g.nodes())
                    if (const Node* n = g.node(id); n && n->type().dag && (!iso || iso->shows(id)))
                        shown.push_back(id);
                g.select(shown, SelectMode::Toggle);
                return nullptr;
            }
            UndoChunk chunk("Invert Selection");
            for (NodeId node : hilitedMeshes())
                if (MeshPtr m = meshOf(node))
                    select(node, k, isolation::visibleOnly(isolation::activeVisibility(node, *m), k, allIds(*m, k)), Op::Toggle);
            return nullptr;
        },
    });
}
} // namespace lila::scene::components
