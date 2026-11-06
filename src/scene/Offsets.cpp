#include "scene/Offsets.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/AnimCurve.h"
#include "scene/Graph.h"
#include "scene/TransformOps.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace lila::scene::offsets
{
namespace
{
using anim::BlendNode;
using anim::LayerNode;

constexpr double kEps = 1e-9;
const char* kSpaceNames[] = {"local", "parent", "world", "node"};

/** Attrs the offset types add after their base's, in this order. */
enum Own
{
    kEnvelope,
    kSpace,
    kCompensate,
    kParentWorld,
    kParentRest,
    kSpaceWorld,
    kPreRotation,
    kRestWorld,
    kHasRange,
    kRangeStart,
    kRangeEnd,
    kEase,
    kOwnCount,
};

glm::dquat rotationOf(const glm::dmat4& m)
{
    glm::dmat3 r(m);
    glm::dvec3 x = glm::length(r[0]) > 1e-12 ? glm::normalize(r[0]) : glm::dvec3(1, 0, 0);
    glm::dvec3 y = r[1] - glm::dot(r[1], x) * x;
    y = glm::length(y) > 1e-12 ? glm::normalize(y) : glm::dvec3(0, 1, 0);
    glm::dvec3 z = glm::cross(x, y);
    return glm::normalize(glm::quat_cast(glm::dmat3(x, y, z)));
}

glm::dquat quatOf(const glm::dvec3& euler, RotateOrder order)
{
    return glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(euler, order))));
}

glm::dquat powQ(glm::dquat q, double e)
{
    if (q.w < 0.0)
        q = -q;
    return glm::normalize(glm::slerp(glm::dquat(1, 0, 0, 0), q, e));
}

glm::dvec3 vecOf(const PlugValue& v)
{
    if (const glm::dvec3* x = std::get_if<glm::dvec3>(&v))
        return *x;
    if (const double* d = std::get_if<double>(&v))
        return glm::dvec3(*d, 0.0, 0.0);
    return glm::dvec3(0.0);
}

/**
 * The blend of one offset joint. Rotation: the joint's local rotate R becomes R' with
 *   world rotation = O_s^w * (base world rotation), O_s = S * O * S^-1 about the pivot,
 * that is R' = Pre^-1 * P^-1 * O_s^w * B * Pre * R, where P is the actual parent rotation, B the parent's rotation
 * without this layer when compensating (else P), Pre the joint orient and S the space's rotation. Local space applies
 * O after R instead. Translation adds w * M * d with M taking the space's axes into the parent frame.
 */
class OffsetBlendNode final : public BlendNode
{
public:
    explicit OffsetBlendNode(bool rot) : mBase(rot ? BlendNode::kRotateOrder + 1 : BlendNode::kOutput + 1) { rotation = rot; }

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const Frame f = frame();
        const glm::dvec3 a = vecOf(value(kInputA)), b = vecOf(value(kInputB));
        if (rotation)
        {
            glm::dquat out = f.left * quatOf(a, f.order) * f.right;
            setOutput(kOutput, xformops::closestEuler(glm::mat3_cast(out), f.order, a));
            setOutput(at(kRestWorld), glm::mat4_cast(f.rest * f.pre * quatOf(a, f.order)));
        }
        else
            setOutput(kOutput, a + f.w * (f.m * b));
    }

    std::optional<glm::dvec3> solveInputB(const glm::dvec3& o) const override
    {
        const Frame f = frame();
        if (std::abs(f.w) < 1e-6)
            throw std::invalid_argument("offset " + name() + " has no weight at this frame; pose it where it applies");
        const glm::dvec3 a = vecOf(value(kInputA)), b = vecOf(value(kInputB));
        if (!rotation)
            return glm::inverse(f.m) * (o - a) / f.w;
        const glm::dquat r = quatOf(a, f.order), d = quatOf(o, f.order);
        glm::dquat ctrl;
        if (f.space == Space::Local)
            ctrl = powQ(glm::inverse(f.inner * r) * d, 1.0 / f.w);
        else
            ctrl = glm::inverse(f.s) * powQ(f.p * f.pre * d * glm::inverse(r) * glm::inverse(f.pre) * glm::inverse(f.base), 1.0 / f.w) * f.s;
        return xformops::closestEuler(glm::mat3_cast(ctrl), f.order, b);
    }

    std::optional<glm::dvec3> solveInputA(const glm::dvec3& o) const override
    {
        const Frame f = frame();
        const glm::dvec3 a = vecOf(value(kInputA)), b = vecOf(value(kInputB));
        if (!rotation)
            return o - f.w * (f.m * b);
        glm::dquat r = glm::inverse(f.left) * quatOf(o, f.order) * glm::inverse(f.right);
        return xformops::closestEuler(glm::mat3_cast(r), f.order, a);
    }

    bool staticInputB() const override { return true; }

    int at(Own k) const { return mBase + int(k); }

