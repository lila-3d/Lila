#include "scene/Morphs.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "scene/Deformers.h"
#include "scene/Drivers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"

#include <glm/gtc/quaternion.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace lila::scene
{
using namespace rigutil;

namespace
{
using morphs::kDeltas;
using morphs::kEditMesh;
using morphs::kEditWeight;
using morphs::kInBetweens;
using morphs::kLink;
using morphs::kMorphName;
using morphs::kPointWeights;
using morphs::kPrevious;
using morphs::kTargetMesh;
using morphs::kWeight;

constexpr double kShapeEps = 1e-6;

template <class V> void readArray(const json& j, const char* key, std::vector<V>& out)
{
    out.clear();
    if (!j.contains(key) || !j[key].is_array())
        return;
    out.reserve(j[key].size());
    for (const json& x : j[key])
        out.push_back(V(x.get<double>()));
}

std::vector<double> flatten(const std::vector<glm::dvec3>& v)
{
    std::vector<double> out;
    out.reserve(v.size() * 3);
    for (const glm::dvec3& p : v)
        out.insert(out.end(), {p.x, p.y, p.z});
    return out;
}

std::vector<glm::dvec3> unflatten(const std::vector<double>& f)
{
    std::vector<glm::dvec3> out(f.size() / 3);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = {f[i * 3], f[i * 3 + 1], f[i * 3 + 2]};
    return out;
}

template <class F> void parallelPoints(size_t n, F&& fn)
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 512), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i)
            fn(i);
    });
}

/** What one morphTarget passes down its chain. */
struct MorphLink final : Data
{
    DataPtr prev;
    double weight = 0.0;
    DataPtr deltas;
    MeshPtr live;
    DataPtr inBetweens;
    MeshPtr edit;
    double editWeight = 1.0;
    DataPtr pointWeights;
    const char* kind() const override { return "morphLink"; }
    json toJson() const override { return {{"kind", kind()}}; }
};

/** One shape of a target as evaluation sees it: sparse offsets, or a whole mesh to diff against the input. */
struct ShapeRef
{
    double weight = 1.0;
    const MorphDeltas* sparse = nullptr;
    const geo::Mesh* mesh = nullptr;
};

/** The full shape first, then the in-betweens by weight; an edit mesh stands in for the shape it edits. */
std::vector<ShapeRef> shapesOf(const DataPtr& deltas, const MeshPtr& live, const DataPtr& inBetweens, const MeshPtr& edit, double editWeight)
{
    std::vector<ShapeRef> out;
    out.push_back({1.0, dynamic_cast<const MorphDeltas*>(deltas.get()), live.get()});
    if (const auto* ib = dynamic_cast<const MorphInBetweens*>(inBetweens.get()))
        for (const MorphInBetweens::Shape& s : ib->shapes)
            out.push_back({s.weight, nullptr, nullptr});
    if (edit)
        for (ShapeRef& s : out)
            if (std::abs(s.weight - editWeight) < kShapeEps)
                s.mesh = edit.get();
    return out;
}

std::vector<double> knotsOf(const std::vector<ShapeRef>& shapes)
{
    std::vector<double> k;
    for (const ShapeRef& s : shapes)
        k.push_back(s.weight);
    return k;
}

/** shapeCoefficients for shapes in any order. */
std::vector<double> coefficients(const std::vector<double>& weights, double w)
{
    std::vector<size_t> order(weights.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return weights[a] < weights[b]; });
    std::vector<double> sorted;
    for (size_t i : order)
        sorted.push_back(weights[i]);
    std::vector<double> c = morphs::shapeCoefficients(sorted, w);
    std::vector<double> out(weights.size(), 0.0);
    for (size_t k = 0; k < order.size(); ++k)
        out[order[k]] = c[k];
    return out;
}

class MorphNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        for (DataPtr d = as<DataPtr>(attr("targets")); d;)
        {
            const auto* link = dynamic_cast<const MorphLink*>(d.get());
            if (!link)
                break;
            d = link->prev;
            if (link->weight == 0.0)
                continue;
            std::vector<ShapeRef> shapes = shapesOf(link->deltas, link->live, link->inBetweens, link->edit, link->editWeight);
            const auto* ib = dynamic_cast<const MorphInBetweens*>(link->inBetweens.get());
            std::vector<double> c = coefficients(knotsOf(shapes), link->weight);
            const auto* pw = dynamic_cast<const PointWeights*>(link->pointWeights.get());
            auto mask = [pw](size_t p) { return pw && p < pw->values.size() ? double(pw->values[p]) : 1.0; };
            for (size_t s = 0; s < shapes.size(); ++s)
            {
                const double w = c[s];
                if (w == 0.0)
                    continue;
                if (const geo::Mesh* m = shapes[s].mesh)
                {
                    if (m->pointCapacity() != in.pointCapacity())
                        throw std::runtime_error("a live or edited target's point count differs from the mesh");
                    parallelPoints(points.size(),
                                   [&](size_t p) { points[p] += (m->position(geo::Index(p)) - in.position(geo::Index(p))) * (w * mask(p)); });
                    continue;
                }
                const std::vector<uint32_t>* idx = nullptr;
                const std::vector<glm::dvec3>* del = nullptr;
                if (s == 0 && shapes[s].sparse)
                {
                    idx = &shapes[s].sparse->indices;
                    del = &shapes[s].sparse->deltas;
                }
                else if (s > 0 && ib)
                {
                    idx = &ib->shapes[s - 1].indices;
                    del = &ib->shapes[s - 1].deltas;
                }
                if (!idx)
                    continue;
                const size_t n = std::min(idx->size(), del->size());
                parallelPoints(n, [&](size_t k) {
                    uint32_t p = (*idx)[k];
                    if (p < points.size())
                        points[p] += (*del)[k] * (w * mask(p));
                });
            }
        }
    }
};

class MorphTargetNode : public Node
{
public:
    void compute(const EvalContext&) override
    {
        auto link = std::make_shared<MorphLink>();
        link->prev = as<DataPtr>(kPrevious);
        link->weight = as<double>(kWeight);
        link->deltas = as<DataPtr>(kDeltas);
        link->live = as<MeshPtr>(kTargetMesh);
        link->inBetweens = as<DataPtr>(kInBetweens);
        link->edit = as<MeshPtr>(kEditMesh);
        link->editWeight = as<double>(kEditWeight);
        link->pointWeights = as<DataPtr>(kPointWeights);
        setOutput(kLink, DataPtr(std::move(link)));
    }
};

AttrDef keyable(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc)};
}
AttrDef stored(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable, .doc = std::move(doc)};
}
AttrDef hiddenStored(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrHidden,
            .doc = std::move(doc)};
}
AttrDef connected(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrHidden, .doc = std::move(doc)};
}

// ------------------------------------------------------------------------------------------------ helpers

Graph& g() { return Graph::current(); }

Node& meshNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!n.type().isA("mesh") || n.type().isA("lattice"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return n;
}

