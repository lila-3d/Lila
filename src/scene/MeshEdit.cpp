#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Knife.h"
#include "geo/LoopTools.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Reduce.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"
#include "scene/AutoBevel.h"
#include "scene/Components.h"
#include "scene/Fields.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/LastOp.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/ObjectSets.h"
#include "scene/SoftSelect.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <cstring>
#include <format>
#include <optional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <tbb/parallel_for.h>
#include <glm/gtc/matrix_transform.hpp>

namespace lila::scene
{
namespace
{
using components::Ids;
using geo::Index;

Graph& graph() { return Graph::current(); }

/** Direct edits work on the base mesh under any modifiers; a procedural source is frozen first. */
PlugRef basePlug(Node& n)
{
    return modifiers::editPlug(graph(), n.id());
}

geo::Mesh baseMesh(const PlugRef& p)
{
    graph().evaluate();
    const MeshPtr& m = std::get<MeshPtr>(graph().storedValue(p));
    return m ? *m : geo::Mesh();
}

int symmetryAxis(const Node& n)
{
    int64_t axis = n.as<int64_t>(MeshNode::kSymmetry);
    return axis >= 1 && axis <= 3 ? int(axis - 1) : -1;
}

/** Only for matching points that have no stored partner; stored pairs survive any drift. */
double symmetryTolerance(const geo::Mesh& mesh)
{
    return std::max(1e-4, glm::length(mesh.bounds().size()) * 1e-5);
}

/**
 * The radial count the mesh's points fit: the largest that leaves none without an orbit, else the one leaving fewest.
 * 0 when no count fits any point, or the mesh is too dense to try them all.
 */
int bestRadialCount(const geo::Mesh& mesh, const geo::RadialSymmetry& radial, double tolerance)
{
    LILA_ZONE();

    if (mesh.numPoints() > 20000)
        return 0;
    int best = 0;
    Index fewest = mesh.numPoints();
    for (int count = geo::kMaxRadialCount; count >= 2; --count)
    {
        const geo::RadialSymmetry r(radial.origin(), radial.axis(), count, radial.mirror(), radial.reference());
        const Index unmatched = geo::SymmetryMap::buildRadial(mesh, r, tolerance).unmatchedCount();
        if (unmatched == 0)
            return count;
        if (unmatched < fewest)
        {
            fewest = unmatched;
            best = count;
        }
    }
    return best;
}

std::string symmetryLayer(const geo::SymmetrySpec& spec)
{
    return spec.radial ? geo::radialLayerName(*spec.radial) : geo::symmetryLayerName(spec.axis);
}

std::string symmetryName(const geo::SymmetrySpec& spec)
{
    return spec.radial ? symmetryLabel(spec) : std::string(1, char('X' + spec.axis));
}

void reportSync(const Node& n, const geo::SymmetrySpec& spec, const geo::SymmetrySync& s, bool hadLayer)
{
    const Index rematched = s.topological + s.byPosition;
    if (rematched == 0)
        return;
    if (hadLayer)
        log::write(LogLevel::Warning, std::format("{}: symmetry {} re-matched {} points without a stored partner ({} by position), {} unmatched",
                                                  n.path(), symmetryName(spec), rematched, s.byPosition, s.unmatched));
    else
        log::write(LogLevel::Info, std::format("{}: symmetry {} paired {} points, {} unmatched", n.path(), symmetryName(spec), rematched, s.unmatched));
}

/** The mesh's persistent symmetry pairs (or radial orbits), stored onto it if it has none yet. */
std::optional<geo::SymmetryMap> symmetryFor(const Node& n, geo::Mesh& mesh)
{
    const std::optional<geo::SymmetrySpec> spec = symmetrySpec(n);
    if (!spec)
        return std::nullopt;
    const bool hadLayer = mesh.pointAttrs().has(symmetryLayer(*spec));
    geo::SymmetrySync sync;
    geo::SymmetryMap map = geo::SymmetryMap::persistent(mesh, *spec, symmetryTolerance(mesh), &sync);
    reportSync(n, *spec, sync, hadLayer);
    return map;
}

/**
 * After a topology op: pairs the points it created (every wedge was made together) and sets every moved or new
 * point's orbit to the exact images of its member in the driving wedge.
 */
void maintainSymmetry(const Node& n, geo::Mesh& mesh, const std::vector<glm::dvec3>& before, int driveWedge)
{
    LILA_ZONE();

    const std::optional<geo::SymmetrySpec> spec = symmetrySpec(n);
    if (!spec)
        return;
    const Index firstNew = Index(before.size());
    geo::SymmetryMap map = geo::SymmetryMap::persistent(mesh, *spec, symmetryTolerance(mesh));
    Ids touched;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && (p >= firstNew || std::memcmp(&before[p], &mesh.position(p), sizeof(glm::dvec3)) != 0))
            touched.push_back(p);
    map.enforceWedge(mesh, touched, driveWedge);
}

Ids remapIds(const Ids& ids, const std::vector<Index>& map)
{
    Ids out;
    for (Index i : ids)
        if (i < map.size() && map[i] != geo::kInvalid)
            out.push_back(map[i]);
    return out;
}

struct EditResult
{
    ComponentMode kind = ComponentMode::Object;
    Ids ids;
};

using EditFn = std::function<EditResult(geo::Mesh&, const Ids& selection, const geo::SymmetryMap* symmetry)>;

/**
 * Runs `fn` on every mesh with a component selection, converted to `kind`. Each mesh is edited on a copy, compacted,
 * written back to inMesh (undoable) and its component selection replaced by what the op returns.
 */
json editSelected(const char* label, ComponentMode kind, const EditFn& fn, bool allowWhole = false,
                  const std::function<bool(const Node&)>& handledWhole = nullptr)
{
    LILA_ZONE();

    Graph& g = graph();
    g.evaluate();
    std::map<NodeId, Ids> targets;
    ComponentMode from = components::mode();
    if (from != ComponentMode::Object)
        for (auto& [node, ids] : components::selected(from))
            if (Node* n = g.node(node); n && n->type().isA("mesh"))
                if (MeshPtr cage = modifiers::cage(g, node))
                    targets[node] = components::convert(*cage, from, ids, kind);
    bool handled = false;
    if (targets.empty() && allowWhole)
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("mesh"))
            {
                if (handledWhole && handledWhole(*n))
                    handled = true;
                else
                    targets[id] = {};
            }
    if (targets.empty() && handled)
        return nullptr;
    if (targets.empty())
        throw CommandError(std::string(label) + ": no components selected");

    UndoChunk chunk(label);
    std::map<NodeId, Ids> newSel[3] = {components::selected(ComponentMode::Vertex), components::selected(ComponentMode::Edge),
                                       components::selected(ComponentMode::Face)};
    ComponentMode resultKind = ComponentMode::Object;
    for (auto& [node, sel] : targets)
    {
        Node& n = *g.node(node);
        PlugRef base = basePlug(n);
        geo::Mesh mesh = baseMesh(base);
        auto sym = symmetryFor(n, mesh);
        std::vector<glm::dvec3> before;
        int drive = 0;
        if (sym)
        {
            before.assign(mesh.positions().begin(), mesh.positions().end());
            drive = sym->majorityWedge(components::convert(mesh, kind, sel, ComponentMode::Vertex));
        }
        const Index firstNew = mesh.pointCapacity();
        EditResult r = fn(mesh, sel, sym ? &*sym : nullptr);
        geo::clearSymmetryKeys(mesh, firstNew);
        if (sym)
            maintainSymmetry(n, mesh, before, drive);
        geo::MeshRemap remap;
        mesh.compact(&remap);
        const std::vector<Index>* maps[3] = {&remap.points, &remap.edges, &remap.faces};
        // The other kinds' selections follow the new ids, so they come back when their mode does.
        for (int k = 0; k < 3; ++k)
            if (auto it = newSel[k].find(node); it != newSel[k].end())
                if (Ids kept = remapIds(it->second, *maps[k]); kept.empty())
                    newSel[k].erase(it);
                else
                    it->second = std::move(kept);
        if (r.kind != ComponentMode::Object)
        {
            resultKind = r.kind;
            Ids mapped = remapIds(r.ids, *maps[int(r.kind) - 1]);
            if (mapped.empty())
                newSel[int(r.kind) - 1].erase(node);
            else
                newSel[int(r.kind) - 1][node] = std::move(mapped);
        }
        auto edited = std::make_shared<geo::Mesh>(std::move(mesh));
        g.setValue(base, MeshPtr(edited));
        modifiers::baseRemapped(g, node, remap, *edited);
        objectSets::meshRemapped(g, node, remap);
    }
    components::setAll(newSel[0], newSel[1], newSel[2]);
    // A component mode is the user's: repeated edits (bridge after bridge) must not bounce it to the result's kind.
    if (resultKind != ComponentMode::Object && from == ComponentMode::Object)
        components::setModeUndoable(resultKind);
    return nullptr;
}

/**
 * Maya's default for a component edit run in object mode: every component of kind on the selected meshes gets selected
 * and kind becomes the mode, so Extrude on an object extrudes all its faces.
 */
void selectWholeInObjectMode(ComponentMode kind)
{
    if (components::mode() != ComponentMode::Object)
        return;
    Graph& g = graph();
    g.evaluate();
    std::map<NodeId, Ids> all;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("mesh"))
            if (MeshPtr cage = modifiers::cage(g, id))
            {
                const geo::Mesh& m = *cage;
                Ids ids;
                const Index count = kind == ComponentMode::Vertex ? m.pointCapacity() : kind == ComponentMode::Edge ? m.edgeCapacity() : m.faceCapacity();
                for (Index i = 0; i < count; ++i)
                    if (kind == ComponentMode::Vertex ? m.pointValid(i) : kind == ComponentMode::Edge ? m.edgeValid(i) : m.faceValid(i))
                        ids.push_back(i);
                if (!ids.empty())
                    all[id] = std::move(ids);
            }
    if (all.empty())
        return;
    components::setAll(kind == ComponentMode::Vertex ? all : std::map<NodeId, Ids>{}, kind == ComponentMode::Edge ? all : std::map<NodeId, Ids>{},
                       kind == ComponentMode::Face ? all : std::map<NodeId, Ids>{});
    components::setModeUndoable(kind);
}

EditResult faces(Ids ids) { return {ComponentMode::Face, std::move(ids)}; }
EditResult edges(Ids ids) { return {ComponentMode::Edge, std::move(ids)}; }
EditResult points(Ids ids) { return {ComponentMode::Vertex, std::move(ids)}; }

Ids withMirror(const geo::Mesh& m, const geo::SymmetryMap* s, const Ids& ids, ComponentMode kind)
{
    if (!s)
        return ids;
    switch (kind)
    {
    case ComponentMode::Vertex: return s->expandPoints(ids);
    case ComponentMode::Edge: return s->expandEdges(m, ids);
    case ComponentMode::Face: return s->expandFaces(m, ids);
    default: return ids;
    }
}

Node& meshArg(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}
/** The mesh= flag's mesh, else the first selected or hilited mesh. */
Node& meshFlag(const Args& a, const char* cmd)
{
    Graph& g = graph();
    std::string name = a.get<std::string>("mesh", "");
    if (name.empty())
    {
        std::vector<NodeId> candidates = g.selection();
        for (auto& [node, ids] : components::selected(components::mode()))
            candidates.push_back(node);
        for (NodeId id : candidates)
            if (Node* n = g.node(id); n && n->type().isA("mesh"))
                return *n;
        throw CommandError(std::string(cmd) + ": give mesh= or select a mesh");
    }
    return meshArg(name, cmd);
}

/** How many pieces the points make, joined by the mesh's edges between them. */
int selectionPieces(const geo::Mesh& m, const Ids& points)
{
    std::set<Index> open(points.begin(), points.end());
    int pieces = 0;
    while (!open.empty())
    {
        ++pieces;
        std::vector<Index> stack{*open.begin()};
        open.erase(open.begin());
        while (!stack.empty())
        {
            const Index p = stack.back();
            stack.pop_back();
            m.forEachPointEdge(p, [&](Index e) {
                if (auto it = open.find(m.edgeOtherPoint(e, p)); it != open.end())
                {
                    stack.push_back(*it);
                    open.erase(it);
                }
            });
        }
    }
    return pieces;
}

/** The wedge holding most of points; the one last picked with symmetry when it holds as many. */
int driveWedge(NodeId node, const geo::SymmetryMap& sym, const Ids& points)
{
    const int picked = components::pickedWedge(node);
    if (picked < 0 || picked >= sym.order())
        return sym.majorityWedge(points);
    std::vector<int64_t> count(size_t(sym.order()), 0);
    for (Index p : points)
        if (const int w = sym.wedge(p); w >= 0)
            ++count[size_t(w)];
    return count[size_t(picked)] == *std::max_element(count.begin(), count.end()) ? picked : sym.majorityWedge(points);
}

/** World-space distance along the edges from the nearest selected point, up to limit (infinity beyond it). */
std::vector<double> surfaceDistances(const geo::Mesh& m, const glm::dmat4& world, const Ids& selected, double limit)
{
    LILA_ZONE();

    std::vector<double> dist(m.pointCapacity(), std::numeric_limits<double>::infinity());
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    for (Index p : selected)
        if (p < dist.size() && m.pointValid(p))
        {
            dist[p] = 0.0;
            open.push({0.0, p});
        }
    auto wp = [&](Index p) { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); };
    while (!open.empty())
    {
        auto [d, p] = open.top();
        open.pop();
        if (d > dist[p])
            continue;
        glm::dvec3 at = wp(p);
        m.forEachPointEdge(p, [&](Index e) {
            Index q = m.edgePoint(e, 0) == p ? m.edgePoint(e, 1) : m.edgePoint(e, 0);
            double nd = d + glm::length(wp(q) - at);
            if (nd < dist[q] && nd < limit)
            {
                dist[q] = nd;
                open.push({nd, q});
            }
        });
    }
    return dist;
}
} // namespace

glm::dvec3 selectionCenter()
{
    Graph& g = graph();
    g.evaluate();
    ComponentMode kind = components::mode();
    glm::dvec3 sum(0.0);
    size_t count = 0;
    if (kind == ComponentMode::Object)
        return sum;
    for (auto& [node, ids] : components::selected(kind))
    {
        Node* n = g.node(node);
        if (const components::PointProvider* pp = n ? components::pointProvider(*n) : nullptr)
        {
            glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
            std::set<uint32_t> want(ids.begin(), ids.end());
            for (auto& [id, p] : pp->points(g, node))
                if (want.contains(id))
                {
                    sum += glm::dvec3(w * glm::dvec4(p, 1.0));
                    ++count;
                }
            continue;
        }
        MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, node) : nullptr;
        if (!cage)
            continue;
        // The object-space sum only changes with the mesh or the selection; the manipulator asks every frame.
        struct Cached
        {
            std::weak_ptr<const geo::Mesh> cage;
            uint64_t serial = 0;
            ComponentMode kind = ComponentMode::Object;
            glm::dvec3 sum{0.0};
            size_t count = 0;
        };
        static std::unordered_map<NodeId, Cached> cache;
        Cached& c = cache[node];
        if (c.cage.lock() != cage || c.serial != components::serial() || c.kind != kind)
        {
            const geo::Mesh& m = *cage;
            c = {cage, components::serial(), kind};
            for (Index p : components::convert(m, kind, ids, ComponentMode::Vertex))
                if (m.pointValid(p))
                {
                    c.sum += m.position(p);
                    ++c.count;
                }
        }
        const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
        sum += glm::dvec3(w * glm::dvec4(c.sum, double(c.count)));
        count += c.count;
    }
    return count ? sum / double(count) : sum;
}