private:
    struct Frame
    {
        double w = 1.0;
        Space space = Space::Parent;
        RotateOrder order = RotateOrder::XYZ;
        glm::dquat p{1, 0, 0, 0}, rest{1, 0, 0, 0}, base{1, 0, 0, 0}, s{1, 0, 0, 0}, pre{1, 0, 0, 0};
        glm::dquat inner{1, 0, 0, 0}; // Pre^-1 P^-1 B Pre: what compensation alone does to R
        glm::dquat left{1, 0, 0, 0}, right{1, 0, 0, 0}; // R' = left * R * right
        glm::dmat3 m{1.0};
    };

    Frame frame() const
    {
        Frame f;
        f.w = as<double>(kWeight) * as<double>(at(kEnvelope));
        f.space = Space(std::clamp<int64_t>(as<int64_t>(at(kSpace)), 0, 3));
        const glm::dmat4& pw = as<glm::dmat4>(at(kParentWorld));
        const glm::dmat4& sw = as<glm::dmat4>(at(kSpaceWorld));
        if (!rotation)
        {
            if (f.space == Space::World)
                f.m = glm::inverse(glm::dmat3(pw));
            else if (f.space == Space::Node)
                f.m = glm::inverse(glm::dmat3(pw)) * glm::dmat3(sw);
            return f;
        }
        f.order = RotateOrder(std::clamp<int64_t>(as<int64_t>(kRotateOrder), 0, 5));
        f.p = rotationOf(pw);
        f.rest = rotationOf(as<glm::dmat4>(at(kParentRest)));
        f.base = as<bool>(at(kCompensate)) ? f.rest : f.p;
        f.pre = rotationOf(as<glm::dmat4>(at(kPreRotation)));
        f.s = f.space == Space::World ? glm::dquat(1, 0, 0, 0) : f.space == Space::Node ? rotationOf(sw) : f.p;
        const glm::dquat o = quatOf(vecOf(value(kInputB)), f.order);
        f.inner = glm::inverse(f.pre) * glm::inverse(f.p) * f.base * f.pre;
        if (f.space == Space::Local)
        {
            f.left = f.inner;
            f.right = powQ(o, f.w);
        }
        else
            f.left = glm::inverse(f.pre) * glm::inverse(f.p) * f.s * powQ(o, f.w) * glm::inverse(f.s) * f.base * f.pre;
        return f;
    }

    int mBase;
};

int ownAttr(const Node& n, Own k)
{
    return n.type().attrIndex(std::array<const char*, kOwnCount>{"envelope", "offsetSpace", "compensate", "parentWorld",
                                                                  "parentRest", "spaceWorld", "preRotation", "restWorld",
                                                                  "hasRange", "rangeStart", "rangeEnd", "ease"}[k]);
}

Graph& graph() { return Graph::current(); }

/** Blends hanging off the layer's weight. */
std::vector<NodeId> blendsOf(Graph& g, NodeId layer)
{
    std::vector<NodeId> out;
    for (const PlugRef& d : g.destinations({layer, LayerNode::kOutWeight, -1}))
        if (Node* n = g.node(d.node); n && isOffsetBlend(*n) && std::find(out.begin(), out.end(), d.node) == out.end())
            out.push_back(d.node);
    return out;
}

NodeId envelopeCurve(Graph& g, NodeId layer)
{
    for (NodeId b : blendsOf(g, layer))
        if (auto src = g.source({b, ownAttr(*g.node(b), kEnvelope), -1}); src && anim::isCurve(*g.node(src->node)))
            return src->node;
    return kNoNode;
}

