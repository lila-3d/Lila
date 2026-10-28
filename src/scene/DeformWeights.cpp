#include "scene/DeformWeights.h"

#include "core/Profile.h"
#include "deform/Deform.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Deformers.h"
#include "scene/Fields.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/ObjectSets.h"
#include "scene/RigUtil.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>

namespace lila::scene
{
namespace correction
{
void registerType();
void registerCommands();
} // namespace correction

using namespace rigutil;

json PointIds::toJson() const { return {{"kind", kind()}, {"points", points}}; }

DataPtr PointIds::fromJson(const json& j)
{
    auto out = std::make_shared<PointIds>();
    for (const json& p : j.value("points", json::array()))
        if (p.is_number_integer() || p.is_number_unsigned())
            out->points.push_back(p.get<uint32_t>());
    std::ranges::sort(out->points);
    out->points.erase(std::unique(out->points.begin(), out->points.end()), out->points.end());
    return out;
}

namespace deformWeights
{
namespace
{
Graph& g() { return Graph::current(); }

std::vector<glm::dvec3> worldPoints(const geo::Mesh& m, const glm::dmat4& w)
{
    std::vector<glm::dvec3> out(m.pointCapacity());
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        out[p] = glm::dvec3(w * glm::dvec4(m.position(p), 1.0));
    return out;
}

geo::Mesh inWorld(const geo::Mesh& m, const glm::dmat4& w)
{
    geo::Mesh out = m;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        out.setPosition(p, glm::dvec3(w * glm::dvec4(m.position(p), 1.0)));
    return out;
}

MeshPtr deformerInput(Graph& graph, NodeId d)
{
    graph.evaluate();
    return std::get<MeshPtr>(graph.value({d, DeformerNode::kInMesh, -1}));
}

/** The field feeding a deformer, kNoNode when none. */
NodeId fieldOf(Graph& graph, NodeId deformer)
{
    auto src = graph.source({deformer, DeformerNode::kField, -1});
    return src && graph.node(src->node) && fields::isField(*graph.node(src->node)) ? src->node : kNoNode;
}

/**
 * The deformers a command works on: the deformers named, else the top deformer of each mesh named (the selection
 * when nothing is given).
 */
std::vector<NodeId> deformersOf(const std::vector<std::string>& names, const char* cmd)
{
    Graph& graph = g();
    std::vector<NodeId> out, fromMeshes;
    for (const std::string& name : names)
    {
        Node& n = nodeNamed(name, cmd);
        if (deformers::isDeformer(n))
            out.push_back(n.id());
        else if (n.type().isA("mesh"))
            if (auto list = deformers::list(graph, n.id()); !list.empty())
                fromMeshes.push_back(list.back());
    }
    if (out.empty())
        out = fromMeshes;
    if (out.empty())
        throw CommandError(std::string(cmd) + ": give or select a deformer (or a mesh with one)");
    return out;
}

/** The map a command works on: a deformer's weights, or with layer= (or a field named) a field's painted layer. */
MapTarget targetOf(const Args& a, const char* cmd)
{
    Graph& graph = g();
    auto objs = a.objects();
    NodeId mesh = kNoNode;
    if (a.has("mesh"))
        mesh = nodeNamed(a.get<std::string>("mesh"), cmd).id();
    for (const std::string& o : objs)
        if (Node* n = graph.find(o); n && fields::isField(*n))
            return guarded([&] { return resolve(graph, n->id(), int(a.get<int64_t>("layer", -1)), mesh); });
    NodeId d = deformersOf(objs, cmd).front();
    if (a.has("layer"))
    {
        NodeId f = fieldOf(graph, d);
        if (f == kNoNode)
            throw CommandError(std::string(cmd) + ": " + graph.node(d)->name() + " reads no field");
        return resolve(graph, f, int(a.get<int64_t>("layer")), modifiers::meshOf(graph, d));
    }
    return resolve(graph, d);
}

/** Points from points=, set= (its components on the target's mesh) or the selected components; empty means all. */
std::vector<uint32_t> pointsArg(Graph& graph, const Args& a, NodeId mesh, const char* cmd)
{
    std::vector<uint32_t> out;
    if (a.has("points"))
        out = a.flags["points"].get<std::vector<uint32_t>>();
    else if (a.has("set"))
    {
        Node& set = nodeNamed(a.get<std::string>("set"), cmd);
        if (!objectSets::isSet(set))
            throw CommandError(std::string(cmd) + ": " + set.name() + " is not a set");
        graph.evaluate();
        for (const objectSets::Member& m : objectSets::members(graph, set.id()))
        {
            if (m.node != mesh)
                continue;
            if (m.kind == ComponentMode::Object)
                return {};
            if (MeshPtr mm = graph.node(mesh)->as<MeshPtr>(MeshNode::kOutMesh))
            {
                components::Ids ids = components::convert(*mm, m.kind, m.ids, ComponentMode::Vertex);
                out.insert(out.end(), ids.begin(), ids.end());
            }
        }
        if (out.empty())
            throw CommandError(std::string(cmd) + ": " + set.name() + " has no components on " + graph.node(mesh)->name());
    }
    else if (mesh != kNoNode && components::mode() != ComponentMode::Object)
        out = components::pointsOf(mesh);
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

skin::PaintMode modeOf(const std::string& m, const char* cmd)
{
    if (m == "add")
        return skin::PaintMode::Add;
    if (m == "replace")
        return skin::PaintMode::Replace;
    if (m == "smooth")
        return skin::PaintMode::Smooth;
    if (m == "scale")
        return skin::PaintMode::Scale;
    throw CommandError(std::string(cmd) + ": mode must be add, replace, smooth or scale");
}

int axisOf(const std::string& a, const char* cmd)
{
    if (a == "x" || a == "y" || a == "z")
        return a[0] - 'x';
    throw CommandError(std::string(cmd) + ": axis must be x, y or z");
}

json exportJson(Graph& graph, const MapTarget& t)
{
    std::vector<float> values = read(graph, t);
    std::vector<double> pos;
    for (const glm::dvec3& p : worldPoints(*t.mesh, t.world))
        pos.insert(pos.end(), {p.x, p.y, p.z});
    std::vector<uint32_t> tris = deform::trianglePoints(*t.mesh);
    return {{"kind", "lilaWeightMap"}, {"version", 1}, {"node", graph.node(t.node)->name()}, {"layer", t.layer}, {"weights", values},
            {"positions", pos}, {"triangles", tris}};
}
} // namespace

// ------------------------------------------------------------------------------------------------ weights

std::vector<float> effective(const DeformerNode& d, const geo::Mesh& in)
{
    LILA_ZONE();

    const size_t n = in.pointCapacity();
    std::vector<float> w(n, 1.0f);
    if (const auto* map = dynamic_cast<const PointWeights*>(d.as<DataPtr>(DeformerNode::kWeights).get()))
        std::copy_n(map->values.begin(), std::min(n, map->values.size()), w.begin());
    if (const auto* f = dynamic_cast<const fields::Evaluator*>(d.as<DataPtr>(DeformerNode::kField).get()))
    {
        std::vector<float> fv = fields::evaluate(*f, in, d.matrix("geomMatrix"));
        const bool replace = d.as<int64_t>(DeformerNode::kFieldMode) == 1;
        for (size_t i = 0; i < n; ++i)
            w[i] = replace ? fv[i] : w[i] * fv[i];
    }
    if (const auto* m = dynamic_cast<const PointIds*>(d.as<DataPtr>(DeformerNode::kMembership).get()))
    {
        std::vector<char> member(n, 0);
        for (uint32_t p : m->points)
            if (p < n)
                member[p] = 1;
        for (size_t i = 0; i < n; ++i)
            if (!member[i])
                w[i] = 0.0f;
    }
    return w;
}

// ------------------------------------------------------------------------------------------------ maps

std::vector<MapTarget> paintable(Graph& graph, NodeId mesh)
{
    std::vector<MapTarget> out;
    std::vector<NodeId> seenFields;
    std::vector<NodeId> list = deformers::list(graph, mesh);
    for (NodeId d : list)
        try
        {
            out.push_back(resolve(graph, d));
        }
        catch (const std::exception&)
        {
        }
    for (NodeId d : list)
    {
        NodeId f = fieldOf(graph, d);
        if (f == kNoNode || std::ranges::find(seenFields, f) != seenFields.end())
            continue;
        seenFields.push_back(f);
        std::vector<fields::Layer> ls = fields::layers(graph, f);
        for (size_t i = 0; i < ls.size(); ++i)
            if (ls[i].kind == fields::Kind::Painted)
                try
                {
                    out.push_back(resolve(graph, f, int(i), mesh));
                }
                catch (const std::exception&)
                {
                }
    }
    return out;
}

MapTarget resolve(Graph& graph, NodeId node, int layer, NodeId mesh)
{
    Node* n = graph.node(node);
    if (!n)
        throw CommandError("no such node");
    MapTarget t;
    t.node = node;
    if (deformers::isDeformer(*n))
    {
        t.meshNode = modifiers::meshOf(graph, node);
        t.mesh = deformerInput(graph, node);
        t.label = n->name() + " (" + n->type().name + ")";
    }
    else if (fields::isField(*n))
    {
        std::vector<fields::Layer> ls = fields::layers(graph, node);
        if (layer < 0)
            for (int i = int(ls.size()) - 1; i >= 0 && layer < 0; --i)
                if (ls[size_t(i)].kind == fields::Kind::Painted)
                    layer = i;
        if (layer < 0 || size_t(layer) >= ls.size() || ls[size_t(layer)].kind != fields::Kind::Painted)
            throw CommandError(n->name() + " has no painted layer" + (layer >= 0 ? std::format(" {}", layer) : std::string()));
        t.layer = layer;
        // Painted on the input of a deformer reading the field, so ids match what the deformer sees.
        for (const PlugRef& dst : graph.destinations({node, fields::kOutField, -1}))
            if (graph.node(dst.node) && deformers::isDeformer(*graph.node(dst.node)))
                if (NodeId m = modifiers::meshOf(graph, dst.node); m != kNoNode && (mesh == kNoNode || m == mesh))
                {
                    t.meshNode = m;
                    t.mesh = deformerInput(graph, dst.node);
                    break;
                }
        if (!t.mesh && mesh != kNoNode && graph.node(mesh)->type().isA("mesh"))
        {
            graph.evaluate();
            t.meshNode = mesh;
            t.mesh = graph.node(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
        }
        t.label = std::format("{}: {} (layer {})", n->name(), ls[size_t(layer)].name, layer);
    }
    else
        throw CommandError(n->name() + " is not a deformer or field");
    if (!t.mesh || t.meshNode == kNoNode)
        throw CommandError(n->name() + ": no mesh to paint on");
    t.world = worldOf(*graph.node(t.meshNode));
    return t;
}

std::vector<float> read(Graph& graph, const MapTarget& t)
{
    const size_t n = t.mesh ? t.mesh->pointCapacity() : 0;
    if (t.layer < 0)
        return deformers::pointWeights(graph, t.node, n);
    std::vector<float> out(n, 0.0f);
    std::vector<fields::Layer> ls = fields::layers(graph, t.node);
    if (size_t(t.layer) < ls.size())
        std::copy_n(ls[size_t(t.layer)].values.begin(), std::min(n, ls[size_t(t.layer)].values.size()), out.begin());
    return out;
}

void write(Graph& graph, const MapTarget& t, std::vector<float> values)
{
    for (float& v : values)
        v = std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f);
    if (t.layer < 0)
    {
        auto w = std::make_shared<PointWeights>();
        w->values = std::move(values);
        graph.setValue({t.node, DeformerNode::kWeights, -1}, DataPtr(std::move(w)));
        return;
    }
    std::vector<fields::Layer> ls = fields::layers(graph, t.node);
    if (size_t(t.layer) >= ls.size())
        throw CommandError("the field layer is gone");
    ls[size_t(t.layer)].values = std::move(values);
    fields::setLayers(graph, t.node, std::move(ls));
}

void paint(float& w, skin::PaintMode mode, bool invert, float value, float a)
{
    switch (mode)
    {
    case skin::PaintMode::Add: w = invert ? w - value * a : w + value * a; break;
    case skin::PaintMode::Replace: w += ((invert ? 1.0f - value : value) - w) * a; break;
    case skin::PaintMode::Scale: w *= 1.0f - a * (1.0f - value); break;
    case skin::PaintMode::Smooth: break;
    }
    w = std::clamp(w, 0.0f, 1.0f);
}

void smooth(const geo::Mesh& m, std::vector<float>& values, const std::vector<std::pair<uint32_t, float>>& pts)
{
    LILA_ZONE();

    std::vector<float> next(pts.size());
    tbb::parallel_for(size_t(0), pts.size(), [&](size_t k) {
        auto [p, a] = pts[k];
        next[k] = p < values.size() ? values[p] : 0.0f;
        if (p >= values.size() || !m.pointValid(p))
            return;
        double sum = 0.0;
        int count = 0;
        m.forEachPointEdge(p, [&](geo::Index e) {
            geo::Index q = m.edgeOtherPoint(e, p);
            if (q < values.size())
            {
                sum += values[q];
                ++count;
            }
        });
        if (count)
            next[k] = std::clamp(values[p] + (float(sum / count) - values[p]) * std::clamp(a, 0.0f, 1.0f), 0.0f, 1.0f);
    });
    for (size_t k = 0; k < pts.size(); ++k)
        if (pts[k].first < values.size())
            values[pts[k].first] = next[k];
}

void mirror(const geo::Mesh& world, std::vector<float>& values, int axis, bool positiveToNegative)
{
    geo::SymmetryPlane plane;
    plane.normal = glm::dvec3(0.0);
    plane.normal[axis] = 1.0;
    geo::SymmetryMap map = geo::SymmetryMap::build(world, plane, std::max(1e-3, glm::length(world.bounds().size()) * 1e-4));
    std::vector<float> src = values;
    for (geo::Index p = 0; p < world.pointCapacity() && p < values.size(); ++p)
    {
        if (!world.pointValid(p))
            continue;
        double side = world.position(p)[axis];
        bool target = positiveToNegative ? side < 0.0 : side > 0.0;
        geo::Index q = map.mirrorPoint(p);
        if (target && q != geo::kInvalid && q < src.size())
            values[p] = src[q];
    }
}

std::vector<float> transfer(const geo::Mesh& srcWorld, std::span<const float> src, const geo::Mesh& dstWorld, bool byId)
{
    LILA_ZONE();

    std::vector<float> out(dstWorld.pointCapacity(), 1.0f);
    if (byId && srcWorld.pointCapacity() == dstWorld.pointCapacity())
    {
        std::copy_n(src.begin(), std::min(src.size(), out.size()), out.begin());
        return out;
    }
    geo::MeshBvh bvh(srcWorld);
    if (bvh.empty())
        return out;
    const auto& tris = bvh.triangles();
    tbb::parallel_for(geo::Index(0), dstWorld.pointCapacity(), [&](geo::Index p) {
        geo::BvhClosest c;
        if (!dstWorld.pointValid(p) || !bvh.closestPoint(dstWorld.position(p), c))
            return;
        double v = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            geo::Index q = srcWorld.cornerPoint(tris.corners[size_t(c.triangle) * 3 + size_t(k)]);
            v += c.bary[k] * (q < src.size() ? src[q] : 1.0);
        }
        out[p] = float(std::clamp(v, 0.0, 1.0));
    });
    return out;
}

// ------------------------------------------------------------------------------------------------ registration

void registerTypes()
{
    data::registerKind("deformerMembership", &PointIds::fromJson);
    correction::registerType();
}

void registerCommands()
{
    using T = ArgType;
    const FlagDef layerFlag{"layer", "", T::Int, nullptr, "Work on this painted layer of the field the deformer reads (or of the field named).", {}, {}, 0.0};
    const FlagDef meshFlag{"mesh", "", T::String, nullptr, "For a field's painted layer: the mesh it is painted on."};

    ops::registerCommand({
        .name = "connectField",
        .doc = "Make deformers read a field: their weights become the painted map times the field (mode multiply) or the field "
               "alone (replace). Give the field first, then the deformers (or meshes: their top deformer).",
        .positionalDoc = "field, deformers...",
        .flags = {{"mode", "", T::String, "multiply", "multiply | replace", {"multiply", "replace"}}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("connectField: give a field, then deformers");
            Node& f = nodeNamed(objs.front(), "connectField");
            if (!fields::isField(f))
                throw CommandError("connectField: " + f.name() + " is not a field");
            objs.erase(objs.begin());
            std::string mode = a.get<std::string>("mode");
            if (mode != "multiply" && mode != "replace")
                throw CommandError("connectField: mode must be multiply or replace");
            json out = json::array();
            for (NodeId d : deformersOf(objs, "connectField"))
            {
                graph.connect({f.id(), fields::kOutField, -1}, {d, DeformerNode::kField, -1}, true);
                graph.setValue({d, DeformerNode::kFieldMode, -1}, int64_t(mode == "replace"));
                out.push_back(graph.node(d)->name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "disconnectField",
        .doc = "Stop deformers reading their field.",
        .positionalDoc = "deformers",
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            for (NodeId d : deformersOf(a.objects(), "disconnectField"))
                if (graph.source({d, DeformerNode::kField, -1}))
                    graph.disconnect({d, DeformerNode::kField, -1});
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "setDeformerMembership",
        .doc = "Restrict deformers to some points (Maya's component tags): points outside the membership never move. Takes "
               "points=, the components of set=, or the selected components. mode replace, add, remove, or clear (every "
               "point again).",
        .positionalDoc = "deformers (or meshes: their top deformer)",
        .flags = {{"mode", "", T::String, "replace", "replace | add | remove | clear", {"replace", "add", "remove", "clear"}},
                  {"points", "", T::List, nullptr, "Point ids."},
                  {"set", "", T::String, nullptr, "Take the points of this set's components on the deformer's mesh."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::string mode = a.get<std::string>("mode");
            if (mode != "replace" && mode != "add" && mode != "remove" && mode != "clear")
                throw CommandError("setDeformerMembership: mode must be replace, add, remove or clear");
            json out = json::array();
            for (NodeId d : deformersOf(a.objects(), "setDeformerMembership"))
            {
                PlugRef plug{d, DeformerNode::kMembership, -1};
                if (mode == "clear")
                {
                    graph.setValue(plug, DataPtr());
                    out.push_back(0);
                    continue;
                }
                std::vector<uint32_t> pts = pointsArg(graph, a, modifiers::meshOf(graph, d), "setDeformerMembership");
                if (pts.empty())
                    throw CommandError("setDeformerMembership: no points: give points=, set= or select components");
                const auto* stored = dynamic_cast<const PointIds*>(std::get<DataPtr>(graph.storedValue(plug)).get());
                if (mode == "add" && !stored)
                {
                    out.push_back(nullptr);
                    continue;
                }
                std::vector<uint32_t> cur = stored ? stored->points : std::vector<uint32_t>{};
                auto next = std::make_shared<PointIds>();
                if (mode == "replace")
                    next->points = pts;
                else if (mode == "add")
                    std::ranges::set_union(cur, pts, std::back_inserter(next->points));
                else
                {
                    if (!stored)
                        if (MeshPtr in = deformerInput(graph, d))
                            for (geo::Index p = 0; p < in->pointCapacity(); ++p)
                                if (in->pointValid(p))
                                    cur.push_back(p);
                    std::ranges::set_difference(cur, pts, std::back_inserter(next->points));
                }
                out.push_back(next->points.size());
                graph.setValue(plug, DataPtr(std::move(next)));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "deformerMembership",
        .doc = "The points a deformer may move; null when it moves every point.",
        .positionalDoc = "deformer",
        .undoable = false,
        .run = [](const Args& a) -> json {
            NodeId d = deformersOf(a.objects(), "deformerMembership").front();
            const auto* m = dynamic_cast<const PointIds*>(std::get<DataPtr>(g().storedValue({d, DeformerNode::kMembership, -1})).get());
            return m ? json(m->points) : json();
        },
    });

    ops::registerCommand({
        .name = "floodDeformerWeights",
        .doc = "Apply a weight to every point of a deformer's weight map (or a field's painted layer), or to points= / the "
               "selected components, as the paint tool's flood does.",
        .positionalDoc = "deformer or field",
        .flags = {{"value", "", T::Float, 1.0, "The weight, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"mode", "", T::String, "replace", "add | replace | smooth | scale", {"add", "replace", "smooth", "scale"}},
                  {"strength", "", T::Float, 1.0, "How much of the change applies, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"points", "", T::List, nullptr, "Point ids; default the selected components, else every point."},
                  layerFlag, meshFlag},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            MapTarget t = targetOf(a, "floodDeformerWeights");
            std::vector<float> values = read(graph, t);
            std::vector<uint32_t> pts = pointsArg(graph, a, t.meshNode, "floodDeformerWeights");
            if (pts.empty())
                for (geo::Index p = 0; p < t.mesh->pointCapacity(); ++p)
                    if (t.mesh->pointValid(p))
                        pts.push_back(p);
            const auto mode = modeOf(a.get<std::string>("mode"), "floodDeformerWeights");
            const float amount = float(std::clamp(a.get<double>("strength"), 0.0, 1.0));
            if (mode == skin::PaintMode::Smooth)
            {
                std::vector<std::pair<uint32_t, float>> sp;
                for (uint32_t p : pts)
                    sp.push_back({p, amount});
                smooth(*t.mesh, values, sp);
            }
            else
                for (uint32_t p : pts)
                    if (p < values.size())
                        paint(values[p], mode, false, float(a.get<double>("value")), amount);
            write(graph, t, std::move(values));
            return pts.size();
        },
    });

    ops::registerCommand({
        .name = "smoothDeformerWeights",
        .doc = "Smooth a deformer's weight map (or a field's painted layer) over the mesh's edges.",
        .positionalDoc = "deformer or field",
        .flags = {{"iterations", "", T::Int, 1, "Smoothing passes.", {}, {}, 0.0, 1000.0}, layerFlag, meshFlag},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            MapTarget t = targetOf(a, "smoothDeformerWeights");
            std::vector<float> values = read(graph, t);
            std::vector<std::pair<uint32_t, float>> pts;
            std::vector<uint32_t> sel = pointsArg(graph, a, t.meshNode, "smoothDeformerWeights");
            if (sel.empty())
                for (geo::Index p = 0; p < t.mesh->pointCapacity(); ++p)
                    sel.push_back(p);
            for (uint32_t p : sel)
                pts.push_back({p, 1.0f});
            for (int i = 0; i < std::clamp(a.get<int>("iterations"), 0, 1000); ++i)
                smooth(*t.mesh, values, pts);
            write(graph, t, std::move(values));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "mirrorDeformerWeights",
        .doc = "Mirror a deformer's weight map (or a field's painted layer) across a world plane through the origin, using the "
               "mesh's symmetry: one side's weights are copied to the matching points on the other.",
        .positionalDoc = "deformer or field",
        .flags = {{"axis", "", T::String, "x", "The plane's normal: x, y or z.", {"x", "y", "z"}},
                  {"direction", "", T::String, "positive", "positive copies the +axis side to the -axis side; negative the reverse.",
                   {"positive", "negative"}},
                  layerFlag, meshFlag},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            MapTarget t = targetOf(a, "mirrorDeformerWeights");
            std::vector<float> values = read(graph, t);
            mirror(inWorld(*t.mesh, t.world), values, axisOf(a.get<std::string>("axis"), "mirrorDeformerWeights"),
                   a.get<std::string>("direction") != "negative");
            write(graph, t, std::move(values));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "copyDeformerWeights",
        .doc = "Copy a weight map from one deformer to others, on the same mesh or another: by point id when the point counts "
               "match, else by closest surface point in world space.",
        .positionalDoc = "source, destinations...",
        .flags = {{"byId", "", T::Bool, true, "Copy by point id when the counts match; off always uses closest points."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("copyDeformerWeights: give a source and destinations");
            MapTarget src = resolve(graph, deformersOf({objs.front()}, "copyDeformerWeights").front());
            objs.erase(objs.begin());
            geo::Mesh srcWorld = inWorld(*src.mesh, src.world);
            std::vector<float> values = read(graph, src);
            json out = json::array();
            for (NodeId d : deformersOf(objs, "copyDeformerWeights"))
            {
                MapTarget dst = resolve(graph, d);
                write(graph, dst, transfer(srcWorld, values, inWorld(*dst.mesh, dst.world), a.get<bool>("byId")));
                out.push_back(graph.node(d)->name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "exportDeformerWeights",
        .doc = "Write a deformer's weight map (or a field's painted layer) to a JSON file, with the mesh's world positions and "
               "triangles so it can be imported onto other topology.",
        .positionalDoc = "deformer or field",
        .flags = {{"file", "f", T::String, nullptr, "The .json file to write."}, layerFlag, meshFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.has("file"))
                throw CommandError("exportDeformerWeights: give file=");
            Graph& graph = g();
            MapTarget t = targetOf(a, "exportDeformerWeights");
            std::ofstream f(a.get<std::string>("file"), std::ios::binary);
            if (!f)
                throw CommandError("exportDeformerWeights: can't write " + a.get<std::string>("file"));
            f << exportJson(graph, t).dump();
            return t.mesh->pointCapacity();
        },
    });

    ops::registerCommand({
        .name = "importDeformerWeights",
        .doc = "Read a weight map written by exportDeformerWeights onto a deformer (or a field's painted layer): by point id "
               "when the point counts match, else by closest surface point to the exported mesh.",
        .positionalDoc = "deformer or field",
        .flags = {{"file", "f", T::String, nullptr, "The .json file to read."},
                  {"byId", "", T::Bool, true, "Match by point id when the counts match; off always uses closest points."},
                  layerFlag, meshFlag},
        .run = [](const Args& a) -> json {
            if (!a.has("file"))
                throw CommandError("importDeformerWeights: give file=");
            Graph& graph = g();
            MapTarget t = targetOf(a, "importDeformerWeights");
            std::ifstream f(a.get<std::string>("file"), std::ios::binary);
            json j = f ? json::parse(f, nullptr, false) : json();
            if (!j.is_object() || j.value("kind", "") != "lilaWeightMap")
                throw CommandError("importDeformerWeights: " + a.get<std::string>("file") + " is not a weight map");
            std::vector<float> values = j.value("weights", std::vector<float>{});
            std::vector<double> pos = j.value("positions", std::vector<double>{});
            std::vector<geo::Index> tris = j.value("triangles", std::vector<geo::Index>{});
            std::vector<glm::dvec3> points(pos.size() / 3);
            for (size_t i = 0; i < points.size(); ++i)
                points[i] = {pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]};
            if (points.size() != values.size())
                throw CommandError("importDeformerWeights: the file's weights and positions disagree");
            std::vector<geo::Index> sizes(tris.size() / 3, 3);
            tris.resize(sizes.size() * 3);
            geo::Mesh src = geo::Mesh::fromPolygons(points, sizes, tris);
            write(graph, t, transfer(src, values, inWorld(*t.mesh, t.world), a.get<bool>("byId")));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "deformerTiming",
        .doc = "How long each deformer in a mesh's stack took on its last evaluation, in stack order: [{deformer, type, ms}], "
               "and the total.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            graph.evaluate();
            json list = json::array();
            double total = 0.0;
            for (const std::string& name : a.objects())
            {
                Node& m = nodeNamed(name, "deformerTiming");
                for (NodeId d : deformers::list(graph, m.id()))
                {
                    const Node* n = graph.node(d);
                    double ms = std::get<double>(n->value(DeformerNode::kEvalTime));
                    total += ms;
                    list.push_back({{"mesh", m.name()}, {"deformer", n->name()}, {"type", n->type().name}, {"ms", ms}});
                }
            }
            return {{"deformers", list}, {"totalMs", total}};
        },
    });

    correction::registerCommands();
}
} // namespace deformWeights
} // namespace lila::scene