/** A morph, its mesh, or one of its targets, to the morph. */
NodeId resolveMorph(Graph& graph, const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (n.type().isA("morph"))
        return n.id();
    if (morphs::isTarget(n))
        if (NodeId m = morphs::morphOf(graph, n.id()); m != kNoNode)
            return m;
    if (n.type().isA("mesh"))
        if (NodeId m = morphs::find(graph, n.id()); m != kNoNode)
            return m;
    throw CommandError(std::string(cmd) + ": " + name + " has no morph");
}

Node& targetNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!morphs::isTarget(n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a morphTarget");
    return n;
}

std::shared_ptr<MorphDeltas> deltasBetween(const geo::Mesh& base, const geo::Mesh& target)
{
    auto d = std::make_shared<MorphDeltas>();
    for (geo::Index p = 0; p < base.pointCapacity() && p < target.pointCapacity(); ++p)
    {
        if (!base.pointValid(p))
            continue;
        glm::dvec3 delta = target.position(p) - base.position(p);
        if (glm::length(delta) > 1e-7)
        {
            d->indices.push_back(uint32_t(p));
            d->deltas.push_back(delta);
        }
    }
    return d;
}

std::shared_ptr<MorphDeltas> sparse(const std::vector<glm::dvec3>& dense)
{
    auto d = std::make_shared<MorphDeltas>();
    for (size_t p = 0; p < dense.size(); ++p)
        if (glm::length(dense[p]) > 1e-7)
        {
            d->indices.push_back(uint32_t(p));
            d->deltas.push_back(dense[p]);
        }
    return d;
}

std::vector<glm::dvec3> dense(const std::vector<uint32_t>& indices, const std::vector<glm::dvec3>& deltas, size_t n)
{
    std::vector<glm::dvec3> out(n, glm::dvec3(0.0));
    for (size_t k = 0; k < std::min(indices.size(), deltas.size()); ++k)
        if (indices[k] < n)
            out[indices[k]] = deltas[k];
    return out;
}

MeshPtr morphInput(Graph& graph, NodeId morph)
{
    graph.evaluate();
    return std::get<MeshPtr>(graph.value({morph, DeformerNode::kInMesh, -1}));
}

/** A target's shape at shapeWeight (1 the full shape, else an in-between) as dense offsets from the morph's input. */
std::vector<glm::dvec3> shapeOffsets(Graph& graph, NodeId target, double shapeWeight, const geo::Mesh& base)
{
    Node* t = graph.node(target);
    const size_t n = base.pointCapacity();
    auto fromMesh = [&](const MeshPtr& m) {
        std::vector<glm::dvec3> out(n, glm::dvec3(0.0));
        if (m && m->pointCapacity() == n)
            for (geo::Index p = 0; p < n; ++p)
                out[p] = m->position(p) - base.position(p);
        return out;
    };
    MeshPtr edit = t->as<MeshPtr>(kEditMesh);
    if (edit && std::abs(t->as<double>(kEditWeight) - shapeWeight) < kShapeEps)
        return fromMesh(edit);
    if (std::abs(shapeWeight - 1.0) < kShapeEps)
    {
        if (MeshPtr live = t->as<MeshPtr>(kTargetMesh))
            return fromMesh(live);
        if (const auto* d = dynamic_cast<const MorphDeltas*>(t->as<DataPtr>(kDeltas).get()))
            return dense(d->indices, d->deltas, n);
        return std::vector<glm::dvec3>(n, glm::dvec3(0.0));
    }
    if (const auto* ib = dynamic_cast<const MorphInBetweens*>(t->as<DataPtr>(kInBetweens).get()))
        for (const MorphInBetweens::Shape& s : ib->shapes)
            if (std::abs(s.weight - shapeWeight) < kShapeEps)
                return dense(s.indices, s.deltas, n);
    throw CommandError(std::format("{} has no in-between at {}", t->name(), shapeWeight));
}

/** Shape weights of a target: 1, then its in-betweens. */
std::vector<double> shapeWeights(const Node& t)
{
    std::vector<double> out{1.0};
    if (const auto* ib = dynamic_cast<const MorphInBetweens*>(t.as<DataPtr>(kInBetweens).get()))
        for (const MorphInBetweens::Shape& s : ib->shapes)
            out.push_back(s.weight);
    return out;
}

/** The shape at shapeWeight interpolated from the other shapes, the way evaluation reaches that weight now. */
std::vector<glm::dvec3> interpolatedOffsets(Graph& graph, NodeId target, double w, const geo::Mesh& base)
{
    std::vector<double> ws = shapeWeights(*graph.node(target));
    std::vector<double> c = coefficients(ws, w);
    std::vector<glm::dvec3> out(base.pointCapacity(), glm::dvec3(0.0));
    for (size_t s = 0; s < ws.size(); ++s)
        if (c[s] != 0.0)
        {
            std::vector<glm::dvec3> o = shapeOffsets(graph, target, ws[s], base);
            for (size_t p = 0; p < out.size(); ++p)
                out[p] += o[p] * c[s];
        }
    return out;
}

void setShape(Graph& graph, NodeId target, double shapeWeight, std::shared_ptr<MorphDeltas> d)
{
    if (std::abs(shapeWeight - 1.0) < kShapeEps)
    {
        if (graph.source({target, kTargetMesh, -1}))
            graph.disconnect({target, kTargetMesh, -1});
        graph.setValue({target, kDeltas, -1}, DataPtr(std::move(d)));
    }
    else
        morphs::setInBetween(graph, target, shapeWeight, std::move(d));
}

/** Hooks a target node onto the end of the morph's chain. */
void appendTarget(Graph& graph, NodeId morph, NodeId t)
{
    PlugRef targets{morph, graph.node(morph)->type().attrIndex("targets"), -1};
    if (auto last = graph.source(targets))
    {
        graph.disconnect(targets);
        graph.connect(*last, {t, kPrevious, -1});
    }
    graph.connect({t, kLink, -1}, targets);
}

/** morphName made unique among the morph's targets. */
std::string uniqueMorphName(Graph& graph, NodeId morph, const std::string& wanted, NodeId except = kNoNode)
{
    auto taken = [&](const std::string& s) {
        for (NodeId t : morphs::targetNodes(graph, morph))
            if (t != except && graph.node(t)->as<std::string>(kMorphName) == s)
                return true;
        return false;
    };
    if (!taken(wanted))
        return wanted;
    for (int i = 2;; ++i)
        if (std::string s = wanted + std::to_string(i); !taken(s))
            return s;
}

NodeId newTarget(Graph& graph, NodeId morph, const std::string& morphName)
{
    std::string name = uniqueMorphName(graph, morph, morphName);
    Node* t = guarded([&] { return graph.createNode(*types::find("morphTarget"), name + "Target", kNoNode, true); });
    graph.setValue({t->id(), kMorphName, -1}, name);
    appendTarget(graph, morph, t->id());
    return t->id();
}