anim::Curve envelopeKeys(const Range& r)
{
    using anim::Tangent;
    anim::Curve c;
    const double frame = 1.0 / std::max(1.0, anim::playback().fps);
    double ease = std::clamp(r.ease, 0.0, std::max(0.0, (r.end - r.start) * 0.5));
    if (ease <= kEps)
    {
        // Full strength on every frame of the range, none outside.
        c.insert({.time = r.start - frame, .value = 0.0, .in = Tangent::Flat, .out = Tangent::Step});
        c.insert({.time = r.start, .value = 1.0, .in = Tangent::Flat, .out = Tangent::Flat});
        c.insert({.time = r.end, .value = 1.0, .in = Tangent::Flat, .out = Tangent::Step});
        c.insert({.time = r.end + frame, .value = 0.0, .in = Tangent::Flat, .out = Tangent::Flat});
        return c;
    }
    c.insert({.time = r.start, .value = 0.0, .in = Tangent::Flat, .out = Tangent::Flat});
    c.insert({.time = r.start + ease, .value = 1.0, .in = Tangent::Flat, .out = Tangent::Flat});
    if (r.end - ease > r.start + ease + kEps)
        c.insert({.time = r.end - ease, .value = 1.0, .in = Tangent::Flat, .out = Tangent::Flat});
    c.insert({.time = r.end, .value = 0.0, .in = Tangent::Flat, .out = Tangent::Flat});
    return c;
}

void writeRange(Graph& g, NodeId blend, const std::optional<Range>& r)
{
    const Node& n = *g.node(blend);
    g.setValue({blend, ownAttr(n, kHasRange), -1}, r.has_value());
    g.setValue({blend, ownAttr(n, kRangeStart), -1}, r ? r->start : 0.0);
    g.setValue({blend, ownAttr(n, kRangeEnd), -1}, r ? r->end : 0.0);
    g.setValue({blend, ownAttr(n, kEase), -1}, r ? r->ease : 0.0);
}

std::string bareName(const std::string& n)
{
    size_t c = n.rfind(':');
    return c == std::string::npos ? n : n.substr(c + 1);
}

std::string nameFor(Graph& g, const std::vector<NodeId>& joints)
{
    std::string out = "Offset";
    for (size_t i = 0; i < joints.size() && i < 2; ++i)
        out += "_" + bareName(g.node(joints[i])->name());
    if (joints.size() > 2)
        out += "_plus" + std::to_string(joints.size() - 2);
    return out;
}

int depthOf(const Graph& g, NodeId id)
{
    int d = 0;
    for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
        ++d;
    return d;
}

bool isTransformNode(const Node* n) { return n && n->type().isA("transform"); }

double frames(double seconds) { return anim::frameOf(seconds); }

std::optional<Range> rangeArg(const Args& a, std::optional<Range> fallback, const char* cmd)
{
    std::optional<Range> r = fallback;
    if (a.has("range"))
    {
        const json& v = a.flags["range"];
        if (v.is_string() && (v == "whole" || v == "take"))
            r.reset();
        else if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number())
        {
            double s = anim::secondsOf(v[0].get<double>()), e = anim::secondsOf(v[1].get<double>());
            if (e < s)
                std::swap(s, e);
            r = Range{s, e, r ? r->ease : 0.0};
        }
        else
            throw CommandError(std::string(cmd) + ": range is 'whole' or [start, end] in frames");
    }
    if (a.has("ease") && r)
        r->ease = anim::secondsOf(a.get<double>("ease")) - anim::secondsOf(0.0);
    return r;
}

NodeId offsetNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !anim::isLayer(*n) || !isOffsetLayer(g, n->id()))
        throw CommandError(std::string(cmd) + ": no offset named '" + name + "'");
    return n->id();
}

json infoJson(Graph& g, const Info& i)
{
    auto names = [&](const std::vector<NodeId>& ids) {
        json out = json::array();
        for (NodeId id : ids)
            out.push_back(g.node(id)->path());
        return out;
    };
    json j = {{"name", g.node(i.layer)->path()},
              {"joints", names(i.joints)},
              {"compensated", names(i.compensated)},
              {"space", i.space == Space::Node && i.spaceNode ? g.node(i.spaceNode)->path() : spaceName(i.space)},
              {"weight", i.weight},
              {"mute", i.mute},
              {"range", nullptr},
              {"ease", 0.0}};
    if (i.range)
    {
        j["range"] = json::array({frames(i.range->start), frames(i.range->end)});
        j["ease"] = frames(i.range->ease) - frames(0.0);
    }
    return j;
}

template <class F> auto guarded(F&& f)
{
    try
    {
        return f();
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(e.what());
    }
}
} // namespace