glm::dvec3 symmetricSelectionCenter()
{
    Graph& g = graph();
    g.evaluate();
    const ComponentMode kind = components::mode();
    if (kind == ComponentMode::Object)
        return glm::dvec3(0.0);
    glm::dvec3 sum(0.0);
    size_t count = 0;
    for (auto& [node, ids] : components::selected(kind))
    {
        Node* n = g.node(node);
        MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, node) : nullptr;
        if (!cage)
            return selectionCenter();
        const geo::Mesh& m = *cage;
        const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
        for (Index p : components::convert(m, kind, drivingSide(node, kind, ids, cage), ComponentMode::Vertex))
            if (m.pointValid(p))
            {
                sum += glm::dvec3(w * glm::dvec4(m.position(p), 1.0));
                ++count;
            }
    }
    return count ? sum / double(count) : selectionCenter();
}

namespace
{
Ids drivingSideOf(const Node& n, const geo::Mesh& mesh, ComponentMode kind, const Ids& ids)
{
    geo::Mesh m = mesh;
    auto sym = symmetryFor(n, m);
    if (!sym || ids.empty())
        return ids;
    const Ids pts = components::convert(m, kind, ids, ComponentMode::Vertex);
    // Radial: a ring round the axis is one connected selection and stays whole; separate pieces (a petal in every wedge)
    // each act in place.
    if (std::any_of(pts.begin(), pts.end(), [&](Index p) { return sym->onPlane(p); }) || (sym->isRadial() && selectionPieces(m, pts) <= 1))
        return ids;
    const int drive = driveWedge(n.id(), *sym, pts);
    Ids out;
    for (Index id : ids)
    {
        const Ids own = kind == ComponentMode::Vertex ? Ids{id} : components::convert(m, kind, {id}, ComponentMode::Vertex);
        if (!own.empty() && sym->wedge(own.front()) == drive)
            out.push_back(id);
    }
    return out.empty() ? ids : out;
}
} // namespace

Ids drivingSide(NodeId node, ComponentMode kind, const Ids& ids, MeshPtr mesh)
{
    Graph& g = graph();
    const Node* n = g.node(node);
    if (!n || !n->type().isA("mesh") || kind == ComponentMode::Object)
        return ids;
    if (!mesh)
        mesh = modifiers::cage(g, node);
    const std::optional<geo::SymmetrySpec> spec = symmetrySpec(*n);
    if (!mesh || !spec)
        return ids;
    const glm::dvec3 key(spec->axis, spec->offset, spec->order());
    const int picked = components::pickedWedge(node);
    // Building the symmetry map is the cost, and gizmos ask every frame.
    struct Cached
    {
        std::weak_ptr<const geo::Mesh> mesh;
        ComponentMode kind = ComponentMode::Object;
        glm::dvec3 spec{0.0};
        int picked = -1;
        Ids ids, out;
    };
    static std::map<std::pair<NodeId, const geo::Mesh*>, Cached> cache;
    std::erase_if(cache, [](const auto& e) { return e.second.mesh.expired(); });
    Cached& c = cache[{node, mesh.get()}];
    if (c.mesh.lock() != mesh || c.kind != kind || c.spec != key || c.picked != picked || c.ids != ids)
        c = {mesh, kind, key, picked, ids, drivingSideOf(*n, *mesh, kind, ids)};
    return c.out;
}

namespace
{
std::vector<float> falloffWeights(const geo::Mesh& m, const glm::dmat4& world, const Ids& selected)
{
    LILA_ZONE();

    std::vector<float> w(m.pointCapacity(), 0.0f);
    for (Index p : selected)
        if (p < w.size())
            w[p] = 1.0f;
    if (!PrefStore::get().value<bool>("general", "/softSelect/enabled", false) || selected.empty())
        return w;
    double radius = PrefStore::get().value<double>("general", "/softSelect/radius", 25.0);
    if (radius <= 0.0)
        return w;
    softSelect::Curve curve = softSelect::currentCurve();
    if (softSelect::surfaceFalloff())
    {
        std::vector<double> dist = surfaceDistances(m, world, selected, radius);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (w[p] < 1.0f && dist[p] < radius)
                w[p] = float(curve.evaluate(dist[p] / radius));
        return w;
    }
    std::vector<glm::dvec3> seeds;
    for (Index p : selected)
        if (m.pointValid(p))
            seeds.push_back(glm::dvec3(world * glm::dvec4(m.position(p), 1.0)));
    // Many seeds on a dense mesh: thin them out; the falloff is smooth, so a subset barely shows.
    size_t stride = std::max<size_t>(1, seeds.size() / 4096);
    double r2 = radius * radius;
    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
        if (!m.pointValid(p) || w[p] >= 1.0f)
            return;
        glm::dvec3 q = glm::dvec3(world * glm::dvec4(m.position(p), 1.0));
        double best = r2;
        for (size_t k = 0; k < seeds.size(); k += stride)
            best = std::min(best, glm::dot(q - seeds[k], q - seeds[k]));
        if (best >= r2)
            return;
        w[p] = float(curve.evaluate(std::sqrt(best) / radius));
    });
    return w;
}
} // namespace

std::vector<float> softWeights(const std::vector<glm::dvec3>& points, const std::vector<char>& selected)
{
    std::vector<float> w(points.size(), 0.0f);
    std::vector<glm::dvec3> seeds;
    for (size_t i = 0; i < points.size() && i < selected.size(); ++i)
        if (selected[i])
        {
            w[i] = 1.0f;
            seeds.push_back(points[i]);
        }
    const double radius = PrefStore::get().value<double>("general", "/softSelect/radius", 25.0);
    if (!PrefStore::get().value<bool>("general", "/softSelect/enabled", false) || seeds.empty() || radius <= 0.0)
        return w;
    const softSelect::Curve curve = softSelect::currentCurve();
    for (size_t i = 0; i < points.size(); ++i)
    {
        if (w[i] >= 1.0f)
            continue;
        double best = radius * radius;
        for (const glm::dvec3& s : seeds)
            best = std::min(best, glm::dot(points[i] - s, points[i] - s));
        if (best < radius * radius)
            w[i] = float(curve.evaluate(std::sqrt(best) / radius));
    }
    return w;
}

std::vector<float> softWeights(const geo::Mesh& m, const glm::dmat4& world, const Ids& selected)
{
    std::vector<float> w = falloffWeights(m, world, selected);
    fields::modulateSoftSelection(m, world, w);
    return w;
}

std::vector<glm::dvec3> componentNormals(const geo::Mesh& m, ComponentMode kind, const Ids& ids)
{
    LILA_ZONE();

    std::vector<glm::dvec3> n(m.pointCapacity(), glm::dvec3(0.0));
    // A face selection's points take only the selected faces, so a region offsets as a whole.
    std::vector<char> fromSelection(m.pointCapacity(), 0);
    if (kind == ComponentMode::Face)
        for (Index f : ids)
            if (m.faceValid(f))
            {
                glm::dvec3 a = m.faceAreaVector(f);
                m.forEachFaceCorner(f, [&](Index c) {
                    n[m.cornerPoint(c)] += a;
                    fromSelection[m.cornerPoint(c)] = 1;
                });
            }
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            glm::dvec3 a = m.faceAreaVector(f);
            m.forEachFaceCorner(f, [&](Index c) {
                if (!fromSelection[m.cornerPoint(c)])
                    n[m.cornerPoint(c)] += a;
            });
        }
    for (glm::dvec3& v : n)
        if (double len = glm::length(v); len > 1e-12)
            v /= len;
        else
            v = glm::dvec3(0.0);
    return n;
}

namespace
{
glm::dmat3 worldFrame(const glm::dmat3& f, const glm::dmat3& linear);

/** World frames of a point provider's given components, those without one left out. */
std::vector<glm::dmat3> providedWorldFrames(Graph& g, Node& n, const components::PointProvider& pp, const Ids& ids)
{
    std::vector<glm::dmat3> out;
    if (!pp.frames)
        return out;
    glm::dmat3 linear(n.as<glm::dmat4>(Transform::kWorldMatrix));
    for (const glm::dmat3& f : pp.frames(g, n.id(), ids))
        if (glm::dmat3 w = worldFrame(f, linear); glm::length(w[2]) > 0.5)
            out.push_back(w);
    return out;
}
} // namespace

glm::dvec3 selectionNormal()
{
    Graph& g = graph();
    g.evaluate();
    ComponentMode kind = components::mode();
    glm::dvec3 sum(0.0);
    if (kind == ComponentMode::Object)
        return glm::dvec3(0, 1, 0);
    for (auto& [node, ids] : components::selected(kind))
    {
        Node* n = g.node(node);
        if (const components::PointProvider* pp = n ? components::pointProvider(*n) : nullptr)
        {
            for (const glm::dmat3& f : providedWorldFrames(g, *n, *pp, ids))
                sum += f[2];
            continue;
        }
        MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, node) : nullptr;
        if (!cage)
            continue;
        const geo::Mesh& m = *cage;
        glm::dmat3 toWorld = glm::inverseTranspose(glm::dmat3(n->as<glm::dmat4>(Transform::kWorldMatrix)));
        const Ids side = drivingSide(node, kind, ids, cage);
        // Runs on every manipulator refresh, so only the selection's own faces are visited.
        glm::dvec3 local(0.0);
        if (kind == ComponentMode::Face)
            for (Index f : side)
                local += m.faceValid(f) ? m.faceAreaVector(f) : glm::dvec3(0.0);
        else
            for (Index p : components::convert(m, kind, side, ComponentMode::Vertex))
                if (m.pointValid(p))
                    for (Index f : m.pointFaces(p))
                        local += m.faceAreaVector(f);
        sum += toWorld * local;
    }
    double len = glm::length(sum);
    return len > 1e-12 ? sum / len : glm::dvec3(0, 1, 0);
}

namespace
{
/** The world axis least aligned with the normal: the U fallback, shared by every point so a region's tangents agree. */
glm::dvec3 tangentReference(const glm::dvec3& normal)
{
    int axis = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(normal[i]) < std::abs(normal[axis]) - 1e-9)
            axis = i;
    glm::dvec3 r(0.0);
    r[axis] = 1.0;
    return r;
}

/** Direction of increasing u across face f (dP/du over its triangle fan), weighted by area; zero where its uvs are degenerate. */
glm::dvec3 faceUTangent(const geo::Mesh& m, Index f, std::span<const glm::vec2> uv)
{
    glm::dvec3 sum(0.0);
    const Index c0 = m.faceCorner(f);
    const glm::dvec3 p0 = m.position(m.cornerPoint(c0));
    const glm::dvec2 t0 = uv[c0];
    for (Index c1 = m.cornerNext(c0), c2 = m.cornerNext(c1); c2 != c0; c1 = c2, c2 = m.cornerNext(c2))
    {
        glm::dvec3 e1 = m.position(m.cornerPoint(c1)) - p0, e2 = m.position(m.cornerPoint(c2)) - p0;
        glm::dvec2 d1 = glm::dvec2(uv[c1]) - t0, d2 = glm::dvec2(uv[c2]) - t0;
        double det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-14)
            continue;
        glm::dvec3 t = (e1 * d2.y - e2 * d1.y) / det;
        if (double len = glm::length(t); len > 1e-12)
            sum += t / len * glm::length(glm::cross(e1, e2));
    }
    return sum;
}

struct FrameContext
{
    const geo::Mesh& m;
    std::span<const glm::vec2> uv;
    std::vector<char> selFace, selEdge;
    glm::dvec3 reference{1, 0, 0};
};

FrameContext frameContext(const geo::Mesh& m, ComponentMode kind, const Ids& ids, const glm::dvec3& reference)
{
    FrameContext x{m, m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0))};
    if (x.uv.size() < m.cornerCapacity())
        x.uv = {};
    std::vector<char>& sel = kind == ComponentMode::Face ? x.selFace : x.selEdge;
    if (kind == ComponentMode::Face || kind == ComponentMode::Edge)
    {
        sel.assign(kind == ComponentMode::Face ? m.faceCapacity() : m.edgeCapacity(), 0);
        for (Index i : ids)
            if (i < sel.size())
                sel[i] = 1;
    }
    if (double len = glm::length(reference); len > 1e-12)
        x.reference = reference / len;
    return x;
}

glm::dmat3 pointFrame(const FrameContext& x, Index p)
{
    const geo::Mesh& m = x.m;
    bool onSelected = false;
    if (!x.selFace.empty())
        m.forEachPointCorner(p, [&](Index c) { onSelected = onSelected || x.selFace[m.cornerFace(c)]; });
    glm::dvec3 n(0.0), t(0.0);
    double tWeight = 0.0;
    m.forEachPointCorner(p, [&](Index c) {
        const Index f = m.cornerFace(c);
        if (onSelected && !x.selFace[f])
            return;
        n += m.faceAreaVector(f);
        if (!x.uv.empty())
        {
            glm::dvec3 ft = faceUTangent(m, f, x.uv);
            t += ft;
            tWeight += glm::length(ft);
        }
    });
    if (double len = glm::length(n); len > 1e-12)
        n /= len;
    else
        return glm::dmat3(0.0);
    auto inPlane = [&](const glm::dvec3& v) { return v - n * glm::dot(v, n); };
    t = inPlane(t);
    // Uv islands mirrored across the point cancel out, which counts as having no uvs.
    glm::dvec3 u = tWeight > 0.0 && glm::length(t) > 0.25 * tWeight ? t : inPlane(x.reference);
    if (glm::length(u) < 1e-3)
        u = inPlane(std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
    if (!x.selEdge.empty())
    {
        glm::dvec3 along(0.0);
        m.forEachPointEdge(p, [&](Index e) {
            if (!x.selEdge[e])
                return;
            glm::dvec3 d = inPlane(m.position(m.edgeOtherPoint(e, p)) - m.position(p));
            along += glm::dot(d, u) < 0.0 ? -d : d;
        });
        if (glm::length(along) > 1e-9)
            u = along;
    }
    u = glm::normalize(u);
    return glm::dmat3(u, glm::cross(n, u), n);
}

/** An object-space point frame in world space, kept orthonormal under non-uniform scale; zero stays zero. */
glm::dmat3 worldFrame(const glm::dmat3& f, const glm::dmat3& linear)
{
    glm::dvec3 n = glm::inverseTranspose(linear) * f[2];
    if (glm::length(n) < 1e-12)
        return glm::dmat3(0.0);
    n = glm::normalize(n);
    glm::dvec3 u = linear * f[0];
    u -= n * glm::dot(u, n);
    if (glm::length(u) < 1e-12)
        return glm::dmat3(0.0);
    u = glm::normalize(u);
    return glm::dmat3(u, glm::cross(n, u), n);
}
} // namespace

