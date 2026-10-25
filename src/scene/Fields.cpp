#include "scene/Fields.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/ObjectSets.h"
#include "scene/RigUtil.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace lila::scene::fields
{
using namespace rigutil;

namespace
{
constexpr const char* kKindNames[] = {"shape", "components", "painted", "proximity", "uvRegion", "noise", "attribute"};
constexpr const char* kShapeNames[] = {"sphere", "box", "capsule", "plane", "linear", "radial", "cone"};
constexpr const char* kBlendNames[] = {"add", "multiply", "max", "min", "subtract", "replace"};

template <class E, size_t N> E fromName(std::string_view name, const char* const (&names)[N], const char* what)
{
    for (size_t i = 0; i < N; ++i)
        if (name == names[i])
            return E(i);
    std::string all;
    for (const char* n : names)
        all += std::string(all.empty() ? "" : ", ") + n;
    throw std::invalid_argument(std::format("unknown {} '{}': use {}", what, name, all));
}

json vec(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

glm::dvec3 vecOf(const json& j, glm::dvec3 fallback)
{
    if (!j.is_array() || j.size() != 3)
        return fallback;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

uint32_t hash3(int x, int y, int z, uint32_t seed)
{
    uint32_t h = seed * 0x9E3779B9u;
    for (uint32_t v : {uint32_t(x), uint32_t(y), uint32_t(z)})
    {
        h ^= v + 0x7F4A7C15u + (h << 6) + (h >> 2);
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
    }
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

double lattice(int x, int y, int z, uint32_t seed) { return double(hash3(x, y, z, seed) & 0xFFFFFFu) / double(0xFFFFFF); }

double valueNoise(const glm::dvec3& p, uint32_t seed)
{
    glm::dvec3 f = glm::floor(p);
    glm::dvec3 t = p - f;
    glm::dvec3 u = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    int x = int(f.x), y = int(f.y), z = int(f.z);
    auto at = [&](int dx, int dy, int dz) { return lattice(x + dx, y + dy, z + dz, seed); };
    double x00 = glm::mix(at(0, 0, 0), at(1, 0, 0), u.x), x10 = glm::mix(at(0, 1, 0), at(1, 1, 0), u.x);
    double x01 = glm::mix(at(0, 0, 1), at(1, 0, 1), u.x), x11 = glm::mix(at(0, 1, 1), at(1, 1, 1), u.x);
    return glm::mix(glm::mix(x00, x10, u.y), glm::mix(x01, x11, u.y), u.z);
}

/** Per point values of a vertex colour set or point attribute channel. */
std::vector<float> attributeValues(const geo::Mesh& m, const Layer& l)
{
    std::vector<float> out(m.pointCapacity(), 0.0f);
    auto pick = [&](const glm::vec4& c) {
        return l.channel >= 0 && l.channel < 4 ? c[l.channel] : 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    };
    if (const geo::Attribute* a = l.attribute.empty() ? nullptr : m.pointAttrs().find(l.attribute))
    {
        int comps = geo::attrTypeComponents(a->type());
        for (size_t p = 0; p < std::min<size_t>(out.size(), a->size()); ++p)
        {
            glm::vec4 c(0.0f);
            switch (a->type())
            {
            case geo::AttrType::Float: c = glm::vec4(a->values<float>()[p]); break;
            case geo::AttrType::Float2: c = glm::vec4(a->values<glm::vec2>()[p], 0.0f, 0.0f); break;
            case geo::AttrType::Float3: c = glm::vec4(a->values<glm::vec3>()[p], 0.0f); break;
            case geo::AttrType::Float4: c = a->values<glm::vec4>()[p]; break;
            case geo::AttrType::Int: c = glm::vec4(float(a->values<int32_t>()[p])); break;
            case geo::AttrType::Bool: c = glm::vec4(a->values<bool>()[p] ? 1.0f : 0.0f); break;
            }
            out[p] = comps == 1 ? c.x : pick(c);
        }
        return out;
    }
    std::string set = l.attribute.empty() ? geo::colors::current(m) : l.attribute;
    if (set.empty() || !geo::colors::has(m, set))
        return out;
    std::vector<glm::vec4> colors = geo::colors::pointColors(m, set);
    for (size_t p = 0; p < std::min(out.size(), colors.size()); ++p)
        out[p] = pick(colors[p]);
    return out;
}

/** Average corner UV per point. */
std::vector<glm::dvec2> pointUvs(const geo::Mesh& m, int set)
{
    std::vector<glm::dvec2> uv(m.pointCapacity(), glm::dvec2(-1e9));
    std::span<const glm::vec2> c = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(set));
    if (c.empty())
        return uv;
    std::vector<int> count(m.pointCapacity(), 0);
    for (geo::Index k = 0; k < m.cornerCapacity(); ++k)
        if (m.cornerValid(k) && k < c.size())
        {
            geo::Index p = m.cornerPoint(k);
            uv[p] = count[p] ? uv[p] + glm::dvec2(c[k]) : glm::dvec2(c[k]);
            ++count[p];
        }
    for (size_t p = 0; p < uv.size(); ++p)
        if (count[p] > 1)
            uv[p] /= double(count[p]);
    return uv;
}

// ------------------------------------------------------------------------------------------------ node

class FieldNode final : public Transform
{
public:
    void compute(const EvalContext& ctx) override
    {
        LILA_ZONE();

        Transform::compute(ctx);
        auto e = std::make_shared<Evaluator>();
        const glm::dmat4& w = as<glm::dmat4>(kWorldMatrix);
        e->worldInverse = std::abs(glm::determinant(w)) > 1e-18 ? glm::inverse(w) : glm::dmat4(1.0);
        e->layers = std::dynamic_pointer_cast<const Layers>(as<DataPtr>(kLayers));
        e->opacity = as<double>(kOpacity);
        e->invert = as<bool>(kInvert);
        const MeshPtr& prox = as<MeshPtr>(kProximityMesh);
        bool wantsProximity = e->layers && std::ranges::any_of(e->layers->layers, [](const Layer& l) { return l.kind == Kind::Proximity; });
        if (prox && wantsProximity)
        {
            const glm::dmat4& pm = as<glm::dmat4>(kProximityMatrix);
            if (prox != mProxKey || pm != mProxMatrix)
            {
                auto world = std::make_shared<geo::Mesh>(*prox);
                for (geo::Index p = 0; p < world->pointCapacity(); ++p)
                    world->setPosition(p, glm::dvec3(pm * glm::dvec4(prox->position(p), 1.0)));
                mProxBvh = std::make_shared<geo::MeshBvh>(*world);
                mProxWorld = std::move(world);
                mProxKey = prox;
                mProxMatrix = pm;
            }
            e->proximityMesh = mProxWorld;
            e->proximity = mProxBvh;
        }
        setOutput(kOutField, DataPtr(std::move(e)));
    }

private:
    MeshPtr mProxKey;
    glm::dmat4 mProxMatrix{0.0};
    std::shared_ptr<const geo::Mesh> mProxWorld;
    std::shared_ptr<const geo::MeshBvh> mProxBvh;
};

// ------------------------------------------------------------------------------------------------ command helpers

Graph& g() { return Graph::current(); }

Node& fieldNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!isField(n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a field");
    return n;
}

Node& fieldArg(const Args& a, const char* cmd)
{
    auto objs = a.objects();
    for (const std::string& o : objs)
        if (Node* n = g().find(o); n && isField(*n))
            return *n;
    if (objs.empty())
        throw CommandError(std::string(cmd) + ": give a field");
    return fieldNamed(objs.front(), cmd);
}

size_t layerIndex(const Args& a, const std::vector<Layer>& ls, const char* cmd)
{
    int64_t i = a.get<int64_t>("index", -1);
    if (i < 0)
        i = int64_t(ls.size()) - 1;
    if (i < 0 || size_t(i) >= ls.size())
        throw CommandError(std::format("{}: no layer {} (the field has {})", cmd, i, ls.size()));
    return size_t(i);
}

Layer layerFromArgs(const json& params)
{
    try
    {
        return Layer::fromJson(params);
    }
    catch (const std::exception& e)
    {
        throw CommandError(e.what());
    }
}

/** Point ids for a components layer: given points, a set's members on the mesh, or the selected components. */
std::vector<uint32_t> selectedPoints(Graph& graph, const Args& a, const char* cmd)
{
    if (a.has("points"))
        return a.flags["points"].get<std::vector<uint32_t>>();
    std::vector<uint32_t> out;
    if (a.has("set"))
    {
        Node& set = nodeNamed(a.get<std::string>("set"), cmd);
        if (!objectSets::isSet(set))
            throw CommandError(std::string(cmd) + ": " + set.name() + " is not a set");
        graph.evaluate();
        for (const objectSets::Member& m : objectSets::members(graph, set.id()))
        {
            Node* n = graph.node(m.node);
            if (!n || m.kind == ComponentMode::Object || !n->type().isA("mesh"))
                continue;
            if (MeshPtr mesh = n->as<MeshPtr>(MeshNode::kOutMesh))
            {
                components::Ids ids = components::convert(*mesh, m.kind, m.ids, ComponentMode::Vertex);
                out.insert(out.end(), ids.begin(), ids.end());
            }
        }
    }
    else
        for (const auto& [node, _] : components::selected(components::mode()))
        {
            components::Ids ids = components::pointsOf(node);
            out.insert(out.end(), ids.begin(), ids.end());
        }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

geo::Bounds boundsOf(Graph& graph, const std::vector<NodeId>& meshes)
{
    graph.evaluate();
    geo::Bounds b;
    for (NodeId id : meshes)
    {
        Node* n = graph.node(id);
        MeshPtr m = n ? n->as<MeshPtr>(MeshNode::kOutMesh) : nullptr;
        if (!m)
            continue;
        glm::dmat4 w = worldOf(*n);
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                b.extend(glm::dvec3(w * glm::dvec4(m->position(p), 1.0)));
    }
    return b;
}
} // namespace

// ------------------------------------------------------------------------------------------------ names

const char* kindName(Kind k) { return kKindNames[size_t(k)]; }
const char* shapeName(Shape s) { return kShapeNames[size_t(s)]; }
const char* blendName(Blend b) { return kBlendNames[size_t(b)]; }
Kind kindFromName(std::string_view n) { return fromName<Kind>(n, kKindNames, "layer type"); }
Shape shapeFromName(std::string_view n) { return fromName<Shape>(n, kShapeNames, "shape"); }
Blend blendFromName(std::string_view n) { return fromName<Blend>(n, kBlendNames, "blend mode"); }

// ------------------------------------------------------------------------------------------------ Layer

glm::dmat4 Layer::matrix() const
{
    glm::dmat4 m = eulerToMatrix(rotate, RotateOrder::XYZ);
    m[3] = glm::dvec4(translate, 1.0);
    return m * glm::dmat4(glm::dvec4(scale.x, 0, 0, 0), glm::dvec4(0, scale.y, 0, 0), glm::dvec4(0, 0, scale.z, 0), glm::dvec4(0, 0, 0, 1));
}

json Layer::toJson() const
{
    json j = {{"type", kindName(kind)}, {"name", name}, {"enabled", enabled}, {"blend", blendName(blend)}, {"opacity", opacity},
              {"invert", invert}, {"inMin", inMin}, {"inMax", inMax}};
    if (curve)
        j["curve"] = curve->toJson();
    switch (kind)
    {
    case Kind::Shape:
        j.update({{"shape", shapeName(shape)}, {"softness", softness}, {"length", length}, {"angle", angle}});
        [[fallthrough]];
    case Kind::Noise:
        j.update({{"translate", vec(translate)}, {"rotate", vec(rotate)}, {"scale", vec(scale)}});
        if (kind == Kind::Noise)
            j.update({{"seed", seed}, {"frequency", frequency}, {"octaves", octaves}});
        break;
    case Kind::Components: j["points"] = points; break;
    case Kind::Painted: j["values"] = values; break;
    case Kind::Proximity: j.update({{"distance", distance}, {"softness", softness}}); break;
    case Kind::UvRegion:
        j.update({{"uvSet", uvSet}, {"uvMin", {uvMin.x, uvMin.y}}, {"uvMax", {uvMax.x, uvMax.y}}, {"softness", softness}});
        break;
    case Kind::Attribute: j.update({{"attribute", attribute}, {"channel", channel}}); break;
    }
    return j;
}

void Layer::update(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("layer params must be an object");
    if (j.contains("type"))
        kind = kindFromName(j["type"].get<std::string>());
    if (j.contains("shape"))
        shape = shapeFromName(j["shape"].get<std::string>());
    if (j.contains("blend"))
        blend = blendFromName(j["blend"].get<std::string>());
    name = j.value("name", name);
    enabled = j.value("enabled", enabled);
    opacity = j.value("opacity", opacity);
    invert = j.value("invert", invert);
    inMin = j.value("inMin", inMin);
    inMax = j.value("inMax", inMax);
    if (j.contains("curve"))
    {
        if (j["curve"].is_null() || (j["curve"].is_array() && j["curve"].empty()))
            curve.reset();
        else
            curve = softSelect::Curve::fromJson(j["curve"]);
    }
    if (j.contains("translate"))
        translate = vecOf(j["translate"], translate);
    if (j.contains("rotate"))
        rotate = vecOf(j["rotate"], rotate);
    if (j.contains("scale"))
        scale = vecOf(j["scale"], scale);
    softness = std::clamp(j.value("softness", softness), 0.0, 1.0);
    length = std::max(0.0, j.value("length", length));
    angle = std::clamp(j.value("angle", angle), 0.1, 179.0);
    if (j.contains("points"))
        points = j["points"].get<std::vector<uint32_t>>();
    if (j.contains("values"))
        values = j["values"].get<std::vector<float>>();
    distance = std::max(1e-6, j.value("distance", distance));
    uvSet = j.value("uvSet", uvSet);
    if (j.contains("uvMin"))
        uvMin = {j["uvMin"][0].get<double>(), j["uvMin"][1].get<double>()};
    if (j.contains("uvMax"))
        uvMax = {j["uvMax"][0].get<double>(), j["uvMax"][1].get<double>()};
    seed = j.value("seed", seed);
    frequency = j.value("frequency", frequency);
    octaves = std::clamp(j.value("octaves", octaves), 1, 8);
    attribute = j.value("attribute", attribute);
    channel = std::clamp(j.value("channel", channel), -1, 3);
}

Layer Layer::fromJson(const json& j)
{
    Layer l;
    l.update(j);
    if (l.kind == Kind::Proximity && !j.contains("softness"))
        l.softness = 1.0;
    return l;
}

size_t Layers::bytes() const
{
    size_t n = sizeof(Layers);
    for (const Layer& l : layers)
        n += sizeof(Layer) + l.points.size() * 4 + l.values.size() * 4;
    return n;
}

json Layers::toJson() const
{
    json list = json::array();
    for (const Layer& l : layers)
        list.push_back(l.toJson());
    return {{"kind", kind()}, {"layers", std::move(list)}};
}

DataPtr Layers::fromJson(const json& j)
{
    auto out = std::make_shared<Layers>();
    for (const json& l : j.value("layers", json::array()))
        try
        {
            out->layers.push_back(Layer::fromJson(l));
        }
        catch (const std::exception& e)
        {
            log::warn("field layer skipped: {}", e.what());
        }
    return out;
}

json Evaluator::toJson() const { return {{"kind", kind()}}; }

// ------------------------------------------------------------------------------------------------ math

double falloff(double d, double softness)
{
    if (d >= 1.0)
        return 0.0;
    double inner = 1.0 - std::clamp(softness, 0.0, 1.0);
    if (d <= inner)
        return 1.0;
    double t = (d - inner) / (1.0 - inner);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

double shapeValue(const Layer& l, const glm::dvec3& p)
{
    switch (l.shape)
    {
    case Shape::Sphere: return falloff(glm::length(p), l.softness);
    case Shape::Box: return falloff(std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}), l.softness);
    case Shape::Capsule:
    {
        double half = l.length * 0.5;
        glm::dvec3 q = p;
        q.y -= std::clamp(p.y, -half, half);
        return falloff(glm::length(q), l.softness);
    }
    case Shape::Plane: return falloff(std::abs(p.y), l.softness);
    case Shape::Linear: return std::clamp((p.y + 1.0) * 0.5, 0.0, 1.0);
    case Shape::Radial:
    {
        double a = std::atan2(-p.z, p.x);
        if (a < 0.0)
            a += 2.0 * std::numbers::pi;
        return a / (2.0 * std::numbers::pi);
    }
    case Shape::Cone:
    {
        if (p.y <= 0.0 || l.length <= 0.0)
            return 0.0;
        double ang = std::atan2(std::hypot(p.x, p.z), p.y);
        double d = std::max(ang / glm::radians(l.angle), p.y / l.length);
        return falloff(d, l.softness);
    }
    }
    return 0.0;
}

double noise(const glm::dvec3& p, uint32_t seed, int octaves)
{
    double sum = 0.0, amp = 1.0, total = 0.0;
    glm::dvec3 q = p;
    for (int o = 0; o < std::max(1, octaves); ++o)
    {
        sum += amp * valueNoise(q, seed + uint32_t(o) * 1013u);
        total += amp;
        amp *= 0.5;
        q *= 2.0;
    }
    return sum / total;
}

double remap(const Layer& l, double raw)
{
    double t;
    if (std::abs(l.inMax - l.inMin) < 1e-12)
        t = raw >= l.inMin ? 1.0 : 0.0;
    else
        t = std::clamp((raw - l.inMin) / (l.inMax - l.inMin), 0.0, 1.0);
    if (l.curve)
        t = l.curve->evaluate(t);
    return l.invert ? 1.0 - t : t;
}

double blend(Blend mode, double b, double v, double o)
{
    double r = b;
    switch (mode)
    {
    case Blend::Add: r = b + v * o; break;
    case Blend::Subtract: r = b - v * o; break;
    case Blend::Multiply: r = b * (1.0 - o + o * v); break;
    case Blend::Max: r = b + (std::max(b, v) - b) * o; break;
    case Blend::Min: r = b + (std::min(b, v) - b) * o; break;
    case Blend::Replace: r = b + (v - b) * o; break;
    }
    return std::clamp(r, 0.0, 1.0);
}

std::vector<float> evaluate(const Evaluator& e, std::span<const glm::dvec3> world, const geo::Mesh* mesh)
{
    LILA_ZONE();

    const size_t n = world.size();
    std::vector<double> value(n, 0.0);
    if (e.layers)
        for (const Layer& l : e.layers->layers)
        {
            if (!l.enabled || l.opacity <= 0.0)
                continue;
            std::vector<char> member;
            std::vector<float> perPoint;
            std::vector<glm::dvec2> uv;
            if (l.kind == Kind::Components)
            {
                member.assign(n, 0);
                for (uint32_t p : l.points)
                    if (p < n)
                        member[p] = 1;
            }
            else if (l.kind == Kind::Attribute && mesh)
                perPoint = attributeValues(*mesh, l);
            else if (l.kind == Kind::UvRegion && mesh)
                uv = pointUvs(*mesh, l.uvSet);
            if (l.kind == Kind::Proximity && !e.proximity)
                continue;
            const glm::dmat4 toLayer = glm::inverse(l.matrix()) * e.worldInverse;
            const glm::dvec2 uvCenter = (l.uvMin + l.uvMax) * 0.5, uvHalf = glm::max(glm::abs(l.uvMax - l.uvMin) * 0.5, glm::dvec2(1e-9));
            tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 512), [&](const tbb::blocked_range<size_t>& r) {
                for (size_t i = r.begin(); i != r.end(); ++i)
                {
                    double raw = 0.0;
                    switch (l.kind)
                    {
                    case Kind::Shape: raw = shapeValue(l, glm::dvec3(toLayer * glm::dvec4(world[i], 1.0))); break;
                    case Kind::Noise: raw = noise(glm::dvec3(toLayer * glm::dvec4(world[i], 1.0)) * l.frequency, l.seed, l.octaves); break;
                    case Kind::Components: raw = member[i]; break;
                    case Kind::Painted: raw = i < l.values.size() ? l.values[i] : 0.0; break;
                    case Kind::Attribute: raw = i < perPoint.size() ? perPoint[i] : 0.0; break;
                    case Kind::UvRegion:
                        if (i < uv.size())
                        {
                            glm::dvec2 q = glm::abs(uv[i] - uvCenter) / uvHalf;
                            raw = falloff(std::max(q.x, q.y), l.softness);
                        }
                        break;
                    case Kind::Proximity:
                    {
                        geo::BvhClosest c;
                        if (e.proximity->closestPoint(world[i], c, l.distance))
                            raw = falloff(c.distance / l.distance, l.softness);
                        break;
                    }
                    }
                    value[i] = blend(l.blend, value[i], remap(l, raw), std::clamp(l.opacity, 0.0, 1.0));
                }
            });
        }
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i)
    {
        double v = e.invert ? 1.0 - value[i] : value[i];
        out[i] = float(std::clamp(v * e.opacity, 0.0, 1.0));
    }
    return out;
}