NodeId addTargetFromMesh(Graph& graph, NodeId morph, NodeId targetMesh, const std::string& morphName, bool live)
{
    MeshPtr base = morphInput(graph, morph);
    MeshPtr target = graph.node(targetMesh)->as<MeshPtr>(MeshNode::kOutMesh);
    if (!base || !target)
        throw CommandError("morph: " + graph.node(targetMesh)->name() + " or the base has no geometry");
    if (base->pointCapacity() != target->pointCapacity())
        throw CommandError(std::format("morph: {} has {} points, the base {}", graph.node(targetMesh)->name(), target->pointCapacity(),
                                       base->pointCapacity()));
    NodeId t = newTarget(graph, morph, morphName);
    if (live)
        graph.connect({targetMesh, MeshNode::kOutMesh, -1}, {t, kTargetMesh, -1});
    else
        graph.setValue({t, kDeltas, -1}, DataPtr(deltasBetween(*base, *target)));
    return t;
}

void removeTarget(Graph& graph, NodeId t)
{
    auto prev = graph.source({t, kPrevious, -1});
    std::vector<PlugRef> dsts = graph.destinations({t, kLink, -1});
    for (const PlugRef& d : dsts)
        graph.disconnect(d);
    if (prev)
        for (const PlugRef& d : dsts)
            graph.connect(*prev, d);
    graph.deleteNode(t);
}

json targetInfo(Graph& graph, NodeId t)
{
    Node* n = graph.node(t);
    json ib = json::array();
    for (double w : shapeWeights(*n))
        if (std::abs(w - 1.0) >= kShapeEps)
            ib.push_back(w);
    auto src = graph.source({t, kWeight, -1});
    MeshPtr edit = n->as<MeshPtr>(kEditMesh);
    auto editSrc = graph.source({t, kEditMesh, -1});
    return {{"node", n->name()},
            {"name", n->as<std::string>(kMorphName)},
            {"weight", std::get<double>(graph.value({t, kWeight, -1}))},
            {"sliderMin", n->as<double>(morphs::kSliderMin)},
            {"sliderMax", n->as<double>(morphs::kSliderMax)},
            {"live", graph.source({t, kTargetMesh, -1}).has_value()},
            {"inBetweens", ib},
            {"driver", src ? json(graph.plugName(*src)) : json(nullptr)},
            {"editing", editSrc ? json(graph.node(editSrc->node)->name()) : json(nullptr)}};
}

/** A posed mesh's evaluated points, object space. */
std::vector<glm::dvec3> evaluatedPoints(Graph& graph, NodeId mesh)
{
    graph.evaluate();
    MeshPtr m = graph.node(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
    if (!m)
        throw CommandError(graph.node(mesh)->name() + " has no geometry");
    return {m->positions().begin(), m->positions().end()};
}

/** Copies a mesh node's evaluated shape into a new mesh at the same world placement. */
Node* duplicateShape(Graph& graph, NodeId mesh, const MeshPtr& shape, const std::string& name)
{
    auto copy = std::make_shared<geo::Mesh>(*shape);
    copy->pointAttrs().remove(modifiers::kBypassLayer);
    Node* m = guarded([&] { return graph.createNode(*types::find("mesh"), name, kNoNode, true); });
    graph.setValue({m->id(), MeshNode::kInMesh, -1}, MeshPtr(std::move(copy)));
    Transform::setLocalMatrix(graph, m->id(), worldOf(*graph.node(mesh)));
    return m;
}

void setVisible(Graph& graph, NodeId id, bool on)
{
    PlugRef v{id, Transform::kVisibility, -1};
    if (!graph.source(v) && std::get<bool>(graph.storedValue(v)) != on)
        graph.setValue(v, on);
}

std::string mirroredName(const std::string& name)
{
    static const std::pair<const char*, const char*> kPairs[] = {{"Left", "Right"}, {"left", "right"}, {"L", "R"}, {"l", "r"}};
    for (auto [a, b] : kPairs)
    {
        if (std::string m = rig::mirrorName(name, a, b); m != name)
            return m;
        if (std::string m = rig::mirrorName(name, b, a); m != name)
            return m;
    }
    return name + "_mirror";
}

int axisIndex(const std::string& a, const char* cmd)
{
    if (a == "x")
        return 0;
    if (a == "y")
        return 1;
    if (a == "z")
        return 2;
    throw CommandError(std::string(cmd) + ": axis must be x, y or z");
}

/** The offsets reflected across the symmetry plane: each point takes its partner's offset, mirrored. */
std::vector<glm::dvec3> reflectOffsets(const std::vector<glm::dvec3>& in, const geo::SymmetryMap& map, int axis, int keepSide)
{
    std::vector<glm::dvec3> out = in;
    for (size_t p = 0; p < in.size(); ++p)
    {
        // keepSide 0 flips every point; otherwise only points off that side take their partner's offset.
        if (keepSide != 0 && map.side(geo::Index(p)) == keepSide)
            continue;
        if (map.onPlane(geo::Index(p)))
        {
            out[p] = in[p];
            out[p][axis] = keepSide != 0 ? 0.0 : -in[p][axis];
            continue;
        }
        geo::Index m = map.mirrorPoint(geo::Index(p));
        if (m == geo::kInvalid || m >= in.size())
            continue;
        glm::dvec3 d = in[m];
        d[axis] = -d[axis];
        out[p] = d;
    }
    return out;
}

/** Posed influences of the mesh's skin, most rotated first, for picking a corrective's driver. */
std::vector<NodeId> posedInfluences(Graph& graph, NodeId mesh)
{
    std::vector<std::pair<double, NodeId>> posed;
    NodeId s = skin::find(graph, mesh);
    if (s == kNoNode)
        return {};
    for (const skin::Influence& inf : skin::influences(graph, s))
    {
        if (inf.joint == kNoNode)
            continue;
        Node* j = graph.node(inf.joint);
        glm::dvec3 r = j->as<glm::dvec3>(Transform::kRotate);
        glm::dmat3 rot(eulerToMatrix(r, RotateOrder(std::clamp<int64_t>(j->as<int64_t>(Transform::kRotateOrder), 0, 5))));
        double angle = glm::degrees(2.0 * std::acos(std::clamp(std::abs(glm::quat_cast(rot).w), 0.0, 1.0)));
        if (angle > 5.0)
            posed.push_back({angle, inf.joint});
    }
    std::sort(posed.begin(), posed.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<NodeId> out;
    for (auto& p : posed)
        out.push_back(p.second);
    return out;
}

/** "Body_corrective" or "Body_corrective2" back to "Body". */
std::string baseOfSculpt(const std::string& sculpt)
{
    std::string s = sculpt;
    while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back())))
        s.pop_back();
    const std::string suffix = "_corrective";
    if (s.size() > suffix.size() && s.ends_with(suffix))
        return s.substr(0, s.size() - suffix.size());
    return {};
}
} // namespace

// ------------------------------------------------------------------------------------------------ data kinds

json MorphDeltas::toJson() const { return {{"kind", kind()}, {"indices", indices}, {"deltas", flatten(deltas)}}; }

DataPtr MorphDeltas::fromJson(const json& j)
{
    auto d = std::make_shared<MorphDeltas>();
    readArray(j, "indices", d->indices);
    std::vector<double> f;
    readArray(j, "deltas", f);
    d->deltas = unflatten(f);
    d->indices.resize(std::min(d->indices.size(), d->deltas.size()));
    d->deltas.resize(d->indices.size());
    return d;
}