std::vector<glm::dmat3> componentFrames(const geo::Mesh& m, ComponentMode kind, const Ids& ids, const glm::dvec3& reference)
{
    LILA_ZONE();

    const FrameContext x = frameContext(m, kind, ids, reference);
    std::vector<glm::dmat3> frames(m.pointCapacity(), glm::dmat3(0.0));
    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
        if (m.pointValid(p))
            frames[p] = pointFrame(x, p);
    });
    return frames;
}

glm::dmat3 selectionNormalFrame()
{
    LILA_ZONE();

    const glm::dvec3 n = selectionNormal();
    const glm::dvec3 ref = tangentReference(n);
    Graph& g = graph();
    ComponentMode kind = components::mode();
    glm::dvec3 sum(0.0);
    if (kind != ComponentMode::Object)
        for (auto& [node, ids] : components::selected(kind))
        {
            Node* nd = g.node(node);
            if (const components::PointProvider* pp = nd ? components::pointProvider(*nd) : nullptr)
            {
                for (const glm::dmat3& f : providedWorldFrames(g, *nd, *pp, ids))
                    sum += f[0];
                continue;
            }
            MeshPtr cage = nd && nd->type().isA("mesh") ? modifiers::cage(g, node) : nullptr;
            if (!cage)
                continue;
            const geo::Mesh& m = *cage;
            glm::dmat3 linear(nd->as<glm::dmat4>(Transform::kWorldMatrix));
            // Only the selection's own points, since this runs on every manipulator refresh.
            const Ids side = drivingSide(node, kind, ids, cage);
            const FrameContext x = frameContext(m, kind, side, glm::inverse(linear) * ref);
            for (Index p : components::convert(m, kind, side, ComponentMode::Vertex))
                if (m.pointValid(p))
                    sum += worldFrame(pointFrame(x, p), linear)[0];
        }
    glm::dvec3 u = sum - n * glm::dot(sum, n);
    if (glm::length(u) < 1e-6)
        u = ref - n * glm::dot(ref, n);
    u = glm::normalize(u);
    return glm::dmat3(u, glm::cross(n, u), n);
}

namespace
{
/** Sets up a point provider node's targets by provider id, for edits that need more than a loose point mesh; empty skips the node. */
using ProvidedSetup = std::function<PointTarget(Graph&, Node&, const components::PointProvider&, const Ids&)>;

/**
 * deformSelectedComponents for a point provider's node: its selected points moved to their targets, from provided when set,
 * else from setup given them as a loose point mesh.
 */
bool deformProvidedPoints(Graph& g, Node& n, const components::PointProvider& pp, const Ids& ids, const PointTargetSetup& setup,
                          const ProvidedSetup& provided)
{
    std::map<uint32_t, glm::dvec3> all;
    for (auto& [id, p] : pp.points(g, n.id()))
        all[id] = p;
    geo::Mesh m;
    Ids local;
    std::vector<uint32_t> source;
    for (uint32_t id : ids)
        if (auto it = all.find(id); it != all.end())
        {
            local.push_back(m.addPoint(it->second));
            source.push_back(id);
        }
    if (source.empty())
        return false;
    glm::dmat4 world = n.as<glm::dmat4>(Transform::kWorldMatrix);
    // Soft selection pulls unselected points part of the way. They join the point mesh before setup sees it, but not the
    // selection; provided edits key their targets by id and only know the selection, so they stay hard.
    struct SoftPoint
    {
        uint32_t id;
        Index local;
        float weight;
    };
    std::vector<SoftPoint> soft;
    if (!provided && PrefStore::get().value<bool>("general", "/softSelect/enabled", false))
    {
        std::vector<uint32_t> order;
        std::vector<glm::dvec3> worldPts;
        std::vector<char> picked;
        const std::set<uint32_t> chosen(source.begin(), source.end());
        for (auto& [id, p] : all)
            if (chosen.contains(id) || !pp.soft || pp.soft(id))
            {
                order.push_back(id);
                worldPts.push_back(glm::dvec3(world * glm::dvec4(p, 1.0)));
                picked.push_back(chosen.contains(id) ? 1 : 0);
            }
        const std::vector<float> w = softWeights(worldPts, picked);
        for (size_t k = 0; k < order.size(); ++k)
            if (!picked[k] && w[k] > 0.0f)
                soft.push_back({order[k], m.addPoint(all[order[k]]), w[k]});
    }
    PointTarget targetOf = provided ? provided(g, n, pp, source) : setup(m, world, ComponentMode::Vertex, local);
    if (!targetOf)
        return false;
    std::vector<std::pair<uint32_t, glm::dvec3>> moves;
    for (size_t i = 0; i < source.size(); ++i)
        moves.push_back({source[i], targetOf(provided ? source[i] : local[i], m.position(local[i]))});
    for (const SoftPoint& sp : soft)
    {
        const glm::dvec3 at = m.position(sp.local);
        moves.push_back({sp.id, glm::mix(at, targetOf(sp.local, at), double(sp.weight))});
    }
    pp.setPoints(g, n.id(), moves);
    return true;
}

int deformSelected(const char* label, const PointTargetSetup& setup, const ProvidedSetup& provided, bool allowWhole = false)
{
    LILA_ZONE();

    ComponentMode kind = components::mode();
    Graph& g = graph();
    std::map<NodeId, Ids> selection;
    const bool whole = kind == ComponentMode::Object && allowWhole;
    if (whole)
    {
        kind = ComponentMode::Vertex;
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("mesh"))
                selection[id] = {};
    }
    else if (kind == ComponentMode::Object)
        throw CommandError(std::string(label) + ": no components selected");
    else
        selection = components::selected(kind);
    UndoChunk chunk(label);
    int edited = 0;
    for (auto& [node, ids] : selection)
    {
        Node* n = g.node(node);
        if (const components::PointProvider* pp = n ? components::pointProvider(*n) : nullptr; pp && kind == ComponentMode::Vertex)
        {
            edited += deformProvidedPoints(g, *n, *pp, ids, setup, provided) ? 1 : 0;
            continue;
        }
        if (!n || !n->type().isA("mesh"))
            continue;
        PlugRef base = basePlug(*n);
        geo::Mesh m = baseMesh(base);
        if (whole)
            for (Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p))
                    ids.push_back(p);
        Ids pts = components::convert(m, kind, ids, ComponentMode::Vertex);
        glm::dmat4 world = n->as<glm::dmat4>(Transform::kWorldMatrix);
        PointTarget targetOf = setup(m, world, kind, ids);
        if (!targetOf)
            continue;
        auto sym = symmetryFor(*n, m);

        // With symmetry, the wedge (side) the selection is mostly in drives and the others follow as its exact images.
        int drive = 0;
        if (sym)
        {
            drive = driveWedge(node, *sym, pts);
            pts = sym->expandPoints(pts);
        }
        std::vector<float> weight = softWeights(m, world, pts);
        std::vector<glm::dvec3> before(m.positions().begin(), m.positions().end());
        Ids moved;
        for (Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (!m.pointValid(p) || weight[p] <= 0.0f)
                continue;
            moved.push_back(p);
            if (sym && !sym->drives(p, drive))
                continue;
            m.setPosition(p, glm::mix(before[p], targetOf(p, before[p]), double(weight[p])));
        }
        if (sym)
            sym->enforceWedge(m, moved, drive);
        g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
        ++edited;
    }
    return edited;
}

/** Provided points moved by uvn along their own world frames; points without a frame stay put. */
ProvidedSetup alongProvidedFrames(const glm::dvec3& uvn)
{
    return [uvn](Graph& g, Node& n, const components::PointProvider& pp, const Ids& ids) -> PointTarget {
        if (!pp.frames)
            return {};
        glm::dmat3 linear(n.as<glm::dmat4>(Transform::kWorldMatrix)), toLocal = glm::inverse(linear);
        std::vector<glm::dmat3> frames = pp.frames(g, n.id(), ids);
        auto offsets = std::make_shared<std::map<uint32_t, glm::dvec3>>();
        for (size_t i = 0; i < ids.size() && i < frames.size(); ++i)
            (*offsets)[ids[i]] = toLocal * (worldFrame(frames[i], linear) * uvn);
        return [offsets](uint32_t id, const glm::dvec3& at) {
            auto it = offsets->find(id);
            return it == offsets->end() ? at : at + it->second;
        };
    };
}
} // namespace

int deformSelectedComponents(const char* label, const PointTargetSetup& setup, bool allowWhole)
{
    return deformSelected(label, setup, nullptr, allowWhole);
}

void transformSelectedComponents(const char* label, const glm::dmat4& worldXform)
{
    LILA_ZONE();

    deformSelectedComponents(label, [&](const geo::Mesh&, const glm::dmat4& world, ComponentMode, const Ids&) -> PointTarget {
        glm::dmat4 local = glm::inverse(world) * worldXform * world;
        return [local](Index, const glm::dvec3& p) { return glm::dvec3(local * glm::dvec4(p, 1.0)); };
    });
}

void transformSelectedComponentsOnto(const char* label, const glm::dmat4& worldXform, const SurfaceProjector& onto)
{
    LILA_ZONE();

    deformSelectedComponents(label, [&](const geo::Mesh&, const glm::dmat4& world, ComponentMode, const Ids&) -> PointTarget {
        glm::dmat4 local = glm::inverse(world) * worldXform * world, toLocal = glm::inverse(world);
        return [local, toLocal, world, &onto](Index, const glm::dvec3& p) {
            glm::dvec3 moved(world * local * glm::dvec4(p, 1.0));
            return glm::dvec3(toLocal * glm::dvec4(onto(moved), 1.0));
        };
    });
}

SurfaceProjector closestOnMeshes(const std::vector<NodeId>& meshes)
{
    LILA_ZONE();

    struct Surface
    {
        std::shared_ptr<geo::MeshBvh> bvh;
        glm::dmat4 world, toLocal;
    };
    Graph& g = graph();
    g.evaluate();
    auto surfaces = std::make_shared<std::vector<Surface>>();
    for (NodeId id : meshes)
    {
        Node* n = g.node(id);
        MeshPtr mesh = n && n->type().isA("mesh") ? n->as<MeshPtr>(MeshNode::kOutMesh) : nullptr;
        if (!mesh || mesh->numFaces() == 0)
            continue;
        glm::dmat4 world = n->as<glm::dmat4>(Transform::kWorldMatrix);
        surfaces->push_back({std::make_shared<geo::MeshBvh>(*mesh), world, glm::inverse(world)});
    }
    return [surfaces](const glm::dvec3& p) {
        glm::dvec3 best = p;
        double bestDist = std::numeric_limits<double>::max();
        for (const Surface& s : *surfaces)
        {
            geo::BvhClosest c;
            if (!s.bvh->closestPoint(glm::dvec3(s.toLocal * glm::dvec4(p, 1.0)), c))
                continue;
            glm::dvec3 w(s.world * glm::dvec4(c.point, 1.0));
            if (double d = glm::length(w - p); d < bestDist)
            {
                bestDist = d;
                best = w;
            }
        }
        return best;
    };
}

void offsetSelectedComponents(const char* label, double distance)
{
    LILA_ZONE();

    deformSelected(label, [&](const geo::Mesh& m, const glm::dmat4& world, ComponentMode kind, const Ids& ids) -> PointTarget {
        auto normals = std::make_shared<std::vector<glm::dvec3>>(componentNormals(m, kind, ids));
        glm::dmat3 toWorld = glm::inverseTranspose(glm::dmat3(world)), toLocal = glm::inverse(glm::dmat3(world));
        // The distance is world space, so a scaled object still moves by it.
        return [normals, toWorld, toLocal, distance](Index p, const glm::dvec3& at) {
            glm::dvec3 nw = toWorld * (*normals)[p];
            double len = glm::length(nw);
            return len > 1e-12 ? at + toLocal * (nw / len * distance) : at;
        };
    }, alongProvidedFrames(glm::dvec3(0.0, 0.0, distance)));
}

void offsetSelectedComponents(const char* label, const glm::dvec3& uvn)
{
    LILA_ZONE();

    if (uvn.x == 0.0 && uvn.y == 0.0)
    {
        offsetSelectedComponents(label, uvn.z);
        return;
    }
    const glm::dvec3 ref = tangentReference(selectionNormal());
    deformSelected(label, [&](const geo::Mesh& m, const glm::dmat4& world, ComponentMode kind, const Ids& ids) -> PointTarget {
        glm::dmat3 linear(world), toLocal = glm::inverse(linear);
        auto frames = std::make_shared<std::vector<glm::dmat3>>(componentFrames(m, kind, ids, toLocal * ref));
        return [frames, linear, toLocal, uvn](Index p, const glm::dvec3& at) { return at + toLocal * (worldFrame((*frames)[p], linear) * uvn); };
    }, alongProvidedFrames(uvn));
}

namespace
{
/** polyExtrude duplicate=True: each mesh's selected faces extruded into a new mesh beside it, which ends up selected with its cap. */
json extrudeDuplicate(geo::ExtrudeOptions opts, bool keepSides)
{
    LILA_ZONE();

    Graph& g = graph();
    g.evaluate();
    std::map<NodeId, Ids> targets;
    for (auto& [node, ids] : components::selected(components::mode()))
        if (Node* n = g.node(node); n && n->type().isA("mesh"))
            if (MeshPtr cage = modifiers::cage(g, node))
                if (Ids f = components::convert(*cage, components::mode(), ids, ComponentMode::Face); !f.empty())
                    targets[node] = std::move(f);
    if (targets.empty())
        throw CommandError("polyExtrude: no faces selected");

    UndoChunk chunk("Extrude");
    json out = json::array();
    std::vector<NodeId> made, sources;
    std::map<NodeId, Ids> caps;
    for (auto& [node, sel] : targets)
    {
        Node& n = *g.node(node);
        geo::Mesh m = baseMesh(basePlug(n));
        auto sym = symmetryFor(n, m);
        opts.symmetry = sym ? &*sym : nullptr;
        geo::ExtrudeCopy copy = geo::extrudeCopy(m, sel, opts, keepSides);
        if (copy.cap.empty())
            continue;
        copy.mesh.compact();
        Node* piece = g.createNode(n.type(), n.name(), g.parent(n.id()), false);
        for (int attr : {Transform::kTranslate, Transform::kRotate, Transform::kScale, Transform::kRotatePivot, Transform::kScalePivot,
                         Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate, Transform::kRotateOrder})
            g.setValue({piece->id(), attr, -1}, g.storedValue({n.id(), attr, -1}));
        g.setValue({piece->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(copy.mesh))));
        out.push_back(piece->path());
        made.push_back(piece->id());
        sources.push_back(node);
        caps[piece->id()] = std::move(copy.cap);
    }
    isolation::inherit(made, sources);
    g.select(made);
    // The cap stays selected in face mode, so the extrude handles and the last-op panel keep working on the copy.
    components::keepModeAfterCreate();
    components::setAll({}, {}, caps);
    if (components::mode() != ComponentMode::Face)
        components::setModeUndoable(ComponentMode::Face);
    return out;
}
} // namespace