std::vector<float> evaluate(const Evaluator& e, const geo::Mesh& mesh, const glm::dmat4& meshWorld)
{
    std::vector<glm::dvec3> world(mesh.pointCapacity());
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
        world[p] = glm::dvec3(meshWorld * glm::dvec4(mesh.position(p), 1.0));
    return evaluate(e, world, &mesh);
}

std::shared_ptr<const Evaluator> evaluator(Graph& graph, NodeId field)
{
    Node* n = graph.node(field);
    if (!n || !isField(*n))
        return nullptr;
    graph.evaluate();
    return std::dynamic_pointer_cast<const Evaluator>(n->as<DataPtr>(kOutField));
}

bool isField(const Node& n) { return n.type().isA("field"); }

std::vector<Layer> layers(Graph& graph, NodeId field)
{
    const auto* l = dynamic_cast<const Layers*>(std::get<DataPtr>(graph.storedValue({field, kLayers, -1})).get());
    return l ? l->layers : std::vector<Layer>{};
}

void setLayers(Graph& graph, NodeId field, std::vector<Layer> ls)
{
    auto d = std::make_shared<Layers>();
    d->layers = std::move(ls);
    graph.setValue({field, kLayers, -1}, DataPtr(std::move(d)));
}

void modulateSoftSelection(const geo::Mesh& mesh, const glm::dmat4& world, std::vector<float>& weights)
{
    const PrefStore& prefs = PrefStore::get();
    std::string name = prefs.value<std::string>("general", kSoftSelectFieldPref, "");
    if (name.empty() || !prefs.value<bool>("general", softSelect::kEnabled, false))
        return;
    Graph& graph = Graph::current();
    Node* n = graph.find(name);
    if (!n || !isField(*n))
        return;
    auto e = std::dynamic_pointer_cast<const Evaluator>(n->as<DataPtr>(kOutField));
    if (!e)
        return;
    std::vector<float> f = evaluate(*e, mesh, world);
    for (size_t p = 0; p < std::min(f.size(), weights.size()); ++p)
        weights[p] *= f[p];
}

