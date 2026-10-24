#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Knife.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Reduce.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"
#include "scene/AutoBevel.h"
#include "scene/Components.h"
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
#include <queue>
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

void reportSync(const Node& n, int axis, const geo::SymmetrySync& s, bool hadLayer)
{
    const Index rematched = s.topological + s.byPosition;
    if (rematched == 0)
        return;
    const char axisName = char('X' + axis);
    if (hadLayer)
        log::write(LogLevel::Warning, std::format("{}: symmetry {} re-matched {} points without a stored partner ({} by position), {} unmatched",
                                                  n.path(), axisName, rematched, s.byPosition, s.unmatched));
    else
        log::write(LogLevel::Info, std::format("{}: symmetry {} paired {} points, {} unmatched", n.path(), axisName, rematched, s.unmatched));
}

/** The mesh's persistent symmetry pairs, stored onto it if it has none yet. */
std::optional<geo::SymmetryMap> symmetryFor(const Node& n, geo::Mesh& mesh)
{
    const int axis = symmetryAxis(n);
    if (axis < 0)
        return std::nullopt;
    const bool hadLayer = mesh.pointAttrs().has(geo::symmetryLayerName(axis));
    geo::SymmetrySync sync;
    geo::SymmetryMap map = geo::SymmetryMap::persistent(mesh, axis, symmetryTolerance(mesh), &sync, symmetryOffset(n, axis));
    reportSync(n, axis, sync, hadLayer);
    return map;
}

/**
 * After a topology op: pairs the points it created (both halves were made together) and sets every moved or new
 * point's follower to the exact reflection of its driver.
 */
void maintainSymmetry(const Node& n, geo::Mesh& mesh, const std::vector<glm::dvec3>& before, int drive)
{
    LILA_ZONE();

    const int axis = symmetryAxis(n);
    const Index firstNew = Index(before.size());
    geo::SymmetryMap map = geo::SymmetryMap::persistent(mesh, axis, symmetryTolerance(mesh), nullptr, symmetryOffset(n, axis));
    Ids touched;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && (p >= firstNew || std::memcmp(&before[p], &mesh.position(p), sizeof(glm::dvec3)) != 0))
            touched.push_back(p);
    map.enforce(mesh, touched, drive);
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
    /** Selects the result without leaving the mode the edit started in. */
    bool stayInMode = false;
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
        throw CommandError(std::string(label) + ": select some components first");

    UndoChunk chunk(label);
    std::map<NodeId, Ids> newSel[3] = {components::selected(ComponentMode::Vertex), components::selected(ComponentMode::Edge),
                                       components::selected(ComponentMode::Face)};
    // An op that selects nothing leaves the mode alone.
    ComponentMode resultKind = ComponentMode::Object;
    bool stayInMode = false;
    for (auto& [node, sel] : targets)
    {
        Node& n = *g.node(node);
        PlugRef base = basePlug(n);
        geo::Mesh mesh = baseMesh(base);
        auto sym = symmetryFor(n, mesh);
        std::vector<glm::dvec3> before;
        int drive = 1;
        if (sym)
        {
            before.assign(mesh.positions().begin(), mesh.positions().end());
            drive = sym->majoritySide(components::convert(mesh, kind, sel, ComponentMode::Vertex));
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
            stayInMode = stayInMode || r.stayInMode;
            Ids mapped = remapIds(r.ids, *maps[int(r.kind) - 1]);
            if (mapped.empty())
                newSel[int(r.kind) - 1].erase(node);
            else
                newSel[int(r.kind) - 1][node] = std::move(mapped);
        }
        auto edited = std::make_shared<geo::Mesh>(std::move(mesh));
        g.setValue(base, MeshPtr(edited));
        autoBevel::baseRemapped(g, node, remap, *edited);
        objectSets::meshRemapped(g, node, remap);
    }
    components::setAll(newSel[0], newSel[1], newSel[2]);
    if (resultKind != ComponentMode::Object && !(stayInMode && from != ComponentMode::Object))
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
        MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, node) : nullptr;
        if (!cage)
            continue;
        const geo::Mesh& m = *cage;
        glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
        for (Index p : components::convert(m, kind, ids, ComponentMode::Vertex))
            if (m.pointValid(p))
            {
                sum += glm::dvec3(w * glm::dvec4(m.position(p), 1.0));
                ++count;
            }
    }
    return count ? sum / double(count) : sum;
}