const char* spaceName(Space s) { return kSpaceNames[std::clamp(int(s), 0, 3)]; }

Space spaceFromName(const std::string& s)
{
    for (int i = 0; i < 3; ++i)
        if (s == kSpaceNames[i])
            return Space(i);
    return Space::Node;
}

bool isOffsetBlend(const Node& n) { return n.type().isA(kRotationBlendType) || n.type().isA(kVectorBlendType); }

bool isOffsetLayer(Graph& g, NodeId layer) { return g.node(layer) && anim::isLayer(*g.node(layer)) && !blendsOf(g, layer).empty(); }

std::vector<NodeId> list(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId l : anim::layers(g))
        if (isOffsetLayer(g, l))
            out.push_back(l);
    return out;
}

NodeId blendOf(Graph& g, NodeId layer, NodeId joint, bool translate)
{
    NodeId b = anim::blendFor(g, {joint, translate ? Transform::kTranslate : Transform::kRotate, -1}, layer);
    return b && isOffsetBlend(*g.node(b)) ? b : kNoNode;
}

NodeId create(Graph& g, const std::vector<NodeId>& jointsIn, const Options& opts)
{
    LILA_ZONE();

    std::vector<NodeId> joints;
    for (NodeId j : jointsIn)
    {
        if (!isTransformNode(g.node(j)))
            throw std::invalid_argument("createOffset: " + (g.node(j) ? g.node(j)->path() : std::string("?")) + " is not a transform");
        if (std::find(joints.begin(), joints.end(), j) == joints.end())
            joints.push_back(j);
    }
    if (joints.empty())
        throw std::invalid_argument("createOffset: give the joints to offset");
    if (!opts.rotate && !opts.translate)
        throw std::invalid_argument("createOffset: offset rotate, translate or both");
    if (opts.space == Space::Node && !g.node(opts.spaceNode))
        throw std::invalid_argument("createOffset: no space node");

    std::set<NodeId> offset(joints.begin(), joints.end()), comp;
    if (opts.keepChildren)
    {
        // Joints between two offset ones carry the rest pose down the chain, so they join with a zero control.
        for (NodeId j : joints)
        {
            std::vector<NodeId> path;
            for (NodeId p = g.parent(j); p != kNoNode && isTransformNode(g.node(p)); p = g.parent(p))
            {
                if (offset.contains(p))
                {
                    offset.insert(path.begin(), path.end());
                    break;
                }
                path.push_back(p);
            }
        }
        for (NodeId j : offset)
            for (NodeId c : g.children(j))
                if (isTransformNode(g.node(c)) && !offset.contains(c))
                    comp.insert(c);
    }
    std::vector<NodeId> ordered(offset.begin(), offset.end());
    ordered.insert(ordered.end(), comp.begin(), comp.end());
    std::stable_sort(ordered.begin(), ordered.end(), [&](NodeId a, NodeId b) { return depthOf(g, a) < depthOf(g, b); });

    const NodeId layer = anim::createLayer(g, opts.name.empty() ? nameFor(g, joints) : opts.name, anim::LayerMode::Additive);
    NodeId curve = kNoNode;
    if (opts.range)
    {
        curve = g.createNode(*types::find("animCurveTU"), g.node(layer)->name() + "_envelope", kNoNode, false)->id();
        anim::setCurve(g, curve, envelopeKeys(*opts.range));
    }

    g.evaluate();
    auto setup = [&](NodeId b, NodeId n, bool compensate) {
        const Node& bn = *g.node(b);
        g.setValue({b, ownAttr(bn, kSpace), -1}, int64_t(opts.space));
        g.setValue({b, ownAttr(bn, kCompensate), -1}, compensate);
        if (const auto* t = dynamic_cast<const Transform*>(g.node(n)))
            g.setValue({b, ownAttr(bn, kPreRotation), -1}, t->preRotation());
        writeRange(g, b, opts.range);
        const NodeId parent = g.parent(n);
        if (isTransformNode(g.node(parent)))
        {
            g.connect({parent, Transform::kWorldMatrix, -1}, {b, ownAttr(bn, kParentWorld), -1});
            const NodeId parentBlend = blendOf(g, layer, parent);
            if (parentBlend)
                g.connect({parentBlend, ownAttr(*g.node(parentBlend), kRestWorld), -1}, {b, ownAttr(bn, kParentRest), -1});
            else
                g.connect({parent, Transform::kWorldMatrix, -1}, {b, ownAttr(bn, kParentRest), -1});
        }
        if (opts.space == Space::Node)
        {
            try
            {
                g.connect({opts.spaceNode, Transform::kWorldMatrix, -1}, {b, ownAttr(bn, kSpaceWorld), -1});
            }
            catch (const std::invalid_argument&)
            {
                throw std::invalid_argument("createOffset: " + g.node(opts.spaceNode)->path() + " moves with " + g.node(n)->path() +
                                            "; pick a space above it");
            }
        }
        if (curve)
            g.connect({curve, anim::CurveNode::kOutput, -1}, {b, ownAttr(bn, kEnvelope), -1});
    };
    for (NodeId n : ordered)
    {
        const bool compensate = comp.contains(n);
        if (opts.rotate || compensate)
            setup(anim::addMember(g, layer, {n, Transform::kRotate, -1}, kRotationBlendType), n, compensate);
        if (opts.translate && !compensate)
            setup(anim::addMember(g, layer, {n, Transform::kTranslate, -1}, kVectorBlendType), n, false);
    }
    ops::execute("setActiveAnimLayer", {{"args", {g.node(layer)->path()}}});
    g.evaluate();
    return layer;
}