// ------------------------------------------------------------------------------------------------ registration

void registerTypes()
{
    if (types::find("field"))
        return;
    data::registerKind("fieldLayers", &Layers::fromJson);
    auto in = [](std::string name, PlugType type, PlugValue def, uint32_t flags, std::string doc) {
        return AttrDef{.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = flags, .doc = std::move(doc)};
    };
    NodeType t;
    t.name = "field";
    t.icon = "chart-donut";
    t.base = types::find("transform");
    t.dag = true;
    t.doc = "A falloff field: a stack of layers (shapes, noise, components, painted maps, proximity, UV regions, attributes) "
            "blended into a 0 to 1 value per point. Connect it to deformers to shape where they act, or use it for soft "
            "selection.";
    t.ownAttrs = {
        in("layers", PlugType::Data, DataPtr(), AttrInput | AttrStorable | AttrHidden,
           "The layer stack, bottom first. Edit it with addFieldLayer, setFieldLayer, moveFieldLayer and removeFieldLayer."),
        in("opacity", PlugType::Float, 1.0, AttrInput | AttrStorable | AttrKeyable, "Scales the whole field's value."),
        in("invert", PlugType::Bool, false, AttrInput | AttrStorable | AttrKeyable, "Flips the field's value (1 - value) before opacity."),
        in("proximityMesh", PlugType::Mesh, MeshPtr(), AttrInput | AttrHidden, "The mesh proximity layers measure distance to (its outMesh)."),
        in("proximityMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrInput | AttrHidden, "The proximity mesh's worldMatrix."),
        in("outField", PlugType::Data, DataPtr(), AttrOutput | AttrHidden, "The evaluated field, for deformers' field input."),
    };
    t.create = [] { return std::make_unique<FieldNode>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    using T = ArgType;
    static const std::vector<std::string> kTypeChoices(std::begin(kKindNames), std::end(kKindNames));
    static const std::vector<std::string> kShapeChoices(std::begin(kShapeNames), std::end(kShapeNames));

    ops::registerCommand({
        .name = "field",
        .doc = "Create a falloff field with a first shape layer, placed and sized on the given or selected meshes (or at the "
               "origin). Deformers among the objects get the field connected. Returns the field.",
        .positionalDoc = "meshes or deformers",
        .flags = {{"shape", "", T::String, "sphere", "First layer's shape, or none for an empty stack.", [] {
                       auto c = kShapeChoices;
                       c.push_back("none");
                       return c;
                   }()},
                  {"position", "", T::Vec3, nullptr, "World position. Default: the centre of the meshes."},
                  {"radius", "", T::Float, nullptr, "Field scale, cm. Default: a quarter of the meshes' size."},
                  {"name", "", T::String, nullptr, "Name for the field."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<NodeId> meshes, defs;
            for (const std::string& o : a.objects())
            {
                Node* n = graph.find(o);
                if (!n)
                    throw CommandError("field: no node named '" + o + "'");
                if (deformers::isDeformer(*n))
                {
                    defs.push_back(n->id());
                    if (NodeId m = modifiers::meshOf(graph, n->id()); m != kNoNode)
                        meshes.push_back(m);
                }
                else if (n->type().isA("mesh"))
                    meshes.push_back(n->id());
            }
            geo::Bounds b = boundsOf(graph, meshes);
            glm::dvec3 pos = a.has("position") ? vec3Of(a.flags["position"]) : b.valid() ? b.center() : glm::dvec3(0.0);
            double radius = a.has("radius") ? a.get<double>("radius") : b.valid() ? std::max(glm::length(b.size()) * 0.25, 1e-3) : 10.0;
            Node* f = guarded([&] { return graph.createNode(*types::find("field"), a.has("name") ? a.get<std::string>("name") : "Field", kNoNode, true); });
            graph.setValue({f->id(), Transform::kTranslate, -1}, pos);
            graph.setValue({f->id(), Transform::kScale, -1}, glm::dvec3(radius));
            std::string shape = a.get<std::string>("shape");
            if (shape != "none")
            {
                Layer l;
                l.shape = guarded([&] { return shapeFromName(shape); });
                l.name = shape;
                setLayers(graph, f->id(), {l});
            }
            for (NodeId d : defs)
                graph.connect({f->id(), kOutField, -1}, {d, DeformerNode::kField, -1}, true);
            graph.select({f->id()});
            return f->name();
        },
    });

    ops::registerCommand({
        .name = "addFieldLayer",
        .doc = "Add a layer to a field (on top unless index= is given) and return its index. Components layers take points=, "
               "set= or the selected components; proximity layers take mesh=.",
        .positionalDoc = "field",
        .flags = {{"type", "", T::String, "shape", "The layer type.", kTypeChoices},
                  {"shape", "", T::String, nullptr, "Shape layers: the shape.", kShapeChoices},
                  {"blend", "", T::String, nullptr, "Blend mode: add, multiply, max (default), min, subtract or replace.",
                   {"add", "multiply", "max", "min", "subtract", "replace"}},
                  {"params", "", T::Any, nullptr, "Other layer settings as {key: value} (see fieldLayers for the keys)."},
                  {"points", "", T::List, nullptr, "Components layers: point ids."},
                  {"set", "", T::String, nullptr, "Components layers: take the points of this set's components."},
                  {"mesh", "", T::String, nullptr, "Proximity layers: the mesh to measure to (connected to the field)."},
                  {"index", "", T::Int, nullptr, "Insert at this position, 0 the bottom."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            Node& f = fieldArg(a, "addFieldLayer");
            json params = a.has("params") ? a.flags["params"] : json::object();
            if (!params.is_object())
                throw CommandError("addFieldLayer: params must be an object");
            params["type"] = a.get<std::string>("type");
            if (a.has("shape"))
                params["shape"] = a.get<std::string>("shape");
            if (a.has("blend"))
                params["blend"] = a.get<std::string>("blend");
            Layer l = layerFromArgs(params);
            if (l.name.empty())
                l.name = l.kind == Kind::Shape ? shapeName(l.shape) : kindName(l.kind);
            if (l.kind == Kind::Components && !params.contains("points"))
                l.points = selectedPoints(graph, a, "addFieldLayer");
            if (l.kind == Kind::Proximity && a.has("mesh"))
            {
                Node& m = nodeNamed(a.get<std::string>("mesh"), "addFieldLayer");
                if (!m.type().isA("mesh"))
                    throw CommandError("addFieldLayer: " + m.name() + " is not a mesh");
                graph.connect({m.id(), MeshNode::kOutMesh, -1}, {f.id(), kProximityMesh, -1}, true);
                graph.connect({m.id(), Transform::kWorldMatrix, -1}, {f.id(), kProximityMatrix, -1}, true);
            }
            std::vector<Layer> ls = layers(graph, f.id());
            size_t at = a.has("index") ? std::min<size_t>(size_t(std::max<int64_t>(0, a.get<int64_t>("index"))), ls.size()) : ls.size();
            ls.insert(ls.begin() + ptrdiff_t(at), std::move(l));
            setLayers(graph, f.id(), std::move(ls));
            return at;
        },
    });

    ops::registerCommand({
        .name = "setFieldLayer",
        .doc = "Change a field layer's settings: params= {key: value} with any of the keys fieldLayers lists (type, shape, "
               "blend, opacity, invert, inMin, inMax, curve, translate, rotate, scale, softness, points, values, ...).",
        .positionalDoc = "field",
        .flags = {{"index", "", T::Int, nullptr, "The layer, 0 the bottom. Default: the top."},
                  {"params", "", T::Any, nullptr, "The settings to change."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            Node& f = fieldArg(a, "setFieldLayer");
            std::vector<Layer> ls = layers(graph, f.id());
            size_t i = layerIndex(a, ls, "setFieldLayer");
            try
            {
                ls[i].update(a.has("params") ? a.flags["params"] : json::object());
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("setFieldLayer: ") + e.what());
            }
            setLayers(graph, f.id(), std::move(ls));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "removeFieldLayer",
        .doc = "Remove a layer from a field.",
        .positionalDoc = "field",
        .flags = {{"index", "", T::Int, nullptr, "The layer, 0 the bottom. Default: the top."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            Node& f = fieldArg(a, "removeFieldLayer");
            std::vector<Layer> ls = layers(graph, f.id());
            ls.erase(ls.begin() + ptrdiff_t(layerIndex(a, ls, "removeFieldLayer")));
            setLayers(graph, f.id(), std::move(ls));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "moveFieldLayer",
        .doc = "Move a field layer to another position in the stack (0 the bottom, applied first).",
        .positionalDoc = "field",
        .flags = {{"index", "", T::Int, nullptr, "The layer to move. Default: the top."}, {"to", "", T::Int, 0, "Its new position."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            Node& f = fieldArg(a, "moveFieldLayer");
            std::vector<Layer> ls = layers(graph, f.id());
            size_t from = layerIndex(a, ls, "moveFieldLayer");
            size_t to = std::min<size_t>(size_t(std::max<int64_t>(0, a.get<int64_t>("to"))), ls.size() - 1);
            Layer l = std::move(ls[from]);
            ls.erase(ls.begin() + ptrdiff_t(from));
            ls.insert(ls.begin() + ptrdiff_t(to), std::move(l));
            setLayers(graph, f.id(), std::move(ls));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "fieldLayers",
        .doc = "A field's layers, bottom first, each as its settings.",
        .positionalDoc = "field",
        .undoable = false,
        .run = [](const Args& a) -> json {
            json out = json::array();
            for (const Layer& l : layers(g(), fieldArg(a, "fieldLayers").id()))
                out.push_back(l.toJson());
            return out;
        },
    });

    ops::registerCommand({
        .name = "sampleField",
        .doc = "A field's value (0 to 1) at every point of a mesh, by point id.",
        .positionalDoc = "field, mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("sampleField: give a field and a mesh");
            Node& f = fieldNamed(objs[0], "sampleField");
            Node& m = nodeNamed(objs[1], "sampleField");
            auto e = evaluator(graph, f.id());
            MeshPtr mesh = m.type().isA("mesh") ? m.as<MeshPtr>(MeshNode::kOutMesh) : nullptr;
            if (!e || !mesh)
                throw CommandError("sampleField: " + m.name() + " has no geometry");
            return evaluate(*e, *mesh, worldOf(m));
        },
    });

    ops::registerCommand({
        .name = "softSelectField",
        .doc = "Make soft selection read a field: soft select weights are multiplied by the field's value. No field clears it. "
               "A preference, so not undoable.",
        .positionalDoc = "field",
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name;
            if (!a.positional.empty())
                name = fieldNamed(a.positional[0].get<std::string>(), "softSelectField").path();
            PrefStore::get().set("general", kSoftSelectFieldPref, name);
            return name;
        },
    });
}
} // namespace lila::scene::fields