std::vector<float> softWeights(const geo::Mesh& m, const glm::dmat4& world, const Ids& selected)
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
        MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, node) : nullptr;
        if (!cage)
            continue;
        const geo::Mesh& m = *cage;
        glm::dmat3 toWorld = glm::inverseTranspose(glm::dmat3(n->as<glm::dmat4>(Transform::kWorldMatrix)));
        // Runs on every manipulator refresh, so only the selection's own faces are visited.
        glm::dvec3 local(0.0);
        if (kind == ComponentMode::Face)
            for (Index f : ids)
                local += m.faceValid(f) ? m.faceAreaVector(f) : glm::dvec3(0.0);
        else
            for (Index p : components::convert(m, kind, ids, ComponentMode::Vertex))
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
/** Where each point would go at full weight, given its rest position; set up once per mesh. */
using PointTarget = std::function<glm::dvec3(Index, const glm::dvec3&)>;
using TargetSetup = std::function<PointTarget(const geo::Mesh&, const glm::dmat4& world, ComponentMode kind, const Ids& ids)>;

void deformSelectedComponents(const char* label, const TargetSetup& setup)
{
    ComponentMode kind = components::mode();
    if (kind == ComponentMode::Object)
        throw CommandError(std::string(label) + ": select some components first");
    Graph& g = graph();
    UndoChunk chunk(label);
    auto selection = components::selected(kind);
    for (auto& [node, ids] : selection)
    {
        Node* n = g.node(node);
        if (!n || !n->type().isA("mesh"))
            continue;
        PlugRef base = basePlug(*n);
        geo::Mesh m = baseMesh(base);
        Ids pts = components::convert(m, kind, ids, ComponentMode::Vertex);
        glm::dmat4 world = n->as<glm::dmat4>(Transform::kWorldMatrix);
        PointTarget targetOf = setup(m, world, kind, ids);
        auto sym = symmetryFor(*n, m);

        // With symmetry, the side the selection is on drives and the other side follows as its exact reflection.
        int drive = 1;
        if (sym)
        {
            drive = sym->majoritySide(pts);
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
            if (sym && sym->side(p) == -drive)
                continue;
            m.setPosition(p, glm::mix(before[p], targetOf(p, before[p]), double(weight[p])));
        }
        if (sym)
            sym->enforce(m, moved, drive);
        g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    }
}
} // namespace

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

    deformSelectedComponents(label, [&](const geo::Mesh& m, const glm::dmat4& world, ComponentMode kind, const Ids& ids) -> PointTarget {
        auto normals = std::make_shared<std::vector<glm::dvec3>>(componentNormals(m, kind, ids));
        glm::dmat3 toWorld = glm::inverseTranspose(glm::dmat3(world)), toLocal = glm::inverse(glm::dmat3(world));
        // The distance is world space, so a scaled object still moves by it.
        return [normals, toWorld, toLocal, distance](Index p, const glm::dvec3& at) {
            glm::dvec3 nw = toWorld * (*normals)[p];
            double len = glm::length(nw);
            return len > 1e-12 ? at + toLocal * (nw / len * distance) : at;
        };
    });
}

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
                  {"individual", "", T::Bool, false, "Each face separately."}},
        .run = [](const Args& a) -> json {
            selectWholeInObjectMode(K::Face);
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
                  {"individual", "", T::Bool, false, "Each face separately."}},
        .run = [](const Args& a) -> json {
            selectWholeInObjectMode(K::Face);
            return editSelected("Inset", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::InsetOptions o{a.get<double>("thickness"), a.get<double>("depth"), a.get<bool>("individual"), s};
                return faces(geo::insetFaces(m, sel, o).faces);
            });
        },
    });

    ops::registerCommand({
        .name = "polyBevel",
        .doc = "Bevel the selected edges (vertex or face selections convert to their edges).",
        .flags = {{"offset", "", T::Float, 2.0, "Width, cm."},
                  {"segments", "", T::Int, 1, "Segments across the bevel."},
                  {"profile", "", T::Float, 0.5, "0.5 round, 0.25 straight, toward 1 square."},
                  {"clamp", "", T::Bool, true, "Keep widths from overlapping."},
                  {"smoothingAngle", "", T::Float, 30.0, "New edges sharper than this are hard."}},
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
                r.stayInMode = true;
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
        .doc = "Insert edge loops across the ring of the first selected edge.",
        .flags = {{"fraction", "", T::Float, 0.5, "Where along the ring, 0 to 1."}, {"count", "", T::Int, 1, "Evenly spaced loops."}},
        .run = [](const Args& a) -> json {
            return editSelected("Insert Edge Loop", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                Ids out;
                if (sel.empty())
                    return edges(out);
                geo::EdgeLoopOptions o{a.get<double>("fraction"), std::max(1, a.get<int>("count"))};
                if (s && o.count == 1)
                    return edges(geo::knifeApply(m, geo::knifeLoopPlan(m, sel.front(), o.fraction, s)));
                // Evenly spaced loops on a ring that crosses the plane are symmetric already; any other ring gets its mirror's loops too.
                Index mirror = s ? s->mirrorEdge(m, sel.front()) : geo::kInvalid;
                if (mirror != geo::kInvalid && std::ranges::count(geo::edgeRing(m, sel.front()), mirror))
                    mirror = geo::kInvalid;
                out = geo::insertEdgeLoop(m, sel.front(), o);
                if (mirror != geo::kInvalid && m.edgeValid(mirror))
                    std::ranges::copy(geo::insertEdgeLoop(m, mirror, o), std::back_inserter(out));
                return edges(out);
            });
        },
    });

    ops::registerCommand({
        .name = "polyMerge",
        .doc = "Weld selected vertices closer than a distance (all of a mesh's points if it has no component selection). "
               "Exactly two selected vertices weld whatever the distance.",
        .flags = {{"distance", "", T::Float, 0.01, "Weld distance, cm."}, {"average", "", T::Bool, false, "Move merged points to their average."}},
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
        .doc = "Join two selected boundary loops with faces.",
        .flags = {{"divisions", "", T::Int, 0, "Extra rows between the loops."}},
        .run = [](const Args& a) -> json {
            return editSelected("Bridge", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap*) {
                return faces(geo::bridgeEdgeLoops(m, sel, {std::max(0, a.get<int>("divisions"))}));
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
               "the mesh's symmetry: one half is reduced and mirrored.",
        .positionalDoc = "objects",
        .flags = {{"ratio", "", T::Float, 0.5, "Fraction of triangles to keep, 0 to 1."},
                  {"targetTriangles", "", T::Int, 0, "Triangles to keep; above 0 it overrides ratio."},
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
                o.symmetryAxis = axis;
                o.symmetryOffset = symmetryOffset(n, axis);
                o.symmetryTolerance = symmetryTolerance(mesh);
                geo::ReduceResult r = geo::reduceMesh(mesh, o);
                if (r.symmetryIgnored)
                    log::warn("polyReduce {}: faces straddle the symmetry plane, so it was reduced without symmetry", n.path());
                if (axis >= 0 && axis == symmetryAxis(n))
                {
                    r.mesh.pointAttrs().remove(geo::symmetryLayerName(axis));
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
        .flags = {{"maxAngle", "", T::Float, 40.0, "Largest angle between the two triangles, degrees."}},
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
        .flags = {{"levels", "", T::Int, 1, "Subdivision levels."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            UndoChunk chunk("Subdivide");
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(name, "polySubdivide");
                PlugRef base = basePlug(n);
                geo::SubdivideOptions o;
                o.levels = std::clamp(a.get<int>("levels"), 1, 5);
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
                  {"tolerance", "", T::Float, 0.001, "Points this close to the plane are cut onto it and welded, cm."},
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
                for (int k = 0; k < 3; ++k)
                    m.pointAttrs().remove(geo::symmetryLayerName(k));
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
            Node* out = g.createNode(*types::find("mesh"), a.get<std::string>("name", names.back()), kNoNode, true);
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
                throw CommandError("polyExtract: select some faces first");
            std::map<NodeId, Ids> targets;
            for (auto& [node, ids] : components::selected(from))
                if (Node* n = g.node(node); n && n->type().isA("mesh"))
                    if (MeshPtr cage = modifiers::cage(g, node))
                        if (Ids f = components::convert(*cage, from, ids, K::Face); !f.empty())
                            targets[node] = std::move(f);
            if (targets.empty())
                throw CommandError("polyExtract: select some faces first");
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
        .flags = {{"angle", "", T::Float, 180.0, "Edges whose faces meet sharper than this turn hard, degrees."}},
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

    // With symmetry, a selection clear of the mirror plane (a pair of legs) pivots on the driving side's own centre, so
    // each side scales and turns in place instead of about the plane.
    auto symmetricPivot = []() -> glm::dvec3 {
        Graph& g = graph();
        g.evaluate();
        ComponentMode kind = components::mode();
        glm::dvec3 sum(0.0);
        size_t count = 0;
        for (auto& [node, ids] : components::selected(kind))
        {
            Node* n = g.node(node);
            if (!n || !n->type().isA("mesh") || kind == K::Object)
                continue;
            geo::Mesh m = baseMesh(basePlug(*n));
            Ids pts = components::convert(m, kind, ids, K::Vertex);
            auto sym = symmetryFor(*n, m);
            if (sym && std::none_of(pts.begin(), pts.end(), [&](Index p) { return sym->onPlane(p); }))
            {
                int drive = sym->majoritySide(pts);
                std::erase_if(pts, [&](Index p) { return sym->side(p) != drive; });
            }
            glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
            for (Index p : pts)
                if (m.pointValid(p))
                {
                    sum += glm::dvec3(w * glm::dvec4(m.position(p), 1.0));
                    ++count;
                }
        }
        return count ? sum / double(count) : selectionCenter();
    };

    ops::registerCommand({
        .name = "moveComponents",
        .doc = "Move the selected components by an offset in world space, or with normal= push each point along its own vertex normal "
               "(a face selection uses only the selected faces' normals, so the region inflates as a whole).",
        .flags = {{"by", "", T::Vec3, nullptr, "Offset, cm."},
                  {"normal", "", T::Float, nullptr, "Distance along the vertex normals, cm."},
                  {"onto", "", T::List, nullptr, "Meshes whose surface each moved point then lands on, at its closest point."}},
        .run = [vec](const Args& a) -> json {
            if (a.has("by") == a.has("normal"))
                throw CommandError("moveComponents: give by= or normal=");
            if (a.has("normal"))
            {
                offsetSelectedComponents("Move Components", a.get<double>("normal"));
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
        .run = [vec, symmetricPivot](const Args& a) -> json {
            if (!a.has("by"))
                throw CommandError("rotateComponents: give by=");
            glm::dvec3 pivot = a.has("pivot") ? vec(a.flags["pivot"]) : symmetricPivot();
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
        .run = [vec, symmetricPivot](const Args& a) -> json {
            if (!a.has("by"))
                throw CommandError("scaleComponents: give by=");
            glm::dvec3 pivot = a.has("pivot") ? vec(a.flags["pivot"]) : symmetricPivot();
            const glm::dmat4 r = a.has("orient") ? eulerToMatrix(vec(a.flags["orient"]), RotateOrder::XYZ) : glm::dmat4(1.0);
            transformSelectedComponents("Scale Components", glm::translate(glm::dmat4(1.0), pivot) * r * glm::scale(glm::dmat4(1.0), vec(a.flags["by"])) *
                                                                glm::transpose(r) * glm::translate(glm::dmat4(1.0), -pivot));
            return nullptr;
        },
    });

    // Warns when a mesh just given symmetry is not symmetric about its plane, so edits will not mirror cleanly.
    auto warnIfAsymmetric = [](NodeId id) {
        Graph& g = graph();
        g.evaluate();
        Node* n = g.node(id);
        MeshPtr cage = n ? modifiers::cage(g, id) : nullptr;
        const int axis = n ? symmetryAxis(*n) : -1;
        if (!cage || axis < 0)
            return;
        geo::Mesh m = *cage;
        const double tolerance = symmetryTolerance(m);
        geo::SymmetryMap map = geo::SymmetryMap::persistent(m, axis, tolerance, nullptr, symmetryOffset(*n, axis));
        geo::SymmetryCheck c = map.check(m);
        if (c.unmatched > 0 || c.maxAsymmetry > tolerance)
            log::warn("{} is not symmetric in {}: {} points without a partner, max asymmetry {:g} cm", n->path(), char('X' + axis), c.unmatched,
                         c.maxAsymmetry);
    };

    ops::registerCommand({
        .name = "setSymmetry",
        .doc = "Object-space mirror plane for component edits on meshes: 'off', 'x', 'y' or 'z'.",
        .positionalDoc = "objects",
        .flags = {{"axis", "", T::String, "x", "off, x, y or z."}},
        .run = [warnIfAsymmetric](const Args& a) -> json {
            std::string axis = a.get<std::string>("axis", "x");
            int64_t v = axis == "off" ? 0 : axis == "x" ? 1 : axis == "y" ? 2 : axis == "z" ? 3 : -1;
            if (v < 0)
                throw CommandError("setSymmetry: axis must be off, x, y or z");
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(name, "setSymmetry");
                // A custom plane belongs to the axis it was set on.
                if (v > 0 && v != n.as<int64_t>(MeshNode::kSymmetry) && n.as<int64_t>(MeshNode::kSymmetryPlane) == 1)
                    graph().setValue({n.id(), MeshNode::kSymmetryPlane, -1}, int64_t(0));
                const bool changed = v != n.as<int64_t>(MeshNode::kSymmetry);
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
               "axis; that axis becomes the symmetry axis), or back through the pivot with pivot=True. Returns the plane "
               "per mesh as [axis, offset].",
        .positionalDoc = "objects",
        .flags = {{"pivot", "", T::Bool, false, "Put the plane back through the pivot."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            json out = json::object();
            UndoChunk chunk("Set Symmetry Plane");
            auto forget = [&](Node& n) {
                // Pairs stored about the old plane pair the wrong points about the new one.
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                for (int k = 0; k < 3; ++k)
                    m.pointAttrs().remove(geo::symmetryLayerName(k));
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
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
                    const int axis = std::max(0, symmetryAxis(*n));
                    out[n->path()] = {std::string(1, char('x' + axis)), symmetryOffset(*n, axis)};
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
        .flags = {{"axis", "", T::String, nullptr, "x, y or z to turn on with; default: the axis already in use, else the last one picked."}},
        .run = [warnIfAsymmetric](const Args& a) -> json {
            std::vector<NodeId> ids;
            for (const std::string& name : a.objects())
                if (Node* n = graph().find(name); n && n->type().isA("mesh"))
                    ids.push_back(n->id());
            if (ids.empty())
                throw CommandError("toggleSymmetry: no meshes given or selected");
            SymmetryState st = symmetryState(graph(), ids);
            std::string axis = a.get<std::string>("axis", "");
            if (axis.empty())
                axis = st.axis >= 0 ? std::string(1, char('x' + st.axis)) : PrefStore::get().value<std::string>("general", "/symmetry/axis", "x");
            if (axis != "x" && axis != "y" && axis != "z")
                throw CommandError("toggleSymmetry: axis must be x, y or z");
            const bool off = st.on == st.meshes && (!a.has("axis") || st.axis == axis[0] - 'x');
            const int64_t v = off ? 0 : axis[0] - 'x' + 1;
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
        if (symmetryAxis(n) < 0)
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
        .doc = "Report how symmetric meshes are across their symmetry plane: the largest distance between a point and its partner's "
               "reflection, and the points without a partner. Changes nothing.",
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
                const int axis = symmetryAxis(n);
                const bool stored = m.pointAttrs().has(geo::symmetryLayerName(axis));
                geo::SymmetrySync sync;
                geo::SymmetryMap map = geo::SymmetryMap::persistent(m, axis, symmetryTolerance(m), &sync, symmetryOffset(n, axis));
                geo::SymmetryCheck c = map.check(m);
                out[n.path()] = {{"axis", std::string(1, char('x' + axis))},
                                 {"maxAsymmetry", c.maxAsymmetry},
                                 {"worstPoint", c.worstPoint == geo::kInvalid ? json(nullptr) : json(c.worstPoint)},
                                 {"pairs", c.pairs},
                                 {"onPlane", c.onPlane},
                                 {"unmatched", c.unmatched},
                                 {"stored", stored},
                                 {"withoutStoredPartner", sync.topological + sync.byPosition + sync.unmatched}};
                log::write(LogLevel::Info, std::format("{}: symmetry {} max asymmetry {:g} cm, {} pairs, {} on the plane, {} unmatched", n.path(),
                                                       char('X' + axis), c.maxAsymmetry, c.pairs, c.onPlane, c.unmatched));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "symmetrize",
        .doc = "Make meshes exactly symmetric: every point on one side of the symmetry plane is copied, reflected, onto its partner, and "
               "on-plane points are put on the plane.",
        .positionalDoc = "objects",
        .flags = {{"side", "", T::String, "positive", "The side that is kept.", {"positive", "negative"}}},
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
               "welded on the plane, and the symmetry pairs are stored again. With pairsOnly, the geometry is left alone and only "
               "the stored pairs are matched again, by position or, for meshes that are no longer symmetric, by topology outward "
               "from the selected edge (which must lie on the plane). Returns the points left unmatched per mesh.",
        .positionalDoc = "objects",
        .flags = {{"axis", "", T::String, nullptr, "x, y or z; default: the mesh's symmetry axis, else x."},
                  {"side", "", T::String, "auto", "The side that is kept; auto: the side of the selected components, else the side holding nearly all of the "
                   "mesh, else positive.",
                   {"auto", "positive", "negative"}},
                  {"pairsOnly", "", T::Bool, false, "Only match the stored pairs again, leaving the geometry as it is."},
                  {"topological", "", T::Bool, false, "With pairsOnly: match by topology from the selected edge."},
                  {"tolerance", "", T::Float, 0.0, "Weld or match distance, cm (0: scaled to the mesh)."}},
        .run = [symmetricMesh, symmetryTargets, keepSide](const Args& a) -> json {
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
                const int axis = !axisFlag.empty() ? axisFlag[0] - 'x' : std::max(0, symmetryAxis(n));
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                const double tol = a.get<double>("tolerance") > 0.0 ? a.get<double>("tolerance") : symmetryTolerance(m);
                geo::SymmetryMap map;
                if (!pairsOnly)
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
                    ComponentMode from = components::mode();
                    Ids sel = from == ComponentMode::Object ? Ids{} : components::convert(m, from, components::selected(n.id(), from), ComponentMode::Edge);
                    if (sel.empty())
                        throw CommandError("symmetryRebuild: select an edge on the symmetry plane of " + n.path());
                    map = geo::SymmetryMap::buildTopological(m, sel.front(), geo::axisPlane(axis, symmetryOffset(n, axis)));
                    if (map.empty())
                        throw CommandError("symmetryRebuild: the topology of " + n.path() + " is not symmetric about that edge");
                    map.store(m, axis);
                }
                else
                {
                    map = geo::SymmetryMap::build(m, geo::axisPlane(axis, symmetryOffset(n, axis)), tol);
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

    ops::registerCommand({
        .name = "polyKnife",
        .doc = "Knife / multi-cut: cut a mesh along a path of points, splitting the faces between them and the edges the path "
               "crosses. Between two points the cut follows the surface under the straight screen line joining them (seen from "
               "eye= or along direction=; without either, along the points' average normal). With loopEdge= it inserts an edge "
               "loop across that edge's quad ring instead. Follows the mesh's symmetry. Returns the new edges.",
        .flags = {{"mesh", "", T::String, nullptr, "Mesh to cut; default: the selected or hilited mesh."},
                  {"points", "", T::List, nullptr,
                   "Path points: {\"vertex\": id}, {\"edge\": id, \"t\": 0..1 from its first point}, or {\"face\": id, \"position\": "
                   "[x, y, z] world}."},
                  {"closed", "", T::Bool, false, "Also cut from the last point back to the first."},
                  {"through", "", T::Bool, false, "Cut every face under the line, hidden faces too."},
                  {"eye", "", T::Vec3, nullptr, "World eye of a perspective view."},
                  {"direction", "", T::Vec3, nullptr, "World view direction of an orthographic view."},
                  {"loopEdge", "", T::Int, -1, "Insert an edge loop across this edge's ring instead of cutting a path."},
                  {"fraction", "", T::Float, 0.5, "Where along loopEdge the loop crosses it, from its first point."},
                  {"select", "", T::Bool, true, "Select the new edges."}},
        .run = [vec](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            std::string name = a.get<std::string>("mesh", "");
            if (name.empty())
            {
                std::vector<NodeId> candidates = g.selection();
                for (auto& [node, ids] : components::selected(components::mode()))
                    candidates.push_back(node);
                for (NodeId id : candidates)
                    if (Node* n = g.node(id); n && n->type().isA("mesh"))
                    {
                        name = n->path();
                        break;
                    }
                if (name.empty())
                    throw CommandError("polyKnife: give mesh= or select a mesh");
            }
            Node& n = meshArg(name, "polyKnife");
            PlugRef base = basePlug(n);
            geo::Mesh mesh = baseMesh(base);
            const glm::dmat4 world = n.as<glm::dmat4>(Transform::kWorldMatrix);
            const glm::dmat4 toLocal = glm::inverse(world);
            auto sym = symmetryFor(n, mesh);

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
            int drive = 1;
            if (sym)
            {
                before.assign(mesh.positions().begin(), mesh.positions().end());
                Ids touched;
                for (const geo::KnifeSegment& s : plan.segments)
                    if (!s.mirrored)
                        for (Index p : mesh.facePoints(s.face))
                            touched.push_back(p);
                drive = sym->majoritySide(touched);
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
            autoBevel::baseRemapped(g, n.id(), remap, *edited);
            if (a.get<bool>("select"))
            {
                components::setAll({}, {{n.id(), mapped}}, {});
                components::setMode(ComponentMode::Edge);
            }
            return mapped;
        },
    });

    for (const char* name : {"polyExtrude", "polyInset", "polyBevel", "polyInsertLoop", "polyMerge", "polyBridge", "polyQuadrangulate",
                             "polySubdivide", "polyMirror", "polyTriangulate", "polyReduce", "polySoftEdge"})
        ops::setAdjustable(name);
}
} // namespace lila::scene