size_t MorphInBetweens::bytes() const
{
    size_t n = 0;
    for (const Shape& s : shapes)
        n += s.indices.size() * sizeof(uint32_t) + s.deltas.size() * sizeof(glm::dvec3);
    return n;
}

json MorphInBetweens::toJson() const
{
    json list = json::array();
    for (const Shape& s : shapes)
        list.push_back({{"weight", s.weight}, {"indices", s.indices}, {"deltas", flatten(s.deltas)}});
    return {{"kind", kind()}, {"shapes", list}};
}

DataPtr MorphInBetweens::fromJson(const json& j)
{
    auto d = std::make_shared<MorphInBetweens>();
    if (j.contains("shapes") && j["shapes"].is_array())
        for (const json& s : j["shapes"])
        {
            Shape shape;
            shape.weight = s.value("weight", 0.5);
            readArray(s, "indices", shape.indices);
            std::vector<double> f;
            readArray(s, "deltas", f);
            shape.deltas = unflatten(f);
            shape.indices.resize(std::min(shape.indices.size(), shape.deltas.size()));
            shape.deltas.resize(shape.indices.size());
            d->shapes.push_back(std::move(shape));
        }
    std::sort(d->shapes.begin(), d->shapes.end(), [](const Shape& a, const Shape& b) { return a.weight < b.weight; });
    return d;
}

// ------------------------------------------------------------------------------------------------ api