Info info(Graph& g, NodeId layer)
{
    Info out;
    out.layer = layer;
    const Node* l = g.node(layer);
    if (!l)
        return out;
    out.weight = l->as<double>(LayerNode::kWeight);
    out.mute = l->as<bool>(LayerNode::kMute);
    bool first = true;
    for (const PlugRef& m : anim::layerMembers(g, layer))
    {
        NodeId b = anim::blendFor(g, m, layer);
        if (!b || !isOffsetBlend(*g.node(b)))
            continue;
        const Node& bn = *g.node(b);
        auto& bucket = bn.as<bool>(ownAttr(bn, kCompensate)) ? out.compensated : out.joints;
        if (std::find(bucket.begin(), bucket.end(), m.node) == bucket.end())
            bucket.push_back(m.node);
        if (!std::exchange(first, false))
            continue;
        out.space = Space(std::clamp<int64_t>(bn.as<int64_t>(ownAttr(bn, kSpace)), 0, 3));
        if (auto src = g.source({b, ownAttr(bn, kSpaceWorld), -1}))
            out.spaceNode = src->node;
        if (bn.as<bool>(ownAttr(bn, kHasRange)))
            out.range = Range{bn.as<double>(ownAttr(bn, kRangeStart)), bn.as<double>(ownAttr(bn, kRangeEnd)), bn.as<double>(ownAttr(bn, kEase))};
    }
    return out;
}

void setControl(Graph& g, NodeId layer, NodeId joint, std::optional<glm::dvec3> rotate, std::optional<glm::dvec3> translate)
{
    const std::string name = g.node(joint) ? g.node(joint)->path() : std::string("?");
    if (rotate)
    {
        NodeId b = blendOf(g, layer, joint);
        if (!b)
            throw std::invalid_argument("editOffset: " + name + " has no rotation in this offset");
        g.setValue({b, BlendNode::kInputB, -1}, *rotate);
    }
    if (translate)
    {
        NodeId b = blendOf(g, layer, joint, true);
        if (!b)
            throw std::invalid_argument("editOffset: " + name + " has no translation in this offset");
        g.setValue({b, BlendNode::kInputB, -1}, *translate);
    }
}

void setRange(Graph& g, NodeId layer, std::optional<Range> range)
{
    NodeId curve = envelopeCurve(g, layer);
    std::vector<NodeId> blends = blendsOf(g, layer);
    if (!range)
    {
        if (curve)
            g.deleteNode(curve);
        for (NodeId b : blends)
        {
            g.setValue({b, ownAttr(*g.node(b), kEnvelope), -1}, 1.0);
            writeRange(g, b, range);
        }
        return;
    }
    if (!curve)
        curve = g.createNode(*types::find("animCurveTU"), g.node(layer)->name() + "_envelope", kNoNode, false)->id();
    anim::setCurve(g, curve, envelopeKeys(*range));
    for (NodeId b : blends)
    {
        const PlugRef env{b, ownAttr(*g.node(b), kEnvelope), -1};
        if (!g.source(env))
            g.connect({curve, anim::CurveNode::kOutput, -1}, env);
        writeRange(g, b, range);
    }
}