void registerMeshEditCommands()
{
    using T = ArgType;
    using K = ComponentMode;

    softSelect::registerCommands();

    ops::registerCommand({
        .name = "polyExtrude",
        .doc = "Extrude the selected faces along their normals, or in edge mode grow a strip of faces off the selected edges. "
               "Connected faces extrude as regions unless individual=True.",
        .flags = {{"thickness", "", T::Float, 0.0, "Distance along the normals, cm."},
                  {"offset", "", T::Float, 0.0,
                   "Faces: inward distance of the extruded border, cm; negative grows them. Edges: outward distance from their faces, cm."},
                  {"individual", "", T::Bool, false, "Each face separately."},
                  {"duplicate", "", T::Bool, false, "Faces: leave the mesh as it is and put the extruded faces in a new mesh."},
                  {"keepSides", "", T::Bool, false, "With duplicate, the new mesh also keeps the side faces."}},
        .run = [](const Args& a) -> json {
            selectWholeInObjectMode(K::Face);
            if (a.get<bool>("duplicate") && components::mode() != K::Edge)
            {
                geo::ExtrudeOptions o{a.get<double>("thickness"), a.get<bool>("individual"), nullptr, a.get<double>("offset")};
                return extrudeDuplicate(o, a.get<bool>("keepSides"));
            }
            if (components::mode() == K::Edge)
                return editSelected("Extrude", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                    geo::EdgeExtrudeOptions o{a.get<double>("thickness"), a.get<double>("offset"), s};
                    return edges(geo::extrudeEdges(m, sel, o).edges);
                });
            return editSelected("Extrude", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::ExtrudeOptions o{a.get<double>("thickness"), a.get<bool>("individual"), s, a.get<double>("offset")};
                return faces(geo::extrudeFaces(m, sel, o).faces);
            });
        },
    });

    ops::registerCommand({
        .name = "polyInset",
        .doc = "Inset the selected faces.",
        .flags = {{"thickness", "", T::Float, 5.0, "Inward distance, cm."},
                  {"depth", "", T::Float, 0.0, "Push along the normals afterwards, cm."},
                  {"individual", "", T::Bool, false, "Each face separately."},
                  {"clamp", "", T::Bool, true, "Shrink the thickness where the inset would cross itself, as on narrow faces."}},
        .run = [](const Args& a) -> json {
            selectWholeInObjectMode(K::Face);
            return editSelected("Inset", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::InsetOptions o{a.get<double>("thickness"), a.get<double>("depth"), a.get<bool>("individual"), s,
                                    a.get<bool>("clamp")};
                return faces(geo::insetFaces(m, sel, o).faces);
            });
        },
    });

    ops::registerCommand({
        .name = "polyBevel",
        .doc = "Bevel the selected edges (vertex or face selections convert to their edges).",
        .flags = {{"offset", "", T::Float, 2.0, "Width, cm.", {}, {}, 0.0},
                  {"segments", "", T::Int, 1, "Segments across the bevel.", {}, {}, 1.0, 64.0},
                  {"profile", "", T::Float, 0.5, "0.5 round, 0.25 straight, toward 1 square.", {}, {}, 0.0, 1.0},
                  {"clamp", "", T::Bool, true, "Keep widths from overlapping."},
                  {"smoothingAngle", "", T::Float, 30.0, "New edges sharper than this are hard.", {}, {}, 0.0, 180.0}},
        .run = [](const Args& a) -> json {
            selectWholeInObjectMode(K::Edge);
            double used = a.get<double>("offset");
            json out = editSelected("Bevel", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::BevelOptions o;
                o.offset = a.get<double>("offset");
                o.segments = std::max(1, a.get<int>("segments"));
                o.profile = a.get<double>("profile");
                o.clamp = a.get<bool>("clamp");
                o.smoothingAngle = a.get<double>("smoothingAngle");
                o.symmetry = s;
                double applied = o.offset;
                o.appliedOffset = &applied;
                EditResult r = faces(geo::bevelEdges(m, sel, o));
                used = std::min(used, applied);
                return r;
            });
            if (a.get<bool>("clamp") && used < a.get<double>("offset") - 1e-9)
                lastOp::limitFlag("offset", std::floor(used * 1000.0) / 1000.0);
            return out;
        },
    });

    ops::registerCommand({
        .name = "deleteComponents",
        .doc = "Delete the selected vertices, edges or faces (with whatever depends on them).",
        .run = [](const Args&) -> json {
            K kind = components::mode();
            return editSelected("Delete Components", kind, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                if (kind == K::Vertex)
                    geo::deletePoints(m, sel, s);
                else if (kind == K::Edge)
                    geo::deleteEdges(m, sel, s);
                else
                    geo::deleteFaces(m, sel, s);
                return EditResult{};
            });
        },
    });

    ops::registerCommand({
        .name = "polyInsertLoop",
        .doc = "Insert edge loops across the ring of the first selected edge. Several loops are centred on fraction, spacing apart.",
        .flags = {{"fraction", "", T::Float, 0.5, "Where along the ring, 0 to 1; with several loops, where their middle goes.", {}, {}, 0.0, 1.0},
                  {"count", "", T::Int, 1, "Number of loops.", {}, {}, 1.0},
                  {"spacing", "", T::Float, 1.0, "Several loops: gap between neighbours, 1 even spacing across the edge, smaller packs them together.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            return editSelected("Insert Edge Loop", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                if (sel.empty())
                    return edges({});
                geo::EdgeLoopOptions o{a.get<double>("fraction"), std::max(1, a.get<int>("count")), a.get<double>("spacing")};
                if (!s)
                    return edges(geo::insertEdgeLoop(m, sel.front(), o));
                // One plan for every loop and its images, so a ring that is its own image gets each mirrored spot once.
                geo::KnifePlan plan;
                for (double t : geo::edgeLoopFractions(o))
                    std::ranges::copy(geo::knifeLoopPlan(m, sel.front(), t, s).segments, std::back_inserter(plan.segments));
                return edges(geo::knifeApply(m, plan));
            });
        },
    });

    ops::registerCommand({
        .name = "polySplitRings",
        .doc = "Subdivide one way: insert loops across every ring through the selected edges, or across the selected faces in one "
               "direction, without the perpendicular cuts a full subdivide adds. Returns the new edges.",
        .flags = {{"count", "", T::Int, 1, "Loops per ring, evenly spaced.", {}, {}, 1.0, 32.0},
                  {"direction", "", T::String, "along", "Faces: across adds lines across the selection's short way, along the long way.",
                   {"across", "along"}},
                  {"edgeFlow", "", T::Bool, true, "Curve the new loops to follow the surrounding surface rather than lie flat."}},
        .run = [](const Args& a) -> json {
            // Object mode splits whole meshes, read as every face so direction still picks the way.
            const K mode = components::mode();
            const K kind = mode == K::Face || mode == K::Object ? K::Face : K::Edge;
            const int count = std::max(1, a.get<int>("count"));
            const bool across = a.get<std::string>("direction") == "across";
            const bool flow = a.get<bool>("edgeFlow");
            return editSelected("Split Rings", kind, [&](geo::Mesh& m, const Ids& picked, const geo::SymmetryMap* s) {
                Ids sel = picked;
                if (sel.empty())
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            sel.push_back(f);
                const Ids ringEdges = kind == K::Face ? geo::regionRingEdges(m, sel, across) : sel;
                const Ids made = geo::splitRings(m, ringEdges, count, s);
                if (flow && !made.empty())
                    for (const geo::PointMove& mv : geo::editEdgeFlow(m, made, {}))
                        m.setPosition(mv.point, mv.position);
                return edges(made);
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyOffsetLoop",
        .doc = "Insert an edge loop either side of the selected edges, parallel to them (Maya's Offset Edge Loop). Partial loops end on "
               "a border, or inside the mesh by meeting at the end vertex or wrapping round it. Returns the new edges.",
        .flags = {{"distance", "", T::Float, 0.25, "How far out: a fraction of each crossing edge's length, or cm with absolute.", {}, {}, 0.0},
                  {"absolute", "", T::Bool, false, "distance is in cm, square to the selected edges."},
                  {"side", "", T::String, "both", "both, left or right of the edges' direction.", {"both", "left", "right"}},
                  {"ends", "", T::String, "converge", "Open ends inside the mesh: converge (meet at the end vertex) or wrap (round it).",
                   {"converge", "wrap"}},
                  {"deleteEdge", "", T::Bool, false, "Dissolve the selected edges afterward, leaving the two new loops."}},
        .run = [](const Args& a) -> json {
            return editSelected("Offset Edge Loop", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                const std::string side = a.get<std::string>("side");
                geo::OffsetLoopOptions o;
                o.distance = std::max(0.0, a.get<double>("distance"));
                o.absolute = a.get<bool>("absolute");
                o.side = side == "left" ? geo::OffsetSide::Left : side == "right" ? geo::OffsetSide::Right : geo::OffsetSide::Both;
                o.ends = a.get<std::string>("ends") == "wrap" ? geo::OffsetEnds::Wrap : geo::OffsetEnds::Converge;
                o.symmetry = s;
                return edges(geo::offsetEdgeLoops(m, sel, o, a.get<bool>("deleteEdge")));
            });
        },
    });

    ops::registerCommand({
        .name = "polyMerge",
        .doc = "Weld selected vertices closer than a distance (all of a mesh's points if it has no component selection). "
               "Exactly two selected vertices weld whatever the distance.",
        .flags = {{"distance", "", T::Float, 0.01, "Weld distance, cm.", {}, {}, 0.0}, {"average", "", T::Bool, false, "Move merged points to their average."}},
        .run = [](const Args& a) -> json {
            const bool pair = components::mode() == K::Vertex;
            return editSelected("Merge", K::Vertex, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::MergeOptions o{a.get<double>("distance"), a.get<bool>("average"), s};
                // Just past the pair's own gap, so a mirrored pair welds too without reaching further.
                if (pair && sel.size() == 2 && m.pointValid(sel[0]) && m.pointValid(sel[1]))
                    o.distance = std::max(o.distance, glm::distance(m.position(sel[0]), m.position(sel[1])) * 1.001 + 1e-6);
                return points(geo::mergeByDistance(m, sel, o));
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyMergeCenter",
        .doc = "Weld the selected vertices into one at their centre.",
        .run = [](const Args&) -> json {
            return editSelected("Merge to Center", K::Vertex, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap*) {
                Index p = geo::mergeToCenter(m, sel);
                return points(p == geo::kInvalid ? Ids{} : Ids{p});
            });
        },
    });

    ops::registerCommand({
        .name = "polyTargetWeld",
        .doc = "Target weld: merge one vertex into another, which keeps its place (or both meet halfway with center=), or an "
               "edge into another, end to nearest end. Follows the mesh's symmetry; a weld across the symmetry plane meets on it. "
               "Returns the welded vertices.",
        .flags = {{"mesh", "", T::String, nullptr, "Mesh to weld; default: the selected or hilited mesh."},
                  {"source", "", T::Int, -1, "Vertex (or edge) that moves.", {}, {}, -1.0},
                  {"target", "", T::Int, -1, "Vertex (or edge) it merges into.", {}, {}, -1.0},
                  {"edge", "", T::Bool, false, "source and target are edges."},
                  {"center", "", T::Bool, false, "Meet halfway instead of at the target."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            Node& n = meshFlag(a, "polyTargetWeld");
            PlugRef base = basePlug(n);
            geo::Mesh mesh = baseMesh(base);
            const bool edge = a.get<bool>("edge");
            const Index source = Index(a.get<int>("source")), target = Index(a.get<int>("target"));
            const bool valid = edge ? mesh.edgeValid(source) && mesh.edgeValid(target) : mesh.pointValid(source) && mesh.pointValid(target);
            if (!valid || source == target)
                throw CommandError(std::format("polyTargetWeld: {} has no {} {} and {} to weld", n.path(), edge ? "edges" : "vertices",
                                               a.get<int>("source"), a.get<int>("target")));
            auto sym = symmetryFor(n, mesh);
            std::vector<geo::WeldPair> pairs;
            auto add = [&](Index s, Index t) {
                if (edge)
                    std::ranges::copy(geo::edgeWeldPairs(mesh, s, t), std::back_inserter(pairs));
                else
                    pairs.push_back({s, t});
            };
            add(source, target);
            bool center = a.get<bool>("center");
            if (sym)
            {
                std::set<std::pair<Index, Index>> added{{source, target}};
                for (int k = 1; k < sym->order(); ++k)
                {
                    const Index ms = edge ? sym->imageEdge(mesh, k, source) : sym->image(k, source);
                    const Index mt = edge ? sym->imageEdge(mesh, k, target) : sym->image(k, target);
                    if (ms == target)
                        center = true;
                    else if (ms != geo::kInvalid && mt != geo::kInvalid && added.insert({ms, mt}).second)
                        add(ms, mt);
                }
            }

            UndoChunk chunk("Target Weld");
            std::vector<glm::dvec3> before;
            int drive = 0;
            if (sym)
            {
                before.assign(mesh.positions().begin(), mesh.positions().end());
                drive = sym->majorityWedge(edge ? components::convert(mesh, ComponentMode::Edge, {source}, ComponentMode::Vertex) : Ids{source});
            }
            Ids welded = geo::targetWeld(mesh, pairs, center);
            if (welded.empty())
                throw CommandError("polyTargetWeld: nothing to weld");
            if (sym)
                maintainSymmetry(n, mesh, before, drive);
            geo::MeshRemap remap;
            mesh.compact(&remap);
            const std::vector<Index>* maps[3] = {&remap.points, &remap.edges, &remap.faces};
            std::map<NodeId, Ids> sel[3] = {components::selected(ComponentMode::Vertex), components::selected(ComponentMode::Edge),
                                            components::selected(ComponentMode::Face)};
            for (int k = 0; k < 3; ++k)
                if (auto it = sel[k].find(n.id()); it != sel[k].end())
                    if (Ids kept = remapIds(it->second, *maps[k]); kept.empty())
                        sel[k].erase(it);
                    else
                        it->second = std::move(kept);
            Ids mapped = remapIds(welded, remap.points);
            auto edited = std::make_shared<geo::Mesh>(std::move(mesh));
            g.setValue(base, MeshPtr(edited));
            modifiers::baseRemapped(g, n.id(), remap, *edited);
            objectSets::meshRemapped(g, n.id(), remap);
            components::setAll(sel[0], sel[1], sel[2]);
            return mapped;
        },
    });

    ops::registerCommand({
        .name = "polyCollapse",
        .doc = "Collapse each connected group of selected edges to its centre.",
        .run = [](const Args&) -> json {
            return editSelected("Collapse", K::Edge, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                return points(geo::collapseEdges(m, withMirror(m, s, sel, K::Edge)));
            });
        },
    });

    ops::registerCommand({
        .name = "polyDissolve",
        .doc = "Remove selected edges or vertices by merging the faces around them, cleanly.",
        .run = [](const Args&) -> json {
            K kind = components::mode() == K::Vertex ? K::Vertex : K::Edge;
            return editSelected("Dissolve", kind, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                Ids all = withMirror(m, s, sel, kind);
                return kind == K::Vertex ? faces(geo::dissolvePoints(m, all)) : faces(geo::dissolveEdges(m, all, true));
            });
        },
    });

    ops::registerCommand({
        .name = "polyBridge",
        .doc = "Join selected boundary loops with faces. Several pairs bridge at once; a single border round a slot bridges its two long sides.",
        .flags = {{"divisions", "", T::Int, 0, "Extra rows between the loops.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            return editSelected("Bridge", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap*) {
                return faces(geo::bridgeEdges(m, sel, {std::max(0, a.get<int>("divisions"))}));
            });
        },
    });

    ops::registerCommand({
        .name = "polyPoke",
        .doc = "Fan each selected face into triangles around its centre.",
        .run = [](const Args&) -> json {
            return editSelected("Poke", K::Face, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                return faces(geo::pokeFaces(m, withMirror(m, s, sel, K::Face)));
            });
        },
    });

    ops::registerCommand({
        .name = "polySpinEdge",
        .doc = "Turn each selected edge one step within its two faces.",
        .run = [](const Args&) -> json {
            return editSelected("Spin Edge", K::Edge, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                return edges(geo::spinEdges(m, withMirror(m, s, sel, K::Edge)));
            });
        },
    });

    ops::registerCommand({
        .name = "polyFillHole",
        .doc = "Cap the holes bordered by the selected edges (every hole when nothing is selected).",
        .run = [](const Args&) -> json {
            return editSelected("Fill Hole", K::Edge, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap*) {
                return faces(geo::fillHoles(m, sel));
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyGridFill",
        .doc = "Rebuild triangle fans and other patches as a quad grid inside a ring of quads: select the fans' centre vertices "
               "or the faces. With edges selected, fills the holes they border the same way.",
        .flags = {{"gridSize", "", T::Float, 0.4, "The grid's size against the hole; 1 grids right to the border with no ring.", {}, {}, 0.05, 1.0},
                  {"rotate", "", T::Int, 0, "Steps the grid's corners round the border."}},
        .run = [](const Args& a) -> json {
            geo::GridFillOptions o;
            o.gridSize = std::clamp(a.get<double>("gridSize"), 0.05, 1.0);
            o.rotate = a.get<int>("rotate");
            const K mode = components::mode();
            const K kind = mode == K::Vertex || mode == K::Edge ? mode : K::Face;
            return editSelected("Grid Fill", kind, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                const Ids all = withMirror(m, s, sel, kind);
                if (kind == K::Edge)
                    return faces(geo::gridFillHoles(m, all, o));
                return faces(geo::gridFillFaces(m, kind == K::Vertex ? geo::facesOfPoints(m, all) : all, o));
            });
        },
    });

    ops::registerCommand({
        .name = "polyFlip",
        .doc = "Reverse the winding of the selected faces (the whole mesh in object mode).",
        .run = [](const Args&) -> json {
            return editSelected("Flip Normals", K::Face, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                Ids all = sel;
                if (all.empty())
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            all.push_back(f);
                geo::flipNormals(m, withMirror(m, s, all, K::Face));
                return sel.empty() ? EditResult{} : faces(sel);
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyConform",
        .doc = "Make face winding consistent; closed parts face outward.",
        .run = [](const Args&) -> json {
            return editSelected("Conform", K::Face, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap*) {
                geo::conformNormals(m, sel);
                return EditResult{};
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyTriangulate",
        .doc = "Triangulate the selected faces (the whole mesh in object mode), deterministically, as Unreal will. Concave and "
               "non-planar faces, collinear points and faces touching themselves split without overlaps or slivers, and no "
               "diagonal doubles an edge another face already has. With symmetry on, the mirrored faces split the mirrored way. "
               "A whole mesh with modifiers gets a triangulate modifier on top instead, so the result is triangulated rather "
               "than the cage.",
        .flags = {{"beauty", "", T::Bool, true, "Flip n-gon diagonals toward the largest smallest angle (what display and export use)."},
                  {"quadSplit", "", T::String, "shortest", "Quads split along the shorter diagonal (what display, export and MikkTSpace use) or always from the first corner.", {"shortest", "first"}}},
        .run = [](const Args& a) -> json {
            geo::TriangulateOptions o;
            o.beauty = a.get<bool>("beauty");
            const std::string split = a.get<std::string>("quadSplit", "shortest");
            if (split != "shortest" && split != "first")
                throw CommandError("polyTriangulate: quadSplit must be shortest or first");
            o.quads = split == "first" ? geo::QuadSplit::FirstCorner : geo::QuadSplit::ShortestDiagonal;
            UndoChunk chunk("Triangulate");
            const auto onStack = [](const Node& mesh) {
                Graph& g = graph();
                const std::vector<NodeId> stack = modifiers::stack(g, mesh.id());
                if (stack.empty())
                    return false;
                if (g.node(stack.back())->type().name != "triangulateModifier")
                    ops::execute("addModifier", {{"args", {mesh.path()}}, {"kind", "triangulate"}});
                return true;
            };
            return editSelected("Triangulate", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                Ids all = withMirror(m, s, sel, K::Face);
                if (all.empty())
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            all.push_back(f);
                return faces(geo::triangulateFaces(m, all, o));
            }, true, onStack);
        },
    });

    ops::registerCommand({
        .name = "polyReduce",
        .doc = "Reduce the polygon count of whole meshes while keeping their shape, silhouette, UVs and normals. The result is "
               "triangles (or quads with quads=True). A mesh with modifiers gets a decimate modifier on top instead. Honours "
               "the mesh's symmetry: one half (or one radial wedge) is reduced and mirrored (or repeated).",
        .positionalDoc = "objects",
        .flags = {{"ratio", "", T::Float, 0.5, "Fraction of triangles to keep, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"targetTriangles", "", T::Int, 0, "Triangles to keep; above 0 it overrides ratio.", {}, {}, 0.0},
                  {"keepBorders", "", T::Bool, true, "Open borders stay exactly where they are."},
                  {"keepHardEdges", "", T::Bool, true, "Hard edges stay intact, as UV seams and material borders always do; off lets them collapse for a lower count."},
                  {"symmetry", "", T::String, "auto", "auto (the mesh's symmetry setting), off, x, y or z.", {"auto", "off", "x", "y", "z"}},
                  {"quads", "", T::Bool, false, "Join triangle pairs back into quads where they make good ones."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> nodes;
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n || !n->type().isA("mesh"))
                    throw CommandError("polyReduce: " + name + " is not a mesh");
                nodes.push_back(n->id());
            }
            if (a.objects().empty())
                for (NodeId id : g.selection())
                    if (Node* n = g.node(id); n && n->type().isA("mesh"))
                        nodes.push_back(id);
            if (nodes.empty())
                throw CommandError("polyReduce: select a mesh");
            const std::string sym = a.get<std::string>("symmetry", "auto");
            if (sym != "auto" && sym != "off" && sym != "x" && sym != "y" && sym != "z")
                throw CommandError("polyReduce: symmetry must be auto, off, x, y or z");
            geo::ReduceOptions o;
            o.ratio = std::clamp(a.get<double>("ratio"), 0.0, 1.0);
            o.targetTriangles = Index(std::max(0, a.get<int>("targetTriangles")));
            o.keepBorders = a.get<bool>("keepBorders");
            o.keepHardEdges = a.get<bool>("keepHardEdges");
            o.quads = a.get<bool>("quads");
            UndoChunk chunk("Reduce");
            json out = json::object();
            for (NodeId id : nodes)
            {
                Node& n = *g.node(id);
                const int axis = sym == "auto" ? symmetryAxis(n) : sym == "off" ? -1 : sym[0] - 'x';
                if (!modifiers::stack(g, id).empty())
                {
                    json params = {{"ratio", o.ratio}, {"targetTriangles", int64_t(o.targetTriangles)}, {"keepBorders", o.keepBorders},
                                   {"keepHardEdges", o.keepHardEdges}, {"symmetry", axis + 1}, {"quads", o.quads}};
                    out[n.path()] = ops::execute("addModifier", {{"args", {n.path()}}, {"kind", "decimate"}, {"params", params}});
                    continue;
                }
                PlugRef base = basePlug(n);
                geo::Mesh mesh = baseMesh(base);
                if (const auto spec = symmetrySpec(n); sym == "auto" && spec && spec->radial)
                {
                    // Radial: reduce one wedge with its seams held, then repeat it round the axis.
                    const double tol = symmetryTolerance(mesh);
                    geo::cutToWedge(mesh, *spec->radial, 0, tol);
                    mesh.compact();
                    geo::ReduceOptions wo = o;
                    wo.keepBorders = true;
                    wo.symmetryAxis = -1;
                    wo.targetTriangles = o.targetTriangles / Index(spec->radial->order());
                    geo::ReduceResult r = geo::reduceMesh(mesh, wo);
                    geo::symmetrizeRadialTopology(r.mesh, *spec->radial, 0, tol);
                    out[n.path()] = r.mesh.numFaces();
                    g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
                    continue;
                }
                o.symmetryAxis = axis;
                o.symmetryOffset = symmetryOffset(n, axis);
                o.symmetryTolerance = symmetryTolerance(mesh);
                geo::ReduceResult r = geo::reduceMesh(mesh, o);
                if (r.symmetryIgnored)
                    log::warn("polyReduce {}: faces straddle the symmetry plane, so it was reduced without symmetry", n.path());
                if (axis >= 0 && axis == symmetryAxis(n))
                {
                    geo::removeSymmetryLayers(r.mesh);
                    geo::SymmetryMap::persistent(r.mesh, axis, symmetryTolerance(r.mesh), nullptr, symmetryOffset(n, axis));
                }
                out[n.path()] = r.mesh.numFaces();
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
            }
            components::clear();
            return out;
        },
    });

    ops::registerCommand({
        .name = "polyQuadrangulate",
        .doc = "Join neighbouring triangles into quads.",
        .flags = {{"maxAngle", "", T::Float, 40.0, "Largest angle between the two triangles, degrees.", {}, {}, 0.0, 180.0}},
        .run = [](const Args& a) -> json {
            return editSelected("Quadrangulate", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap*) {
                geo::QuadrangulateOptions o;
                o.maxFaceAngle = a.get<double>("maxAngle");
                return faces(geo::quadrangulateFaces(m, sel, o));
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polySubdivide",
        .doc = "Catmull-Clark subdivide whole meshes (crease-aware).",
        .positionalDoc = "objects",
        .flags = {{"levels", "", T::Int, 1, "Subdivision levels.", {}, {}, 1.0, 5.0},
                  {"keepHardEdges", "", T::Bool, false, "Keep normals split along hard edges; off smooths the shading across them."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            UndoChunk chunk("Subdivide");
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(name, "polySubdivide");
                PlugRef base = basePlug(n);
                geo::SubdivideOptions o;
                o.levels = std::clamp(a.get<int>("levels"), 1, 5);
                o.keepHardEdges = a.get<bool>("keepHardEdges", false);
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(geo::subdivide(baseMesh(base), o))));
            }
            components::clear();
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "polyMirror",
        .doc = "Mirror whole meshes across the plane through the object's or the world's origin facing an axis, welding along "
               "the seam. direction positive keeps the + side and rebuilds the - side from it, cutting faces that cross the "
               "plane; negative does the reverse; both adds a mirrored copy of the whole mesh. flip=True reflects the mesh "
               "across the plane instead, keeping nothing of the original.",
        .positionalDoc = "objects",
        .flags = {{"axis", "", T::String, "x", "x, y or z.", {"x", "y", "z"}},
                  {"direction", "", T::String, "positive", "positive (+ to -), negative (- to +) or both.", {"positive", "negative", "both"}},
                  {"space", "", T::String, "object", "object or world: whose origin and axes the plane uses.", {"object", "world"}},
                  {"weld", "", T::Bool, true, "Weld points on the plane."},
                  {"tolerance", "", T::Float, 0.001, "Points this close to the plane are cut onto it and welded, cm.", {}, {}, 0.0},
                  {"flip", "", T::Bool, false, "Reflect the mesh across the plane rather than adding a mirrored copy: the original is not kept."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::string axis = a.get<std::string>("axis", "x");
            if (axis.size() != 1 || axis[0] < 'x' || axis[0] > 'z')
                throw CommandError("polyMirror: axis must be x, y or z");
            const std::string direction = a.get<std::string>("direction", "positive");
            if (direction != "positive" && direction != "negative" && direction != "both")
                throw CommandError("polyMirror: direction must be positive, negative or both");
            const std::string space = a.get<std::string>("space", "object");
            if (space != "object" && space != "world")
                throw CommandError("polyMirror: space must be object or world");
            if (space == "world")
                g.evaluate();
            UndoChunk chunk("Mirror");
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(name, "polyMirror");
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                if (a.get<bool>("flip"))
                {
                    glm::dmat4 reflect(1.0);
                    reflect[axis[0] - 'x'][axis[0] - 'x'] = -1.0;
                    if (space == "world")
                    {
                        const glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);
                        reflect = glm::inverse(w) * reflect * w;
                    }
                    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
                        if (m.pointValid(p))
                            m.setPosition(p, glm::dvec3(reflect * glm::dvec4(m.position(p), 1.0)));
                    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            m.flipFace(f);
                    g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
                    continue;
                }
                geo::MirrorOptions o;
                o.plane.normal = glm::dvec3(0.0);
                o.plane.normal[axis[0] - 'x'] = 1.0;
                o.direction = direction == "positive" ? geo::MirrorDirection::Positive
                            : direction == "negative" ? geo::MirrorDirection::Negative
                                                      : geo::MirrorDirection::Both;
                if (space == "world")
                    o.space = n.as<glm::dmat4>(Transform::kWorldMatrix);
                o.weld = a.get<bool>("weld");
                o.tolerance = std::max(0.0, a.get<double>("tolerance"));
                geo::mirrorMesh(m, o);
                // The result is exactly symmetric, so its pairs are re-found by position on the next edit.
                geo::removeSymmetryLayers(m);
                m.compact();
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "polySeparate",
        .doc = "Split meshes into one object per connected part. Returns the new names.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            std::vector<NodeId> made;
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(name, "polySeparate");
                g.evaluate();
                std::vector<geo::Mesh> parts = geo::separate(*n.as<MeshPtr>(MeshNode::kOutMesh));
                if (parts.size() < 2)
                    continue;
                NodeId parent = g.parent(n.id());
                for (size_t i = 0; i < parts.size(); ++i)
                {
                    Node* part = g.createNode(n.type(), n.name(), parent, false);
                    for (int attr : {Transform::kTranslate, Transform::kRotate, Transform::kScale, Transform::kRotatePivot, Transform::kScalePivot,
                                     Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate, Transform::kRotateOrder})
                        g.setValue({part->id(), attr, -1}, g.storedValue({n.id(), attr, -1}));
                    g.setValue({part->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(parts[i]))));
                    out.push_back(part->path());
                    made.push_back(part->id());
                }
                isolation::inherit(std::vector<NodeId>(made.end() - std::ptrdiff_t(parts.size()), made.end()), {n.id()});
                g.deleteNode(n.id());
            }
            g.select(made);
            return out;
        },
    });

    ops::registerCommand({
        .name = "polyCombine",
        .doc = "Merge meshes into one object (world space kept). Returns its name.",
        .positionalDoc = "objects",
        .flags = {{"name", "", T::String, nullptr, "Name of the combined mesh."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto names = a.objects();
            if (names.size() < 2)
                throw CommandError("polyCombine: select at least two meshes");
            g.evaluate();
            std::vector<geo::Mesh> worldMeshes;
            for (const std::string& name : names)
            {
                Node& n = meshArg(name, "polyCombine");
                geo::Mesh m = *n.as<MeshPtr>(MeshNode::kOutMesh);
                glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);
                for (Index p = 0; p < m.pointCapacity(); ++p)
                    if (m.pointValid(p))
                        m.setPosition(p, glm::dvec3(w * glm::dvec4(m.position(p), 1.0)));
                // Mirrored transforms flip winding; keep faces pointing out.
                if (glm::determinant(glm::dmat3(w)) < 0.0)
                {
                    Ids all;
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            all.push_back(f);
                    geo::flipNormals(m, all);
                }
                worldMeshes.push_back(std::move(m));
            }
            std::vector<const geo::Mesh*> ptrs;
            for (const geo::Mesh& m : worldMeshes)
                ptrs.push_back(&m);
            geo::Mesh combined = geo::combine(ptrs);
            std::string last = names.back();
            last = last.substr(last.rfind('|') + 1);
            Node* out = g.createNode(*types::find("mesh"), a.get<std::string>("name", last), kNoNode, true);
            g.setValue({out->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(combined))));
            std::vector<NodeId> sources;
            for (const std::string& name : names)
                if (Node* n = g.find(name))
                {
                    sources.push_back(n->id());
                    g.deleteNode(n->id());
                }
            isolation::inherit({out->id()}, sources);
            g.select({out->id()});
            return out->path();
        },
    });

    ops::registerCommand({
        .name = "polyExtract",
        .doc = "Move the selected faces out into new mesh objects (one per source mesh). Returns their names.",
        .run = [](const Args&) -> json {
            Graph& g = graph();
            g.evaluate();
            ComponentMode from = components::mode();
            if (from == K::Object)
                throw CommandError("polyExtract: no faces selected");
            std::map<NodeId, Ids> targets;
            for (auto& [node, ids] : components::selected(from))
                if (Node* n = g.node(node); n && n->type().isA("mesh"))
                    if (MeshPtr cage = modifiers::cage(g, node))
                        if (Ids f = components::convert(*cage, from, ids, K::Face); !f.empty())
                            targets[node] = std::move(f);
            if (targets.empty())
                throw CommandError("polyExtract: no faces selected");
            json out = json::array();
            std::vector<NodeId> made, sources;
            components::clear();
            for (auto& [node, sel] : targets)
            {
                Node& n = *g.node(node);
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                geo::Mesh part = geo::extractFaces(m, sel);
                part.compact();
                geo::deleteFaces(m, sel);
                m.compact();
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
                Node* piece = g.createNode(n.type(), n.name(), g.parent(n.id()), false);
                for (int attr : {Transform::kTranslate, Transform::kRotate, Transform::kScale, Transform::kRotatePivot, Transform::kScalePivot,
                                 Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate, Transform::kRotateOrder})
                    g.setValue({piece->id(), attr, -1}, g.storedValue({n.id(), attr, -1}));
                g.setValue({piece->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(part))));
                out.push_back(piece->path());
                made.push_back(piece->id());
                sources.push_back(node);
            }
            isolation::inherit(made, sources);
            components::setMode(K::Object);
            g.select(made);
            return out;
        },
    });

    ops::registerCommand({
        .name = "polySoftEdge",
        .doc = "Make the selected edges soft or hard by the angle their faces meet at: sharper than angle turns hard, the "
               "rest soft. 180 softens every edge, 0 hardens every edge. In object mode, whole selected meshes.",
        .flags = {{"angle", "", T::Float, 180.0, "Edges whose faces meet sharper than this turn hard, degrees.", {}, {}, 0.0, 180.0}},
        .run = [](const Args& a) -> json {
            const double angle = a.get<double>("angle");
            return editSelected("Soften/Harden Edges", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                const Ids edges = withMirror(m, s, sel, K::Edge);
                if (angle >= 180.0)
                    geo::setSoftEdges(m, edges);
                else if (angle <= 0.0)
                {
                    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
                    auto harden = [&](Index e) {
                        if (m.edgeValid(e))
                            hard[e] = true;
                    };
                    if (edges.empty())
                        for (Index e = 0; e < m.edgeCapacity(); ++e)
                            harden(e);
                    for (Index e : edges)
                        harden(e);
                }
                else
                    geo::setHardEdgesByAngle(m, angle, edges);
                // Stored split normals would hide the change.
                if (m.cornerAttrs().has(geo::attr::kNormal))
                    geo::updateCornerNormals(m);
                return EditResult{};
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyDuplicateFaces",
        .doc = "Copy the selected faces into a separate shell of the same mesh and select the copies.",
        .run = [](const Args&) -> json {
            return editSelected("Duplicate Faces", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::Mesh part = geo::extractFaces(m, withMirror(m, s, sel, K::Face));
                part.compact();
                const Index first = m.faceCapacity();
                m.append(part);
                Ids made;
                for (Index f = first; f < m.faceCapacity(); ++f)
                    if (m.faceValid(f))
                        made.push_back(f);
                return faces(std::move(made));
            });
        },
    });

    ops::registerCommand({
        .name = "polyConnect",
        .doc = "Connect selected vertices that share a face with new edges, or the midpoints of selected edges across their faces.",
        .run = [](const Args&) -> json {
            K kind = components::mode() == K::Vertex ? K::Vertex : K::Edge;
            return editSelected("Connect", kind, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                Ids all = withMirror(m, s, sel, kind);
                Ids pts;
                if (kind == K::Edge)
                {
                    for (Index e : all)
                        if (m.edgeValid(e))
                            pts.push_back(m.splitEdge(e, 0.5));
                }
                else
                    pts = all;
                return edges(geo::connectPoints(m, pts));
            });
        },
    });

    auto vec = [](const json& j) { return glm::dvec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()); };

    ops::registerCommand({
        .name = "moveComponents",
        .doc = "Move the selected components by an offset in world space, or with normal= push each point along its own vertex normal "
               "(a face selection uses only the selected faces' normals, so the region inflates as a whole). uvn= also slides each point "
               "along its own surface tangents: U follows the uv0 u direction where the mesh has uvs (in edge mode, the selected edges), "
               "else the world axis least aligned with the selection's normal, and V completes the frame. Curve points use the curve's frame: "
               "U its tangent (a handle's own direction), N across it within its plane, V out of the plane.",
        .flags = {{"by", "", T::Vec3, nullptr, "Offset, cm."},
                  {"normal", "", T::Float, nullptr, "Distance along the vertex normals, cm."},
                  {"uvn", "", T::Vec3, nullptr, "Distances along each point's own U tangent, V tangent and normal, cm."},
                  {"onto", "", T::List, nullptr, "Meshes whose surface each moved point then lands on, at its closest point."}},
        .run = [vec](const Args& a) -> json {
            if (int(a.has("by")) + int(a.has("normal")) + int(a.has("uvn")) != 1)
                throw CommandError("moveComponents: give one of by=, normal= or uvn=");
            if (a.has("normal"))
            {
                offsetSelectedComponents("Move Components", a.get<double>("normal"));
                return nullptr;
            }
            if (a.has("uvn"))
            {
                offsetSelectedComponents("Move Components", vec(a.flags["uvn"]));
                return nullptr;
            }
            glm::dmat4 by = glm::translate(glm::dmat4(1.0), vec(a.flags["by"]));
            if (a.has("onto"))
            {
                std::vector<NodeId> onto;
                for (const json& name : a.flags["onto"])
                    onto.push_back(meshArg(name.get<std::string>(), "moveComponents").id());
                transformSelectedComponentsOnto("Move Components", by, closestOnMeshes(onto));
                return nullptr;
            }
            transformSelectedComponents("Move Components", by);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "rotateComponents",
        .doc = "Rotate the selected components about a world pivot (default: their centre; with symmetry, a pair clear of the mirror plane turns about each side's own centre). Degrees, XYZ order.",
        .flags = {{"by", "", T::Vec3, nullptr, "Rotation, degrees."}, {"pivot", "", T::Vec3, nullptr, "World pivot."}},
        .run = [vec](const Args& a) -> json {
            if (!a.has("by"))
                throw CommandError("rotateComponents: give by=");
            glm::dvec3 pivot = a.has("pivot") ? vec(a.flags["pivot"]) : symmetricSelectionCenter();
            glm::dmat4 r = eulerToMatrix(vec(a.flags["by"]), RotateOrder::XYZ);
            transformSelectedComponents("Rotate Components",
                                        glm::translate(glm::dmat4(1.0), pivot) * r * glm::translate(glm::dmat4(1.0), -pivot));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "scaleComponents",
        .doc = "Scale the selected components about a world pivot (default: their centre; with symmetry, a pair clear of the mirror plane scales about each side's own centre).",
        .flags = {{"by", "", T::Vec3, nullptr, "Factor per axis: the world axes, or the axes orient= turns them to."},
                  {"pivot", "", T::Vec3, nullptr, "World pivot."},
                  {"orient", "", T::Vec3, nullptr, "Rotation of the scale axes from the world axes, degrees, XYZ order."}},
        .run = [vec](const Args& a) -> json {
            if (!a.has("by"))
                throw CommandError("scaleComponents: give by=");
            glm::dvec3 pivot = a.has("pivot") ? vec(a.flags["pivot"]) : symmetricSelectionCenter();
            const glm::dmat4 r = a.has("orient") ? eulerToMatrix(vec(a.flags["orient"]), RotateOrder::XYZ) : glm::dmat4(1.0);
            transformSelectedComponents("Scale Components", glm::translate(glm::dmat4(1.0), pivot) * r * glm::scale(glm::dmat4(1.0), vec(a.flags["by"])) *
                                                                glm::transpose(r) * glm::translate(glm::dmat4(1.0), -pivot));
            return nullptr;
        },
    });

    // Warns when a mesh just given symmetry is not symmetric about it, so edits will not mirror cleanly. For radial
    // symmetry it also names the count the mesh fits best: a count that does not match the topology round the axis is
    // the usual mistake.
    auto warnIfAsymmetric = [](NodeId id) {
        Graph& g = graph();
        g.evaluate();
        Node* n = g.node(id);
        MeshPtr cage = n ? modifiers::cage(g, id) : nullptr;
        const std::optional<geo::SymmetrySpec> spec = n ? symmetrySpec(*n) : std::nullopt;
        if (!cage || !spec)
            return;
        geo::Mesh m = *cage;
        const double tolerance = symmetryTolerance(m);
        geo::SymmetryMap map = geo::SymmetryMap::persistent(m, *spec, tolerance);
        geo::SymmetryCheck c = map.check(m);
        if (c.unmatched == 0 && c.maxAsymmetry <= tolerance)
            return;
        if (!spec->radial)
        {
            log::warn("{} is not symmetric in {}: {} points without a partner, max asymmetry {:g} cm", n->path(), char('X' + spec->axis), c.unmatched,
                      c.maxAsymmetry);
            return;
        }
        const int best = bestRadialCount(m, *spec->radial, tolerance);
        log::warn("{} does not fit {}: {} points without a full orbit (shown in red), max asymmetry {:g} cm{}", n->path(), symmetryLabel(*spec),
                  c.unmatched, c.maxAsymmetry,
                  best > 0 && best != spec->radial->count() ? std::format("; a count of {} fits its topology best", best) : std::string());
    };

    // The radial settings flags shared by setSymmetry and symmetrizeRadial, applied to a mesh node; true if any changed.
    auto applyRadialFlags = [vec](const Args& a, Node& n, const char* cmd) {
        Graph& g = graph();
        bool changed = false;
        auto set = [&](int attr, const PlugValue& v) {
            if (n.value(attr) != v)
            {
                g.setValue({n.id(), attr, -1}, v);
                changed = true;
            }
        };
        if (a.has("count"))
        {
            const int count = a.get<int>("count");
            if (count < 2 || count > geo::kMaxRadialCount)
                throw CommandError(std::format("{}: count must be 2 to {}", cmd, geo::kMaxRadialCount));
            set(MeshNode::kRadialCount, int64_t(count));
        }
        if (a.has("radialAxis"))
        {
            const std::string axis = a.get<std::string>("radialAxis");
            const int64_t v = axis == "x" ? 0 : axis == "y" ? 1 : axis == "z" ? 2 : axis == "custom" ? 3 : -1;
            if (v < 0)
                throw CommandError(std::string(cmd) + ": radialAxis must be x, y, z or custom");
            set(MeshNode::kRadialAxis, v);
        }
        if (a.has("direction"))
        {
            const glm::dvec3 d = vec(a.flags["direction"]);
            if (glm::length(d) < 1e-12)
                throw CommandError(std::string(cmd) + ": direction must not be zero");
            set(MeshNode::kRadialDirection, d);
            set(MeshNode::kRadialAxis, int64_t(3));
        }
        if (a.has("mirror"))
            set(MeshNode::kRadialMirror, a.get<bool>("mirror"));
        if (a.has("phase"))
            set(MeshNode::kRadialPhase, a.get<double>("phase"));
        return changed;
    };

    const std::vector<FlagDef> radialFlags = {
        {"count", "", T::Int, nullptr, "Radial: how many wedges repeat round the axis, 2 to 64; default: the mesh's own.", {}, {}, 2.0, double(geo::kMaxRadialCount)},
        {"radialAxis", "", T::String, nullptr, "Radial: the axis, x, y, z or custom (direction=); default: the mesh's own.", {"x", "y", "z", "custom"}},
        {"direction", "", T::Vec3, nullptr, "Radial: a custom axis direction, object space."},
        {"mirror", "", T::Bool, nullptr, "Radial: also mirror each wedge across a plane through the axis (dihedral symmetry)."},
        {"phase", "", T::Float, nullptr, "Radial: degrees the wedge boundaries and mirror planes turn about the axis."}};

    std::vector<FlagDef> setSymmetryFlags = {{"axis", "", T::String, "x", "off, x, y, z or radial.", {"off", "x", "y", "z", "radial"}}};
    setSymmetryFlags.insert(setSymmetryFlags.end(), radialFlags.begin(), radialFlags.end());
    ops::registerCommand({
        .name = "setSymmetry",
        .doc = "Object-space symmetry for component edits on meshes: 'off', a mirror plane 'x', 'y' or 'z', or 'radial': count "
               "wedges repeated round radialAxis (with mirror=True also mirrored within each wedge), every edit applied to all of them.",
        .positionalDoc = "objects",
        .flags = setSymmetryFlags,
        .run = [warnIfAsymmetric, applyRadialFlags](const Args& a) -> json {
            std::string axis = a.get<std::string>("axis", "x");
            int64_t v = axis == "off" ? 0 : axis == "x" ? 1 : axis == "y" ? 2 : axis == "z" ? 3 : axis == "radial" ? kSymmetryRadial : -1;
            if (v < 0)
                throw CommandError("setSymmetry: axis must be off, x, y, z or radial");
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(name, "setSymmetry");
                // A custom plane (or radial centre) belongs to the setting it was set on.
                if (v > 0 && v != n.as<int64_t>(MeshNode::kSymmetry) && n.as<int64_t>(MeshNode::kSymmetryPlane) == 1)
                    graph().setValue({n.id(), MeshNode::kSymmetryPlane, -1}, int64_t(0));
                bool changed = v != n.as<int64_t>(MeshNode::kSymmetry);
                if (applyRadialFlags(a, n, "setSymmetry") && v == kSymmetryRadial)
                    changed = true;
                graph().setValue({n.id(), MeshNode::kSymmetry, -1}, v);
                if (v > 0 && changed)
                    warnIfAsymmetric(n.id());
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "setSymmetryPlane",
        .doc = "Set where meshes' symmetry plane lies: through the selected edges or vertices (which must lie flat across an "
               "axis; that axis becomes the symmetry axis), or back through the pivot with pivot=True. With radial symmetry the "
               "radial axis passes through the selection's centre instead. Returns the plane per mesh as [axis, offset], or "
               "['radial', centre].",
        .positionalDoc = "objects",
        .flags = {{"pivot", "", T::Bool, false, "Put the plane (or radial axis) back through the pivot."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            json out = json::object();
            UndoChunk chunk("Set Symmetry Plane");
            auto forget = [&](Node& n) {
                // Pairs stored about the old plane pair the wrong points about the new one.
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                geo::removeSymmetryLayers(m);
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            };
            auto planeOf = [](const Node& n) -> json {
                const std::optional<geo::SymmetrySpec> spec = symmetrySpec(n);
                if (spec && spec->radial)
                {
                    const glm::dvec3 o = spec->radial->origin();
                    return {"radial", {o.x, o.y, o.z}};
                }
                const int axis = std::max(0, symmetryAxis(n));
                return {std::string(1, char('x' + axis)), symmetryOffset(n, axis)};
            };
            if (a.get<bool>("pivot"))
            {
                std::vector<std::string> names = a.objects();
                if (names.empty())
                    for (NodeId id : g.selection())
                        names.push_back(g.path(id));
                for (const std::string& name : names)
                {
                    Node* n = g.find(name);
                    if (!n || !n->type().isA("mesh") || n->as<int64_t>(MeshNode::kSymmetryPlane) == 0)
                        continue;
                    g.setValue({n->id(), MeshNode::kSymmetryPlane, -1}, int64_t(0));
                    forget(*n);
                    out[n->path()] = planeOf(*n);
                }
                return out;
            }
            const ComponentMode from = components::mode();
            if (from == ComponentMode::Object)
                throw CommandError("setSymmetryPlane: select edges or vertices lying on the plane");
            for (auto& [id, ids] : components::selected(from))
            {
                Node* n = g.node(id);
                MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, id) : nullptr;
                if (!cage)
                    continue;
                const geo::Mesh& m = *cage;
                glm::dvec3 lo(1e300), hi(-1e300), sum(0.0);
                int count = 0;
                for (Index p : components::convert(m, from, ids, ComponentMode::Vertex))
                    if (m.pointValid(p))
                    {
                        lo = glm::min(lo, m.position(p)), hi = glm::max(hi, m.position(p)), sum += m.position(p), ++count;
                    }
                if (count == 0)
                    continue;
                if (n->as<int64_t>(MeshNode::kSymmetry) == kSymmetryRadial)
                {
                    // A ring of points round the axis: its centre is where the axis passes.
                    g.setValue({id, MeshNode::kSymmetryPlane, -1}, int64_t(1));
                    g.setValue({id, MeshNode::kRadialCenter, -1}, sum / double(count));
                    forget(*n);
                    out[n->path()] = planeOf(*n);
                    continue;
                }
                // The mesh's own axis when the selection is flat across it, else whichever axis it is flattest across.
                const glm::dvec3 spread = hi - lo;
                int axis = symmetryAxis(*n);
                if (axis < 0 || spread[axis] > symmetryTolerance(m))
                    axis = spread.x <= spread.y && spread.x <= spread.z ? 0 : spread.y <= spread.z ? 1 : 2;
                if (spread[axis] > std::max(symmetryTolerance(m), glm::length(m.bounds().size()) * 1e-3))
                    throw CommandError(std::format("setSymmetryPlane: the selection on {} does not lie flat across an axis", n->path()));
                const double offset = sum[axis] / count;
                g.setValue({id, MeshNode::kSymmetry, -1}, int64_t(axis + 1));
                g.setValue({id, MeshNode::kSymmetryPlane, -1}, int64_t(1));
                g.setValue({id, MeshNode::kSymmetryOffset, -1}, offset);
                forget(*n);
                out[n->path()] = {std::string(1, char('x' + axis)), offset};
            }
            if (out.empty())
                throw CommandError("setSymmetryPlane: select edges or vertices lying on the plane");
            return out;
        },
    });

    ops::registerCommand({
        .name = "toggleSymmetry",
        .doc = "Symmetry off on the meshes if every one already has it on (on axis=, when given), else on for all of them. Non-meshes are skipped. Returns the axis set, or 'off'.",
        .positionalDoc = "objects",
        .flags = {{"axis", "", T::String, nullptr, "x, y, z or radial to turn on with; default: the axis already in use, else the last one picked."}},
        .run = [warnIfAsymmetric](const Args& a) -> json {
            std::vector<NodeId> ids;
            for (const std::string& name : a.objects())
                if (Node* n = graph().find(name); n && n->type().isA("mesh"))
                    ids.push_back(n->id());
            if (ids.empty())
                throw CommandError("toggleSymmetry: no meshes given or selected");
            SymmetryState st = symmetryState(graph(), ids);
            auto stateAxis = [](int axis) { return axis == SymmetryState::kRadialState ? std::string("radial") : std::string(1, char('x' + axis)); };
            std::string axis = a.get<std::string>("axis", "");
            if (axis.empty())
                axis = st.axis >= 0 ? stateAxis(st.axis) : PrefStore::get().value<std::string>("general", "/symmetry/axis", "x");
            if (axis != "x" && axis != "y" && axis != "z" && axis != "radial")
                throw CommandError("toggleSymmetry: axis must be x, y, z or radial");
            const int64_t wanted = axis == "radial" ? kSymmetryRadial : axis[0] - 'x' + 1;
            const bool off = st.on == st.meshes && (!a.has("axis") || (st.axis >= 0 && stateAxis(st.axis) == axis));
            const int64_t v = off ? 0 : wanted;
            for (NodeId id : ids)
            {
                const bool changed = graph().node(id)->as<int64_t>(MeshNode::kSymmetry) != v;
                graph().setValue({id, MeshNode::kSymmetry, -1}, v);
                if (!off && changed)
                    warnIfAsymmetric(id);
            }
            if (!off)
                PrefStore::get().set("general", "/symmetry/axis", axis);
            return off ? "off" : axis;
        },
    });

    auto symmetricMesh = [](const std::string& name, const char* cmd) -> Node& {
        Node& n = meshArg(name, cmd);
        if (!symmetrySpec(n))
            throw CommandError(std::string(cmd) + ": " + name + " has symmetry off");
        return n;
    };
    // Objects named or selected, else the meshes with selected components.
    auto symmetryTargets = [](const Args& a) {
        std::vector<std::string> names = a.objects();
        if (names.empty() && components::mode() != ComponentMode::Object)
            for (auto& [node, ids] : components::selected(components::mode()))
                if (Node* n = graph().node(node))
                    names.push_back(n->path());
        return names;
    };

    ops::registerCommand({
        .name = "symmetryCheck",
        .doc = "Report how symmetric meshes are across their symmetry plane (or round their radial axis): the largest distance "
               "between a point and its partner's reflection (or its orbit's image), and the points without a partner. For "
               "radial symmetry also the unmatched point ids and the count the mesh fits best. Changes nothing.",
        .positionalDoc = "objects",
        .run = [symmetricMesh, symmetryTargets](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            json out = json::object();
            for (const std::string& name : symmetryTargets(a))
            {
                Node& n = symmetricMesh(name, "symmetryCheck");
                MeshPtr cage = modifiers::cage(g, n.id());
                if (!cage)
                    continue;
                geo::Mesh m = *cage;
                const geo::SymmetrySpec spec = *symmetrySpec(n);
                const bool stored = m.pointAttrs().has(symmetryLayer(spec));
                geo::SymmetrySync sync;
                geo::SymmetryMap map = geo::SymmetryMap::persistent(m, spec, symmetryTolerance(m), &sync);
                geo::SymmetryCheck c = map.check(m);
                json r = {{"axis", spec.radial ? std::string("radial") : std::string(1, char('x' + spec.axis))},
                          {"maxAsymmetry", c.maxAsymmetry},
                          {"worstPoint", c.worstPoint == geo::kInvalid ? json(nullptr) : json(c.worstPoint)},
                          {"pairs", c.pairs},
                          {"onPlane", c.onPlane},
                          {"unmatched", c.unmatched},
                          {"stored", stored},
                          {"withoutStoredPartner", sync.topological + sync.byPosition + sync.unmatched}};
                if (spec.radial)
                {
                    r["count"] = spec.radial->count();
                    r["mirror"] = spec.radial->mirror();
                    std::vector<Index> unmatched = map.unmatchedPoints(m);
                    r["unmatchedPoints"] = unmatched;
                    r["bestCount"] = unmatched.empty() ? spec.radial->count() : bestRadialCount(m, *spec.radial, symmetryTolerance(m));
                }
                out[n.path()] = r;
                log::write(LogLevel::Info, std::format("{}: symmetry {} max asymmetry {:g} cm, {} {}, {} {}, {} unmatched", n.path(), symmetryName(spec),
                                                       c.maxAsymmetry, c.pairs, spec.radial ? "orbits" : "pairs", c.onPlane,
                                                       spec.radial ? "fixed" : "on the plane", c.unmatched));
            }
            return out;
        },
    });

    /** The wedge most of the selected components lie in, else the one holding most of the mesh (a single modeled wedge). */
    auto keepWedge = [](const Node& n, const geo::Mesh& m, const geo::RadialSymmetry& r, double tol) {
        std::vector<Index> count(size_t(r.order()), 0);
        auto tally = [&](Index p) {
            if (m.pointValid(p) && r.axisDistance(m.position(p)) > tol)
                ++count[size_t(r.wedgeOf(m.position(p)))];
        };
        const ComponentMode from = components::mode();
        if (from != ComponentMode::Object)
            for (Index p : components::convert(m, from, components::selected(n.id(), from), ComponentMode::Vertex))
                tally(p);
        if (std::ranges::all_of(count, [](Index c) { return c == 0; }))
            for (Index p = 0; p < m.pointCapacity(); ++p)
                tally(p);
        return int(std::max_element(count.begin(), count.end()) - count.begin());
    };

    ops::registerCommand({
        .name = "symmetrize",
        .doc = "Make meshes exactly symmetric: every point on one side of the symmetry plane is copied, reflected, onto its partner, and "
               "on-plane points are put on the plane. With radial symmetry, one wedge's points are copied round the axis onto their orbits.",
        .positionalDoc = "objects",
        .flags = {{"side", "", T::String, "positive", "The side that is kept.", {"positive", "negative"}},
                  {"wedge", "", T::Int, -1, "Radial: the wedge that is kept; -1: the one most of the selected components are in, else 0.", {}, {}, -1.0}},
        .run = [symmetricMesh, symmetryTargets](const Args& a) -> json {
            Graph& g = graph();
            const int drive = a.get<std::string>("side", "positive") == "negative" ? -1 : 1;
            UndoChunk chunk("Symmetrize");
            for (const std::string& name : symmetryTargets(a))
            {
                Node& n = symmetricMesh(name, "symmetrize");
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                auto sym = symmetryFor(n, m);
                if (sym->isRadial())
                {
                    int wedge = a.get<int>("wedge");
                    if (wedge < 0)
                    {
                        const ComponentMode from = components::mode();
                        wedge = from == ComponentMode::Object
                                    ? 0
                                    : sym->majorityWedge(components::convert(m, from, components::selected(n.id(), from), ComponentMode::Vertex));
                    }
                    if (wedge >= sym->order())
                        throw CommandError(std::format("symmetrize: {} has wedges 0 to {}", n.path(), sym->order() - 1));
                    sym->symmetrizeWedge(m, wedge);
                }
                else
                    sym->symmetrize(m, drive);
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            return nullptr;
        },
    });

    /**
     * The side to keep: the one most of the selected components lie on, else the side holding nearly all of the mesh (a
     * half model), else positive.
     */
    auto keepSide = [](const Node& n, const geo::Mesh& m, int axis, double plane, double tol) {
        const ComponentMode from = components::mode();
        double sum = 0.0;
        if (from != ComponentMode::Object)
            for (Index p : components::convert(m, from, components::selected(n.id(), from), ComponentMode::Vertex))
                if (m.pointValid(p))
                    sum += m.position(p)[axis] - plane;
        if (std::abs(sum) > tol)
            return sum > 0.0 ? 1 : -1;
        Index below = 0, above = 0;
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                const double d = m.position(p)[axis] - plane;
                below += d < -tol, above += d > tol;
            }
        return below > 4 * above ? -1 : 1;
    };

    ops::registerCommand({
        .name = "symmetryRebuild",
        .doc = "Rebuild meshes symmetric: the kept side is mirrored across the plane through the object's pivot, replacing the other side, "
               "welded on the plane, and the symmetry pairs are stored again. A mesh with radial symmetry is rebuilt from its kept "
               "wedge instead (see symmetrizeRadial). With pairsOnly, the geometry is left alone and only "
               "the stored pairs are matched again, by position or, for meshes that are no longer symmetric, by topology outward "
               "from the selected edge (which must lie on the plane; any edge for radial symmetry). Returns the points left unmatched per mesh.",
        .positionalDoc = "objects",
        .flags = {{"axis", "", T::String, nullptr, "x, y or z; default: the mesh's symmetry axis, else x."},
                  {"side", "", T::String, "auto", "The side that is kept; auto: the side of the selected components, else the side holding nearly all of the "
                   "mesh, else positive.",
                   {"auto", "positive", "negative"}},
                  {"wedge", "", T::Int, -1, "Radial: the wedge that is kept; -1: the one most of the selected components are in, else the one holding most of the mesh.", {}, {}, -1.0},
                  {"pairsOnly", "", T::Bool, false, "Only match the stored pairs again, leaving the geometry as it is."},
                  {"topological", "", T::Bool, false, "With pairsOnly: match by topology from the selected edge."},
                  {"tolerance", "", T::Float, 0.0, "Weld or match distance, cm (0: scaled to the mesh).", {}, {}, 0.0}},
        .run = [symmetricMesh, symmetryTargets, keepSide, keepWedge](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            const std::string side = a.get<std::string>("side", "auto");
            if (side != "auto" && side != "positive" && side != "negative")
                throw CommandError("symmetryRebuild: side must be auto, positive or negative");
            const std::string axisFlag = a.get<std::string>("axis", "");
            if (!axisFlag.empty() && (axisFlag.size() != 1 || axisFlag[0] < 'x' || axisFlag[0] > 'z'))
                throw CommandError("symmetryRebuild: axis must be x, y or z");
            const bool pairsOnly = a.get<bool>("pairsOnly") || a.get<bool>("topological");
            json out = json::object();
            UndoChunk chunk("Rebuild Symmetry");
            bool rebuilt = false;
            for (const std::string& name : symmetryTargets(a))
            {
                Node& n = pairsOnly && axisFlag.empty() ? symmetricMesh(name, "symmetryRebuild") : meshArg(name, "symmetryRebuild");
                const std::optional<geo::SymmetrySpec> spec = symmetrySpec(n);
                const bool radial = axisFlag.empty() && spec && spec->radial;
                const int axis = !axisFlag.empty() ? axisFlag[0] - 'x' : std::max(0, symmetryAxis(n));
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                const double tol = a.get<double>("tolerance") > 0.0 ? a.get<double>("tolerance") : symmetryTolerance(m);
                geo::SymmetryMap map;
                ComponentMode from = components::mode();
                auto selectedEdge = [&]() {
                    Ids sel = from == ComponentMode::Object ? Ids{} : components::convert(m, from, components::selected(n.id(), from), ComponentMode::Edge);
                    if (sel.empty())
                        throw CommandError(radial ? "symmetryRebuild: select an edge of " + n.path()
                                                  : "symmetryRebuild: select an edge on the symmetry plane of " + n.path());
                    return sel.front();
                };
                if (!pairsOnly && radial)
                {
                    const int wedge = a.get<int>("wedge") >= 0 ? a.get<int>("wedge") : keepWedge(n, m, *spec->radial, tol);
                    map = geo::symmetrizeRadialTopology(m, *spec->radial, wedge, tol);
                    if (m.numFaces() == 0)
                        throw CommandError(std::format("symmetryRebuild: {} has nothing in wedge {}", n.path(), wedge));
                    rebuilt = true;
                }
                else if (!pairsOnly)
                {
                    const double plane = symmetryOffset(n, axis);
                    const int keep = side == "positive" ? 1 : side == "negative" ? -1 : keepSide(n, m, axis, plane, tol);
                    map = geo::symmetrizeTopology(m, axis, keep, tol, plane);
                    if (m.numFaces() == 0)
                        throw CommandError(std::format("symmetryRebuild: {} has nothing on the {} side of the {} plane", n.path(),
                                                       keep > 0 ? "+" : "-", char('X' + axis)));
                    rebuilt = true;
                }
                else if (a.get<bool>("topological"))
                {
                    map = radial ? geo::SymmetryMap::buildRadialTopological(m, selectedEdge(), *spec->radial)
                                 : geo::SymmetryMap::buildTopological(m, selectedEdge(), geo::axisPlane(axis, symmetryOffset(n, axis)));
                    if (map.empty())
                        throw CommandError("symmetryRebuild: the topology of " + n.path() + " is not symmetric about that edge");
                    map.store(m, axis);
                }
                else
                {
                    map = radial ? geo::SymmetryMap::buildRadial(m, *spec->radial, tol) : geo::SymmetryMap::build(m, geo::axisPlane(axis, symmetryOffset(n, axis)), tol);
                    map.store(m, axis);
                }
                out[n.path()] = map.unmatchedCount();
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            if (rebuilt)
                components::clear();
            return out;
        },
    });

    std::vector<FlagDef> symmetrizeRadialFlags = radialFlags;
    symmetrizeRadialFlags.push_back({"wedge", "", T::Int, -1,
                                     "The wedge that is kept; -1: the one most of the selected components are in, else the one holding most of the mesh.", {}, {}, -1.0});
    symmetrizeRadialFlags.push_back({"tolerance", "", T::Float, 0.0, "Weld distance along the seams and the axis, cm (0: scaled to the mesh).", {}, {}, 0.0});
    ops::registerCommand({
        .name = "symmetrizeRadial",
        .doc = "Rebuild meshes exactly radially symmetric: the kept wedge is repeated round the axis (and mirrored within each wedge with "
               "mirror=True), replacing the rest, and welded along the seams and the axis; then the mesh's symmetry is set to radial with "
               "these settings, so every edit after keeps it. Settings default to the mesh's own radial symmetry. Model one wedge and run "
               "this to get the whole. Returns the faces per mesh.",
        .positionalDoc = "objects",
        .flags = symmetrizeRadialFlags,
        .run = [symmetryTargets, applyRadialFlags, keepWedge](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            std::vector<std::string> names = symmetryTargets(a);
            if (names.empty())
                throw CommandError("symmetrizeRadial: select a mesh");
            json out = json::object();
            UndoChunk chunk("Symmetrize Radial");
            for (const std::string& name : names)
            {
                Node& n = meshArg(name, "symmetrizeRadial");
                applyRadialFlags(a, n, "symmetrizeRadial");
                if (n.as<int64_t>(MeshNode::kSymmetry) != kSymmetryRadial)
                {
                    // A custom mirror plane says nothing about where the radial axis goes.
                    if (n.as<int64_t>(MeshNode::kSymmetryPlane) == 1)
                        g.setValue({n.id(), MeshNode::kSymmetryPlane, -1}, int64_t(0));
                    g.setValue({n.id(), MeshNode::kSymmetry, -1}, kSymmetryRadial);
                }
                const geo::SymmetrySpec spec = *symmetrySpec(n);
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                const double tol = a.get<double>("tolerance") > 0.0 ? a.get<double>("tolerance") : symmetryTolerance(m);
                const int wedge = a.get<int>("wedge") >= 0 ? a.get<int>("wedge") : keepWedge(n, m, *spec.radial, tol);
                if (wedge >= spec.radial->order())
                    throw CommandError(std::format("symmetrizeRadial: {} has wedges 0 to {}", n.path(), spec.radial->order() - 1));
                geo::symmetrizeRadialTopology(m, *spec.radial, wedge, tol);
                if (m.numFaces() == 0)
                    throw CommandError(std::format("symmetrizeRadial: {} has nothing in wedge {}", n.path(), wedge));
                out[n.path()] = m.numFaces();
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            components::clear();
            return out;
        },
    });

    ops::registerCommand({
        .name = "polyKnife",
        .doc = "Knife / multi-cut: cut a mesh along a path of points, splitting the faces between them and the edges the path "
               "crosses. Between two points the cut follows the surface under the straight screen line joining them (seen from "
               "eye= or along direction=; without either, along the points' average normal). With loopEdge= it inserts an edge "
               "loop across that edge's quad ring instead, and with deleteEdge= it dissolves that edge. Follows the mesh's "
               "symmetry. Returns the new edges.",
        .flags = {{"mesh", "", T::String, nullptr, "Mesh to cut; default: the selected or hilited mesh."},
                  {"points", "", T::List, nullptr,
                   "Path points: {\"vertex\": id}, {\"edge\": id, \"t\": 0..1 from its first point}, or {\"face\": id, \"position\": "
                   "[x, y, z] world}."},
                  {"closed", "", T::Bool, false, "Also cut from the last point back to the first."},
                  {"through", "", T::Bool, false, "Cut every face under the line, hidden faces too."},
                  {"eye", "", T::Vec3, nullptr, "World eye of a perspective view."},
                  {"direction", "", T::Vec3, nullptr, "World view direction of an orthographic view."},
                  {"loopEdge", "", T::Int, -1, "Insert an edge loop across this edge's ring instead of cutting a path.", {}, {}, -1.0},
                  {"fraction", "", T::Float, 0.5, "Where along loopEdge the loop crosses it, from its first point.", {}, {}, 0.0, 1.0},
                  {"deleteEdge", "", T::Int, -1, "Dissolve this edge, joining its two faces, instead of cutting.", {}, {}, -1.0},
                  {"select", "", T::Bool, true, "Select the new edges."}},
        .run = [vec](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            Node& n = meshFlag(a, "polyKnife");
            const std::string name = n.path();
            PlugRef base = basePlug(n);
            geo::Mesh mesh = baseMesh(base);
            const glm::dmat4 world = n.as<glm::dmat4>(Transform::kWorldMatrix);
            const glm::dmat4 toLocal = glm::inverse(world);
            auto sym = symmetryFor(n, mesh);

            if (const int deleteEdge = a.get<int>("deleteEdge"); deleteEdge >= 0)
            {
                if (!mesh.edgeValid(Index(deleteEdge)))
                    throw CommandError("polyKnife: " + name + " has no edge " + std::to_string(deleteEdge));
                UndoChunk chunk("Delete Edge");
                std::vector<glm::dvec3> before;
                int drive = 0;
                if (sym)
                {
                    before.assign(mesh.positions().begin(), mesh.positions().end());
                    drive = sym->majorityWedge(components::convert(mesh, K::Edge, {Index(deleteEdge)}, K::Vertex));
                }
                geo::dissolveEdges(mesh, withMirror(mesh, sym ? &*sym : nullptr, {Index(deleteEdge)}, K::Edge), true);
                if (sym)
                    maintainSymmetry(n, mesh, before, drive);
                geo::MeshRemap remap;
                mesh.compact(&remap);
                const std::vector<Index>* maps[3] = {&remap.points, &remap.edges, &remap.faces};
                std::map<NodeId, Ids> sel[3] = {components::selected(K::Vertex), components::selected(K::Edge), components::selected(K::Face)};
                for (int k = 0; k < 3; ++k)
                    if (auto it = sel[k].find(n.id()); it != sel[k].end())
                        if (Ids kept = remapIds(it->second, *maps[k]); kept.empty())
                            sel[k].erase(it);
                        else
                            it->second = std::move(kept);
                auto edited = std::make_shared<geo::Mesh>(std::move(mesh));
                g.setValue(base, MeshPtr(edited));
                modifiers::baseRemapped(g, n.id(), remap, *edited);
                objectSets::meshRemapped(g, n.id(), remap);
                components::setAll(sel[0], sel[1], sel[2]);
                return json::array();
            }

            geo::KnifePlan plan;
            const int loopEdge = a.get<int>("loopEdge");
            std::vector<geo::KnifePoint> pts;
            if (loopEdge >= 0)
            {
                if (!mesh.edgeValid(Index(loopEdge)))
                    throw CommandError("polyKnife: " + name + " has no edge " + std::to_string(loopEdge));
                plan = geo::knifeLoopPlan(mesh, Index(loopEdge), a.get<double>("fraction"), sym ? &*sym : nullptr);
            }
            else
            {
                for (const json& p : a.flags.value("points", json::array()))
                {
                    if (!p.is_object())
                        throw CommandError("polyKnife: each point is {vertex:}, {edge:, t:} or {face:, position:}");
                    if (p.contains("vertex"))
                        pts.push_back(geo::KnifePoint::point(p["vertex"].get<Index>()));
                    else if (p.contains("edge"))
                        pts.push_back(geo::KnifePoint::edge(p["edge"].get<Index>(), std::clamp(p.value("t", 0.5), 0.0, 1.0)));
                    else if (p.contains("face") && p.contains("position"))
                        pts.push_back(geo::KnifePoint::face(p["face"].get<Index>(), glm::dvec3(toLocal * glm::dvec4(vec(p["position"]), 1.0))));
                    else
                        throw CommandError("polyKnife: each point is {vertex:}, {edge:, t:} or {face:, position:}");
                }
                if (pts.size() < 2)
                    throw CommandError("polyKnife: give at least two points");
                geo::KnifeOptions o;
                o.closed = a.get<bool>("closed");
                o.through = a.get<bool>("through");
                if (a.has("eye"))
                    o.eye = glm::dvec3(toLocal * glm::dvec4(vec(a.flags["eye"]), 1.0));
                else if (a.has("direction"))
                    o.viewDir = glm::inverse(glm::dmat3(world)) * vec(a.flags["direction"]);
                o.symmetry = sym ? &*sym : nullptr;
                plan = geo::knifePlan(mesh, pts, o);
            }

            UndoChunk chunk("Knife");
            std::vector<glm::dvec3> before;
            int drive = 0;
            if (sym)
            {
                before.assign(mesh.positions().begin(), mesh.positions().end());
                Ids touched;
                for (const geo::KnifeSegment& s : plan.segments)
                    if (!s.mirrored)
                        for (Index p : mesh.facePoints(s.face))
                            touched.push_back(p);
                drive = sym->majorityWedge(touched);
            }
            const Index firstNew = mesh.pointCapacity();
            Ids cut = geo::knifeApply(mesh, plan);
            if (cut.empty())
                throw CommandError("polyKnife: the path crosses no faces it can cut");
            geo::clearSymmetryKeys(mesh, firstNew);
            if (sym)
                maintainSymmetry(n, mesh, before, drive);
            geo::MeshRemap remap;
            mesh.compact(&remap);
            Ids mapped = remapIds(cut, remap.edges);
            auto edited = std::make_shared<geo::Mesh>(std::move(mesh));
            g.setValue(base, MeshPtr(edited));
            modifiers::baseRemapped(g, n.id(), remap, *edited);
            if (a.get<bool>("select"))
            {
                components::setAll({}, {{n.id(), mapped}}, {});
                if (components::mode() == ComponentMode::Object)
                    components::setMode(ComponentMode::Edge);
            }
            return mapped;
        },
    });

    for (const char* name : {"polyExtrude", "polyInset", "polyBevel", "polyInsertLoop", "polySplitRings", "polyOffsetLoop", "polyMerge", "polyBridge", "polyGridFill", "polyQuadrangulate",
                             "polySubdivide", "polyMirror", "polyTriangulate", "polyReduce", "polySoftEdge"})
        ops::setAdjustable(name);
    {
        namespace opt = ops::options;
        opt::Override loop;
        loop.ranges = {{"fraction", {0.0, 1.0, 0.01, ""}}, {"count", {1.0, 32.0, 1.0, ""}}, {"spacing", {0.0, 2.0, 0.05, ""}}};
        opt::setOverride("polyInsertLoop", std::move(loop));
        opt::Override rings;
        rings.ranges = {{"count", {1.0, 32.0, 1.0, ""}}};
        rings.labels = {{"edgeFlow", "Adjust Edge Flow"}};
        opt::setOverride("polySplitRings", std::move(rings));
        opt::Override offset;
        offset.ranges = {{"distance", {0.0, 1e6, 0.01, ""}}};
        offset.labels = {{"absolute", "Distance in cm"}, {"deleteEdge", "Delete Selected Edges"}};
        opt::setOverride("polyOffsetLoop", std::move(offset));
    }
    // Recording the last op is what makes these adjustable, in any host and not only once the UI installs it.
    lastOp::install();
}
} // namespace lila::scene