namespace morphs
{
bool isTarget(const Node& n) { return n.type().isA("morphTarget"); }

NodeId find(Graph& graph, NodeId mesh)
{
    for (NodeId id : modifiers::stack(graph, mesh))
        if (graph.node(id)->type().isA("morph"))
            return id;
    return kNoNode;
}

NodeId morphOf(Graph& graph, NodeId target)
{
    NodeId cur = target;
    for (int guard = 0; guard < 100000; ++guard)
    {
        std::vector<PlugRef> dsts = graph.destinations({cur, kLink, -1});
        if (dsts.empty())
            return kNoNode;
        cur = dsts[0].node;
        Node* n = graph.node(cur);
        if (n->type().isA("morph"))
            return cur;
        if (!isTarget(*n))
            return kNoNode;
    }
    return kNoNode;
}

std::vector<NodeId> targetNodes(Graph& graph, NodeId morph)
{
    std::vector<NodeId> out;
    Node* m = graph.node(morph);
    if (!m)
        return out;
    PlugRef plug{morph, m->type().attrIndex("targets"), -1};
    while (auto src = graph.source(plug))
    {
        Node* t = graph.node(src->node);
        if (!isTarget(*t) || out.size() > 100000)
            break;
        out.push_back(t->id());
        plug = {t->id(), kPrevious, -1};
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<double> shapeCoefficients(const std::vector<double>& knots, double w)
{
    std::vector<double> c(knots.size(), 0.0);
    if (knots.empty() || w == 0.0)
        return c;
    // Knot list with the empty shape at 0 (index -1).
    std::vector<std::pair<double, int>> k{{0.0, -1}};
    for (size_t i = 0; i < knots.size(); ++i)
        k.push_back({knots[i], int(i)});
    std::stable_sort(k.begin(), k.end(), [](auto& a, auto& b) { return a.first < b.first; });
    size_t seg = 0;
    while (seg + 2 < k.size() && w > k[seg + 1].first)
        ++seg;
    const auto& [wa, ia] = k[seg];
    const auto& [wb, ib] = k[seg + 1];
    const double span = wb - wa;
    const double t = std::abs(span) < 1e-12 ? 1.0 : (w - wa) / span;
    if (ia >= 0)
        c[size_t(ia)] += 1.0 - t;
    if (ib >= 0)
        c[size_t(ib)] += t;
    return c;
}

std::vector<Target> targets(Graph& graph, NodeId morph)
{
    std::vector<Target> out;
    Node* m = graph.node(morph);
    if (!m)
        return out;
    MeshPtr base = morphInput(graph, morph);
    const double envelope = m->as<double>(DeformerNode::kEnvelope);
    for (NodeId t : targetNodes(graph, morph))
    {
        Node* n = graph.node(t);
        std::string name = n->as<std::string>(kMorphName);
        if (name.empty())
            name = n->name();
        std::vector<double> ws = shapeWeights(*n);
        std::vector<double> c = coefficients(ws, n->as<double>(kWeight));
        const auto* pw = dynamic_cast<const PointWeights*>(n->as<DataPtr>(kPointWeights).get());
        for (size_t s = 0; s < ws.size(); ++s)
        {
            Target e;
            e.node = t;
            e.name = s == 0 ? name : std::format("{}_{}", name, int(std::lround(ws[s] * 100.0)));
            e.weight = c[s] * envelope;
            if (base)
            {
                std::vector<glm::dvec3> offsets = shapeOffsets(graph, t, ws[s], *base);
                if (pw)
                    for (size_t p = 0; p < offsets.size() && p < pw->values.size(); ++p)
                        offsets[p] *= double(pw->values[p]);
                std::shared_ptr<MorphDeltas> d = sparse(offsets);
                e.indices = std::move(d->indices);
                e.deltas = std::move(d->deltas);
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::vector<double> weights(Graph& graph, NodeId morph)
{
    graph.evaluate();
    std::vector<double> out;
    Node* m = graph.node(morph);
    if (!m)
        return out;
    const double envelope = m->as<double>(DeformerNode::kEnvelope);
    for (NodeId t : targetNodes(graph, morph))
    {
        Node* n = graph.node(t);
        for (double c : coefficients(shapeWeights(*n), n->as<double>(kWeight)))
            out.push_back(c * envelope);
    }
    return out;
}

NodeId create(Graph& graph, NodeId mesh, const std::string& name)
{
    return deformers::addDeformer(graph, "morph", {mesh}, name, true).front();
}

NodeId addTarget(Graph& graph, NodeId morph, const std::string& morphName, std::shared_ptr<const MorphDeltas> deltas, double weight)
{
    NodeId t = newTarget(graph, morph, morphName);
    graph.setValue({t, kDeltas, -1}, DataPtr(std::move(deltas)));
    graph.setValue({t, kWeight, -1}, weight);
    return t;
}

void setInBetween(Graph& graph, NodeId target, double weight, std::shared_ptr<const MorphDeltas> deltas)
{
    if (std::abs(weight) < kShapeEps || std::abs(weight - 1.0) < kShapeEps)
        throw CommandError("an in-between's weight can't be 0 or 1");
    Node* t = graph.node(target);
    auto next = std::make_shared<MorphInBetweens>();
    if (const auto* ib = dynamic_cast<const MorphInBetweens*>(t->as<DataPtr>(kInBetweens).get()))
        for (const MorphInBetweens::Shape& s : ib->shapes)
            if (std::abs(s.weight - weight) >= kShapeEps)
                next->shapes.push_back(s);
    MorphInBetweens::Shape s;
    s.weight = weight;
    if (deltas)
    {
        s.indices = deltas->indices;
        s.deltas = deltas->deltas;
    }
    next->shapes.push_back(std::move(s));
    std::sort(next->shapes.begin(), next->shapes.end(), [](auto& a, auto& b) { return a.weight < b.weight; });
    graph.setValue({target, kInBetweens, -1}, DataPtr(std::move(next)));
}

void setPointWeights(Graph& graph, NodeId target, DataPtr weights) { graph.setValue({target, kPointWeights, -1}, std::move(weights)); }

std::shared_ptr<MorphDeltas> invertSculpt(Graph& graph, NodeId target, const std::vector<glm::dvec3>& sculpt, double* residual)
{
    LILA_ZONE();

    const NodeId morph = morphOf(graph, target);
    const NodeId mesh = morph != kNoNode ? modifiers::meshOf(graph, morph) : kNoNode;
    if (mesh == kNoNode)
        throw CommandError(graph.node(target)->name() + " is not on a mesh's morph");
    MeshPtr base = morphInput(graph, morph);
    if (!base)
        throw CommandError(graph.node(mesh)->name() + " has no geometry");
    const size_t n = base->pointCapacity();
    if (sculpt.size() != n)
        throw CommandError(std::format("the sculpt has {} points, {} has {}", sculpt.size(), graph.node(mesh)->name(), n));

    UndoStack& undo = UndoStack::get();
    const bool wasRecording = undo.enabled();
    const PlugRef deltasPlug{target, kDeltas, -1};
    const DataPtr saved = std::get<DataPtr>(graph.storedValue(deltasPlug));
    const auto liveSrc = graph.source({target, kTargetMesh, -1});
    struct Restore
    {
        Graph& graph;
        PlugRef plug;
        DataPtr saved;
        UndoStack& undo;
        bool was;
        std::optional<PlugRef> live;
        NodeId target;
        ~Restore()
        {
            try
            {
                graph.setValue(plug, saved);
                if (live)
                    graph.connect(*live, {target, kTargetMesh, -1});
                graph.evaluate();
            }
            catch (const std::exception& e)
            {
                log::error("invertSculpt: {}", e.what());
            }
            undo.setEnabled(was);
        }
    };
    undo.setEnabled(false);
    Restore restore{graph, deltasPlug, saved, undo, wasRecording, liveSrc, target};
    if (liveSrc)
        graph.disconnect({target, kTargetMesh, -1});

    std::vector<uint32_t> all(n);
    for (size_t p = 0; p < n; ++p)
        all[p] = uint32_t(p);
    auto evalWith = [&](const std::vector<glm::dvec3>& f) {
        auto d = std::make_shared<MorphDeltas>();
        d->indices = all;
        d->deltas = f;
        graph.setValue(deltasPlug, DataPtr(std::move(d)));
        std::vector<glm::dvec3> out = evaluatedPoints(graph, mesh);
        if (out.size() != n)
            throw CommandError(graph.node(mesh)->name() + ": a modifier above the morph changes the point count; correctives need point-wise deformers");
        return out;
    };

    std::vector<glm::dvec3> f(n, glm::dvec3(0.0));
    std::vector<glm::dvec3> p0 = evalWith(f);
    // The skin is affine in each rest point, so a unit step gives the exact per-point Jacobian.
    std::vector<glm::dmat3> jac(n, glm::dmat3(0.0));
    for (int k = 0; k < 3; ++k)
    {
        std::vector<glm::dvec3> step(n, glm::dvec3(0.0));
        for (glm::dvec3& s : step)
            s[k] = 1.0;
        std::vector<glm::dvec3> pk = evalWith(step);
        for (size_t p = 0; p < n; ++p)
            jac[p][k] = pk[p] - p0[p];
    }
    std::vector<glm::dmat3> inv(n);
    bool any = false;
    for (size_t p = 0; p < n; ++p)
    {
        double det = glm::determinant(jac[p]);
        any = any || std::abs(det) > 1e-12;
        inv[p] = std::abs(det) > 1e-12 ? glm::inverse(jac[p]) : glm::dmat3(1.0);
    }
    if (!any)
        throw CommandError(graph.node(target)->name() + ": its weight is 0 at this pose, so no offset can reach the sculpt");

    std::vector<glm::dvec3> cur = p0;
    double worst = 0.0;
    for (int iter = 0; iter < 12; ++iter)
    {
        worst = 0.0;
        for (size_t p = 0; p < n; ++p)
        {
            if (!base->pointValid(geo::Index(p)))
                continue;
            glm::dvec3 r = sculpt[p] - cur[p];
            worst = std::max(worst, glm::length(r));
            f[p] += inv[p] * r;
        }
        if (worst < 1e-9)
            break;
        cur = evalWith(f);
    }
    worst = 0.0;
    for (size_t p = 0; p < n; ++p)
        if (base->pointValid(geo::Index(p)))
            worst = std::max(worst, glm::length(sculpt[p] - cur[p]));
    if (residual)
        *residual = worst;
    return sparse(f);
}

void registerTypes()
{
    if (types::find("morph"))
        return;
    data::registerKind("morphDeltas", &MorphDeltas::fromJson);
    data::registerKind("morphInBetweens", &MorphInBetweens::fromJson);

    deformers::registerDeformer({"morph", "mood-smile",
                                 "Morph targets: blends the mesh toward target shapes by their weights, with in-betweens and pose driven "
                                 "correctives. Sits under any skinCluster. Exported to Unreal as morph targets with their curves.",
                                 {connected("targets", PlugType::Data, DataPtr(), "The last morphTarget in the chain.")},
                                 [] { return std::make_unique<MorphNode>(); }});

    NodeType target;
    target.name = "morphTarget";
    target.icon = "mood-smile";
    target.doc = "One morph target: its weight, its full shape as offsets or a live target mesh, and any in-between shapes.";
    target.ownAttrs = {
        keyable("weight", PlugType::Float, 0.0, "How much of the target applies, usually 0 to 1."),
        stored("morphName", PlugType::String, std::string(), "The morph target's name in exports."),
        hiddenStored("deltas", PlugType::Data, DataPtr(), "Full shape offsets from the base, object space."),
        connected("targetMesh", PlugType::Mesh, MeshPtr(), "A live target's outMesh; overrides the stored offsets."),
        connected("previous", PlugType::Data, DataPtr(), "The target before this one."),
        {.name = "link", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "This target plus everything before it."},
        hiddenStored("inBetweens", PlugType::Data, DataPtr(), "In-between shapes, each with the weight it is reached at."),
        connected("editMesh", PlugType::Mesh, MeshPtr(), "The mesh being sculpted in edit mode; stands in for the shape it edits."),
        hiddenStored("editWeight", PlugType::Float, 1.0, "Which shape the edit mesh edits: 1 the full shape, else an in-between's weight."),
        hiddenStored("pointWeights", PlugType::Data, DataPtr(), "Per-point weights scaling every shape of this target; points past the end weigh 1."),
        {.name = "sliderMin", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable,
         .doc = "The weight at the left end of the Morph Targets panel's slider; negative weights push the shape the other way.",
         .category = "Slider"},
        {.name = "sliderMax", .type = PlugType::Float, .defaultValue = 1.0, .flags = AttrInput | AttrStorable,
         .doc = "The weight at the right end of the Morph Targets panel's slider; past 1 overshoots the shape.", .category = "Slider"},
    };
    target.create = [] { return std::make_unique<MorphTargetNode>(); };
    types::registerType(std::move(target));

    // Older scene files name the morph types and their offsets this way.
    types::addFileAlias("blendShape", "morph");
    types::addFileAlias("blendTarget", "morphTarget");
    data::registerKind("blendDeltas", &MorphDeltas::fromJson);
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "morph",
        .doc = "Add a morph deformer (morph targets) to the base mesh, the last argument, with a target per other mesh. Targets need "
               "the base's point count. It sits under any skinCluster, so morphs apply before skinning, as in Unreal. Returns the "
               "morph.",
        .positionalDoc = "targets..., base",
        .flags = {{"live", "", T::Bool, false, "Keep targets connected so editing a target mesh updates the morph."},
                  {"name", "", T::String, nullptr, "Name for the morph."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("morph: give targets and the base mesh last");
            NodeId base = meshNamed(objs.back(), "morph").id();
            objs.pop_back();
            if (find(graph, base) != kNoNode)
                throw CommandError("morph: " + graph.node(base)->name() + " already has a morph; use addMorphTarget");
            NodeId m = create(graph, base, a.has("name") ? a.get<std::string>("name") : "Morph");
            for (const std::string& name : objs)
                addTargetFromMesh(graph, m, meshNamed(name, "morph").id(), name, a.get<bool>("live"));
            return graph.node(m)->name();
        },
    });

    ops::registerCommand({
        .name = "addMorphTarget",
        .doc = "Add targets to a morph (or the morph on a mesh, made if missing): the first argument, then target meshes. With no "
               "target meshes it adds one empty target named by name, to sculpt with editMorphTarget. Returns the new target nodes, "
               "whose weight attrs drive the morphs.",
        .positionalDoc = "morph or mesh, targets...",
        .flags = {{"live", "", T::Bool, false, "Keep the targets connected so editing them updates the morphs."},
                  {"name", "n", T::String, nullptr, "Export name for a single or empty target. Default: the target mesh's name."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("addMorphTarget: give the morph (or its mesh), then the targets");
            Node& first = nodeNamed(objs.front(), "addMorphTarget");
            NodeId m = kNoNode;
            if (first.type().isA("mesh") && find(graph, first.id()) == kNoNode && !first.type().isA("lattice"))
                m = create(graph, first.id(), "Morph");
            else
                m = resolveMorph(graph, objs.front(), "addMorphTarget");
            json out = json::array();
            if (objs.size() == 1)
            {
                out.push_back(graph.node(newTarget(graph, m, a.has("name") ? a.get<std::string>("name") : "Target"))->name());
                return out;
            }
            for (size_t i = 1; i < objs.size(); ++i)
            {
                NodeId t = meshNamed(objs[i], "addMorphTarget").id();
                std::string name = a.has("name") && objs.size() == 2 ? a.get<std::string>("name") : objs[i];
                out.push_back(graph.node(addTargetFromMesh(graph, m, t, name, a.get<bool>("live")))->name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "removeMorphTarget",
        .doc = "Remove morph targets (morphTarget nodes) from their morph. Pose drivers feeding them stay.",
        .positionalDoc = "targets",
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            for (const std::string& name : a.objects())
            {
                Node& n = targetNamed(name, "removeMorphTarget");
                if (auto e = graph.source({n.id(), kEditMesh, -1}))
                    graph.deleteNode(e->node);
                removeTarget(graph, n.id());
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "morphTargets",
        .doc = "A morph's targets in order: [{node, name, weight, sliderMin, sliderMax, live, inBetweens, driver, editing}]. "
               "driver is the plug setting the weight (a pose driver for a corrective), editing the edit mesh in edit mode, "
               "sliderMin and sliderMax the panel slider's range. Set a weight with setAttr(node + \".weight\", w).",
        .positionalDoc = "morph, mesh or target",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("morphTargets: no morph");
            json out = json::array();
            for (NodeId t : targetNodes(graph, resolveMorph(graph, objs.back(), "morphTargets")))
                out.push_back(targetInfo(graph, t));
            return out;
        },
    });

    ops::registerCommand({
        .name = "renameMorphTarget",
        .doc = "Rename a morph target: its export name (made unique on the morph) and its node, which becomes <name>Target. "
               "Returns the node's new name.",
        .positionalDoc = "target, newName",
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            if (a.positional.size() < 2)
                throw CommandError("renameMorphTarget: give the target and the new name");
            Node& t = targetNamed(a.positional[0].get<std::string>(), "renameMorphTarget");
            std::string wanted = a.positional[1].get<std::string>();
            if (wanted.empty())
                throw CommandError("renameMorphTarget: the name is empty");
            NodeId m = morphOf(graph, t.id());
            std::string name = m != kNoNode ? uniqueMorphName(graph, m, wanted, t.id()) : wanted;
            graph.setValue({t.id(), kMorphName, -1}, name);
            return guarded([&] { return graph.rename(t.id(), name + "Target"); });
        },
    });

    ops::registerCommand({
        .name = "extractMorphTarget",
        .doc = "Build a new mesh in a target's shape (base plus offsets): the full shape, or the in-between at inBetween=. "
               "Sculpt it and put it back with setMorphTarget, or use editMorphTarget. Returns the new mesh.",
        .positionalDoc = "target",
        .flags = {{"inBetween", "", T::Float, nullptr, "The in-between's weight; default the full shape."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("extractMorphTarget: no target");
            Node& n = targetNamed(objs.back(), "extractMorphTarget");
            NodeId m = morphOf(graph, n.id());
            if (m == kNoNode)
                throw CommandError("extractMorphTarget: " + n.name() + " is not connected to a morph");
            MeshPtr base = morphInput(graph, m);
            if (!base)
                throw CommandError("extractMorphTarget: the base has no geometry");
            const double w = a.has("inBetween") ? a.get<double>("inBetween") : 1.0;
            std::vector<glm::dvec3> off = guarded([&] { return shapeOffsets(graph, n.id(), w, *base); });
            auto shape = std::make_shared<geo::Mesh>(*base);
            for (size_t p = 0; p < off.size(); ++p)
                shape->setPosition(geo::Index(p), shape->position(geo::Index(p)) + off[p]);
            std::string morph = n.as<std::string>(kMorphName);
            NodeId baseMesh = modifiers::meshOf(graph, m);
            Node* mesh = duplicateShape(graph, baseMesh, shape, morph.empty() ? n.name() : morph);
            return mesh->name();
        },
    });

    ops::registerCommand({
        .name = "setMorphTarget",
        .doc = "Replace a target's shape with a mesh's (same point count as the base): the full shape, or the in-between at "
               "inBetween= (added if missing). A live target becomes stored offsets.",
        .positionalDoc = "target, mesh",
        .flags = {{"inBetween", "", T::Float, nullptr, "The in-between's weight; default the full shape."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("setMorphTarget: give the target, then the mesh");
            Node& t = targetNamed(objs[0], "setMorphTarget");
            Node& mesh = meshNamed(objs[1], "setMorphTarget");
            NodeId m = morphOf(graph, t.id());
            MeshPtr base = m != kNoNode ? morphInput(graph, m) : nullptr;
            MeshPtr shape = mesh.as<MeshPtr>(MeshNode::kOutMesh);
            if (!base || !shape || base->pointCapacity() != shape->pointCapacity())
                throw CommandError("setMorphTarget: " + mesh.name() + " doesn't match the base's points");
            guarded([&] {
                setShape(graph, t.id(), a.has("inBetween") ? a.get<double>("inBetween") : 1.0, deltasBetween(*base, *shape));
                return 0;
            });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "addInBetween",
        .doc = "Add an in-between shape to a target, reached at weight (say 0.5) on the way to the full shape at 1: from mesh when "
               "given, else the shape the target has at that weight now, ready to edit with editMorphTarget(inBetween=).",
        .positionalDoc = "target, mesh?",
        .flags = {{"weight", "w", T::Float, 0.5, "Where the in-between is reached; not 0 or 1."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("addInBetween: no target");
            Node& t = targetNamed(objs[0], "addInBetween");
            NodeId m = morphOf(graph, t.id());
            MeshPtr base = m != kNoNode ? morphInput(graph, m) : nullptr;
            if (!base)
                throw CommandError("addInBetween: " + t.name() + " is not on a morph with geometry");
            const double w = a.get<double>("weight");
            std::shared_ptr<MorphDeltas> d;
            if (objs.size() > 1)
            {
                MeshPtr shape = meshNamed(objs[1], "addInBetween").as<MeshPtr>(MeshNode::kOutMesh);
                if (!shape || shape->pointCapacity() != base->pointCapacity())
                    throw CommandError("addInBetween: " + objs[1] + " doesn't match the base's points");
                d = deltasBetween(*base, *shape);
            }
            else
                d = sparse(interpolatedOffsets(graph, t.id(), w, *base));
            guarded([&] {
                setInBetween(graph, t.id(), w, d);
                return 0;
            });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "removeInBetween",
        .doc = "Remove a target's in-between at weight.",
        .positionalDoc = "target",
        .flags = {{"weight", "w", T::Float, 0.5, "The in-between's weight."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("removeInBetween: no target");
            Node& t = targetNamed(objs[0], "removeInBetween");
            const double w = a.get<double>("weight");
            const auto* ib = dynamic_cast<const MorphInBetweens*>(t.as<DataPtr>(kInBetweens).get());
            auto next = std::make_shared<MorphInBetweens>();
            bool found = false;
            for (const MorphInBetweens::Shape& s : ib ? ib->shapes : std::vector<MorphInBetweens::Shape>{})
            {
                if (std::abs(s.weight - w) < kShapeEps)
                    found = true;
                else
                    next->shapes.push_back(s);
            }
            if (!found)
                throw CommandError(std::format("removeInBetween: {} has no in-between at {}", t.name(), w));
            graph.setValue({t.id(), kInBetweens, -1}, next->shapes.empty() ? DataPtr() : DataPtr(std::move(next)));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "mirrorMorphTarget",
        .doc = "Mirror a target through the base's symmetry (points matched across the axis plane in object space). mode flip "
               "makes a new target with the shape on the other side, named with left and right swapped; symmetrize copies one "
               "side onto the other in place. In-betweens go along. Returns the target written.",
        .positionalDoc = "target",
        .flags = {{"axis", "", T::String, "x", "Mirror axis, object space.", {"x", "y", "z"}},
                  {"mode", "", T::String, "flip", "flip | symmetrize", {"flip", "symmetrize"}},
                  {"direction", "", T::Int, 1, "symmetrize: 1 copies the positive side onto the negative, -1 the reverse.", {}, {}, -1.0, 1.0},
                  {"tolerance", "", T::Float, 1e-3, "How far a point may be from its partner's reflection, cm.", {}, {}, 0.0},
                  {"name", "", T::String, nullptr, "flip: the new target's name."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("mirrorMorphTarget: no target");
            Node& t = targetNamed(objs[0], "mirrorMorphTarget");
            NodeId m = morphOf(graph, t.id());
            MeshPtr base = m != kNoNode ? morphInput(graph, m) : nullptr;
            if (!base)
                throw CommandError("mirrorMorphTarget: " + t.name() + " is not on a morph with geometry");
            const int axis = axisIndex(a.get<std::string>("axis"), "mirrorMorphTarget");
            geo::SymmetryMap map = geo::SymmetryMap::build(*base, geo::axisPlane(axis), a.get<double>("tolerance"));
            if (map.unmatchedCount() > 0)
                log::warn("mirrorMorphTarget: {} points of the base have no mirror partner and keep their offsets", map.unmatchedCount());
            const bool flip = a.get<std::string>("mode") == "flip";
            const int keep = flip ? 0 : (a.get<int>("direction") >= 0 ? 1 : -1);
            NodeId out = t.id();
            if (flip)
            {
                std::string name = a.has("name") ? a.get<std::string>("name") : mirroredName(t.as<std::string>(kMorphName));
                out = newTarget(graph, m, name);
            }
            for (double w : shapeWeights(t))
            {
                std::vector<glm::dvec3> off = shapeOffsets(graph, t.id(), w, *base);
                setShape(graph, out, w, sparse(reflectOffsets(off, map, axis, keep)));
            }
            return graph.node(out)->name();
        },
    });

    ops::registerCommand({
        .name = "editMorphTarget",
        .doc = "Edit mode for sculpting a target: on makes an edit mesh in the target's shape (or the in-between at inBetween=) "
               "where the base is, hides the base and selects the edit mesh; the morph follows it live. off bakes the edit "
               "mesh into the target, deletes it and shows the base. Returns the edit mesh while on.",
        .positionalDoc = "target",
        .flags = {{"on", "", T::Bool, nullptr, "Default: toggle."}, {"inBetween", "", T::Float, nullptr, "Edit this in-between; default the full shape."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("editMorphTarget: no target");
            Node& t = targetNamed(objs[0], "editMorphTarget");
            NodeId m = morphOf(graph, t.id());
            NodeId baseMesh = m != kNoNode ? modifiers::meshOf(graph, m) : kNoNode;
            MeshPtr base = m != kNoNode ? morphInput(graph, m) : nullptr;
            if (!base || baseMesh == kNoNode)
                throw CommandError("editMorphTarget: " + t.name() + " is not on a mesh's morph");
            auto editing = graph.source({t.id(), kEditMesh, -1});
            const bool on = a.has("on") ? a.get<bool>("on") : !editing;
            if (on)
            {
                if (editing)
                    return graph.node(editing->node)->name();
                const double w = a.has("inBetween") ? a.get<double>("inBetween") : 1.0;
                std::vector<glm::dvec3> off = guarded([&] { return shapeOffsets(graph, t.id(), w, *base); });
                auto shape = std::make_shared<geo::Mesh>(*base);
                for (size_t p = 0; p < off.size(); ++p)
                    shape->setPosition(geo::Index(p), shape->position(geo::Index(p)) + off[p]);
                Node* edit = duplicateShape(graph, baseMesh, shape, t.as<std::string>(kMorphName) + "_edit");
                graph.setValue({t.id(), kEditWeight, -1}, w);
                graph.connect({edit->id(), MeshNode::kOutMesh, -1}, {t.id(), kEditMesh, -1});
                setVisible(graph, baseMesh, false);
                graph.select({edit->id()});
                return edit->name();
            }
            if (!editing)
                return nullptr;
            const NodeId editNode = editing->node;
            graph.evaluate();
            MeshPtr edited = graph.node(editNode)->as<MeshPtr>(MeshNode::kOutMesh);
            const double w = t.as<double>(kEditWeight);
            graph.disconnect({t.id(), kEditMesh, -1});
            if (edited && edited->pointCapacity() == base->pointCapacity())
                setShape(graph, t.id(), w, deltasBetween(*base, *edited));
            else
                log::warn("editMorphTarget: the edit mesh's points no longer match the base; the target is unchanged");
            graph.deleteNode(editNode);
            setVisible(graph, baseMesh, true);
            graph.select({baseMesh});
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "sculptCorrective",
        .doc = "Start a corrective: pose the rig, then this copies the mesh as it looks now into <mesh>_corrective, in place, and "
               "hides the mesh. Sculpt the fix on the copy, then run addCorrective. Returns the sculpt mesh.",
        .positionalDoc = "mesh",
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("sculptCorrective: no mesh");
            Node& mesh = meshNamed(objs.back(), "sculptCorrective");
            graph.evaluate();
            MeshPtr posed = mesh.as<MeshPtr>(MeshNode::kOutMesh);
            if (!posed)
                throw CommandError("sculptCorrective: " + mesh.name() + " has no geometry");
            Node* sculpt = duplicateShape(graph, mesh.id(), posed, mesh.name() + "_corrective");
            setVisible(graph, mesh.id(), false);
            graph.select({sculpt->id()});
            return sculpt->name();
        },
    });

    ops::registerCommand({
        .name = "addCorrective",
        .doc = "Turn a sculpt of the posed mesh into a corrective morph target. The fix is taken back through the skin into rest "
               "space (pose-space inversion) so it applies before skinning and matches the sculpt at this pose, and a pose driver "
               "reading each driver joint against its rest drives the weight: 1 at this pose, 0 at rest. Several drivers "
               "multiply (a combination corrective). Give the sculpt then the base (the base can be left out for a sculpt made "
               "by sculptCorrective). Returns {target, drivers, residual}.",
        .positionalDoc = "sculpt, base?",
        .flags = {{"driver", "d", T::Any, nullptr, "Joint name or list of joints. Default: the skin's most rotated influence."},
                  {"name", "n", T::String, nullptr, "The target's name. Default: <driver>_corrective."},
                  {"axis", "", T::String, "x", "Each driver's twist axis (its bone axis).", {"x", "y", "z"}},
                  {"radius", "", T::Float, 0.0, "Degrees from the pose where the weight reaches 0; 0 uses the rest-to-pose angle.", {}, {}, 0.0, 180.0},
                  {"twistWeight", "", T::Float, 0.0, "How much twist about the axis counts, 0 ignoring it.", {}, {}, 0.0},
                  {"falloff", "", T::String, "smooth", "linear | smooth | smoother", {"linear", "smooth", "smoother"}},
                  {"keep", "", T::Bool, false, "Keep the sculpt mesh instead of deleting it."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("addCorrective: give the sculpt, then the base");
            Node& sculpt = meshNamed(objs[0], "addCorrective");
            std::string baseName = objs.size() > 1 ? objs[1] : baseOfSculpt(sculpt.name());
            if (baseName.empty())
                throw CommandError("addCorrective: give the base mesh after the sculpt");
            Node& base = meshNamed(baseName, "addCorrective");

            std::vector<NodeId> driverJoints;
            if (a.has("driver"))
            {
                const json& d = a.flags["driver"];
                for (const json& j : d.is_array() ? d : json::array({d}))
                    driverJoints.push_back(nodeNamed(j.get<std::string>(), "addCorrective").id());
            }
            else
            {
                std::vector<NodeId> posed = posedInfluences(graph, base.id());
                if (posed.empty())
                    throw CommandError("addCorrective: no posed joint found on " + base.name() + "'s skin; give driver=");
                driverJoints.push_back(posed.front());
            }

            graph.evaluate();
            MeshPtr sculptMesh = sculpt.as<MeshPtr>(MeshNode::kOutMesh);
            if (!sculptMesh)
                throw CommandError("addCorrective: " + sculpt.name() + " has no geometry");
            const glm::dmat4 toBase = glm::inverse(worldOf(base)) * worldOf(sculpt);
            std::vector<glm::dvec3> target(sculptMesh->pointCapacity());
            for (geo::Index p = 0; p < sculptMesh->pointCapacity(); ++p)
                target[p] = glm::dvec3(toBase * glm::dvec4(sculptMesh->position(p), 1.0));

            NodeId m = find(graph, base.id());
            if (m == kNoNode)
                m = create(graph, base.id(), "Morph");
            std::string name = a.has("name") ? a.get<std::string>("name") : graph.node(driverJoints.front())->name() + "_corrective";
            NodeId t = newTarget(graph, m, name);

            const int axis = axisIndex(a.get<std::string>("axis"), "addCorrective");
            const std::string fall = a.get<std::string>("falloff");
            const drivers::Falloff falloff = fall == "linear" ? drivers::Falloff::Linear
                                             : fall == "smoother" ? drivers::Falloff::Smoother
                                                                  : drivers::Falloff::Smooth;
            json made = json::array();
            NodeId prev = kNoNode;
            for (NodeId j : driverJoints)
            {
                NodeId d = drivers::createPoseDriver(graph, j, graph.node(t)->as<std::string>(kMorphName) + "_" + graph.node(j)->name() + "Driver",
                                                     axis, a.get<double>("radius"), a.get<double>("twistWeight"), falloff);
                if (prev != kNoNode)
                    graph.connect({prev, drivers::kOutput, -1}, {d, drivers::kInWeight, -1});
                prev = d;
                made.push_back(graph.node(d)->name());
            }
            graph.connect({prev, drivers::kOutput, -1}, {t, kWeight, -1});

            double residual = 0.0;
            std::shared_ptr<MorphDeltas> deltas = invertSculpt(graph, t, target, &residual);
            graph.setValue({t, kDeltas, -1}, DataPtr(std::move(deltas)));
            if (!a.get<bool>("keep"))
                graph.deleteNode(sculpt.id());
            setVisible(graph, base.id(), true);
            graph.select({base.id()});
            return {{"target", graph.node(t)->name()}, {"drivers", made}, {"residual", residual}};
        },
    });
}
} // namespace morphs
} // namespace lila::scene