void remove(Graph& g, NodeId layer)
{
    if (NodeId curve = envelopeCurve(g, layer))
        g.deleteNode(curve);
    ops::execute("deleteAnimLayer", {{"args", {g.node(layer)->path()}}});
}

void bake(Graph& g, NodeId layer)
{
    NodeId curve = envelopeCurve(g, layer);
    ops::execute("mergeAnimLayers", {{"args", {g.node(layer)->path()}}});
    if (curve && g.node(curve))
        g.deleteNode(curve);
}

void registerTypes()
{
    if (types::find(kRotationBlendType))
        return;
    const uint32_t stored = AttrInput | AttrStorable;
    const uint32_t hidden = AttrInput | AttrStorable | AttrHidden;
    const uint32_t wired = AttrInput | AttrHidden;
    std::vector<AttrDef> own = {
        {.name = "envelope", .type = PlugType::Float, .defaultValue = 1.0, .flags = stored | AttrKeyable,
         .doc = "Eases the offset in and out over its range; driven by the offset's envelope curve.", .minValue = 0.0, .maxValue = 1.0},
        {.name = "offsetSpace", .type = PlugType::Enum, .defaultValue = int64_t(Space::Parent), .flags = stored,
         .enumNames = {std::begin(kSpaceNames), std::end(kSpaceNames)}, .doc = "Axes the control value is in."},
        {.name = "compensate", .type = PlugType::Bool, .defaultValue = false, .flags = stored,
         .doc = "Keep the world rotation the joint had without this layer (a child of an offset joint)."},
        {.name = "parentWorld", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = wired, .doc = "The joint's parent's world matrix."},
        {.name = "parentRest", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = wired,
         .doc = "The parent's world rotation without this layer."},
        {.name = "spaceWorld", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = wired, .doc = "The space node's world matrix."},
        {.name = "preRotation", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hidden, .doc = "The joint's orient."},
        {.name = "restWorld", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrOutput | AttrHidden,
         .doc = "The joint's world rotation without this layer, for its children's compensation."},
        {.name = "hasRange", .type = PlugType::Bool, .defaultValue = false, .flags = hidden, .doc = "The offset eases over a range."},
        {.name = "rangeStart", .type = PlugType::Float, .defaultValue = 0.0, .flags = hidden, .doc = "Seconds."},
        {.name = "rangeEnd", .type = PlugType::Float, .defaultValue = 0.0, .flags = hidden, .doc = "Seconds."},
        {.name = "ease", .type = PlugType::Float, .defaultValue = 0.0, .flags = hidden, .doc = "Seconds of ease at each end.", .minValue = 0.0},
    };
    for (bool rot : {true, false})
    {
        NodeType t;
        t.name = rot ? kRotationBlendType : kVectorBlendType;
        t.base = types::find(rot ? "animBlendRotation" : "animBlendVector");
        if (!t.base)
            throw std::logic_error("offsets::registerTypes needs the layer types first");
        t.doc = "An offset layer's blend: one control value turned into a per-frame additive change in the offset's space.";
        t.ownAttrs = own;
        t.create = [rot] { return std::make_unique<OffsetBlendNode>(rot); };
        types::registerType(std::move(t));
    }
}

void registerCommands()
{
    if (ops::find("createOffset"))
        return;
    using T = ArgType;
    const FlagDef rangeFlag{"range", "r", T::Any, nullptr, "'whole' (constant over the take) or [start, end] in frames, eased in and out."};
    const FlagDef easeFlag{"ease", "e", T::Float, nullptr, "Frames of ease at each end of the range.", {}, {}, 0.0};

    ops::registerCommand({
        .name = "createOffset",
        .doc = "Offset body parts non-destructively for polish: a new additive layer (made active) holding one adjustable "
               "offset per joint, constant over the take or eased over a range. Pose on it at any frame and the whole "
               "take moves together; weight, mute, bake or delete it like any layer. Returns the layer.",
        .positionalDoc = "joints",
        .flags = {
            {"space", "s", T::String, "parent", "local, parent, world, or a node whose axes the offset uses."},
            rangeFlag,
            {"ease", "e", T::Float, 0.0, "Frames of ease at each end of the range.", {}, {}, 0.0},
            {"keepChildren", "kc", T::Bool, false, "Children keep their world rotation (offset the torso, keep the head's direction)."},
            {"channels", "ch", T::String, "both", "rotate, translate or both.", {"rotate", "translate", "both"}},
            {"name", "n", T::String, nullptr, "Layer name. Default: Offset_<parts>."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> joints;
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n)
                    throw CommandError("createOffset: no node named '" + name + "'");
                joints.push_back(n->id());
            }
            Options opts;
            const std::string space = a.get<std::string>("space", "parent");
            opts.space = spaceFromName(space);
            if (opts.space == Space::Node)
            {
                Node* s = g.find(space);
                if (!s || !isTransformNode(s))
                    throw CommandError("createOffset: space is local, parent, world or a transform, not '" + space + "'");
                opts.spaceNode = s->id();
            }
            opts.range = rangeArg(a, std::nullopt, "createOffset");
            const std::string ch = a.get<std::string>("channels", "both");
            opts.rotate = ch != "translate";
            opts.translate = ch != "rotate";
            opts.keepChildren = a.get<bool>("keepChildren");
            opts.name = a.get<std::string>("name", "");
            return g.node(guarded([&] { return create(g, joints, opts); }))->path();
        },
    });

    ops::registerCommand({
        .name = "listOffsets",
        .doc = "Offset layers bottom first: [{name, joints, compensated, space, range (frames or None), ease, weight, mute}].",
        .undoable = false,
        .run = [](const Args&) -> json {
            Graph& g = graph();
            json out = json::array();
            for (NodeId l : list(g))
                out.push_back(infoJson(g, info(g, l)));
            return out;
        },
    });

    ops::registerCommand({
        .name = "editOffset",
        .doc = "Change an offset: weight, mute, range and ease, or a joint's control values directly. Returns its info.",
        .positionalDoc = "offset",
        .flags = {
            {"weight", "w", T::Float, nullptr, "Layer weight."},
            {"mute", "m", T::Bool, nullptr, "Mute the offset."},
            rangeFlag,
            easeFlag,
            nodeFlag({"joint", "j", T::String, nullptr, "Joint whose control rotate/translate set."}, "joint"),
            {"rotate", "ro", T::Vec3, nullptr, "Rotation control, degrees in the offset's space."},
            {"translate", "tr", T::Vec3, nullptr, "Translation control, cm in the offset's space."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.positional.size() != 1)
                throw CommandError("editOffset: name one offset");
            NodeId l = offsetNamed(g, a.objects().front(), "editOffset");
            guarded([&] {
                if (a.has("weight"))
                    g.setValue({l, LayerNode::kWeight, -1}, a.get<double>("weight"));
                if (a.has("mute"))
                    g.setValue({l, LayerNode::kMute, -1}, a.get<bool>("mute"));
                if (a.has("range") || a.has("ease"))
                    setRange(g, l, rangeArg(a, info(g, l).range, "editOffset"));
                if (a.has("rotate") || a.has("translate"))
                {
                    if (!a.has("joint"))
                        throw CommandError("editOffset: rotate and translate need a joint");
                    Node* j = g.find(a.get<std::string>("joint"));
                    if (!j)
                        throw CommandError("editOffset: no joint named '" + a.get<std::string>("joint") + "'");
                    auto vec = [&](const char* f) -> std::optional<glm::dvec3> {
                        if (!a.has(f))
                            return std::nullopt;
                        const json& v = a.flags[f];
                        return glm::dvec3(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
                    };
                    setControl(g, l, j->id(), vec("rotate"), vec("translate"));
                }
                return 0;
            });
            g.evaluate();
            return infoJson(g, info(g, l));
        },
    });

    ops::registerCommand({
        .name = "deleteOffset",
        .doc = "Delete offsets: their layers, blends and envelopes. The animation underneath is untouched.",
        .positionalDoc = "offsets",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            int n = 0;
            for (const std::string& name : a.objects())
            {
                remove(g, offsetNamed(g, name, "deleteOffset"));
                ++n;
            }
            return n;
        },
    });

    ops::registerCommand({
        .name = "bakeOffset",
        .doc = "Merge offsets down into the layer below (or the base animation), sampled every frame across the keys.",
        .positionalDoc = "offsets",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            int n = 0;
            for (const std::string& name : a.objects())
            {
                bake(g, offsetNamed(g, name, "bakeOffset"));
                ++n;
            }
            return n;
        },
    });
}
} // namespace lila::scene::offsets
