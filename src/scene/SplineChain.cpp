// Spline chains: the splineChain and splineJoint nodes, making and editing them.

#include "scene/SplineChain.h"
#include "scene/SplineChainSolve.h"

#include "core/Profile.h"
#include "curve/Along.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/RigMath.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace lila::scene::spline
{
using D3 = glm::dvec3;
using rigmath::rotationOf;

namespace
{
const std::vector<std::string> kLengthModes = {"keepLength", "stretch", "squash"};
const std::vector<std::string> kUpModes = {"curveFrames", "upVector", "upObjects"};
const std::vector<std::string> kRamps = {"linear", "easeIn", "easeOut", "smooth"};

constexpr uint32_t kStored = AttrInput | AttrStorable;
constexpr uint32_t kKeyable = AttrInput | AttrStorable | AttrKeyable;
constexpr uint32_t kHiddenStored = AttrInput | AttrStorable | AttrHidden;
constexpr uint32_t kRestArray = AttrInput | AttrStorable | AttrHidden | AttrArray;

AttrDef attr(const char* name, PlugType type, PlugValue def, uint32_t flags, const char* doc, std::optional<double> lo = {},
             std::optional<double> hi = {})
{
    AttrDef a{.name = name, .type = type, .defaultValue = std::move(def), .flags = flags, .doc = doc, .minValue = lo, .maxValue = hi};
    if (type == PlugType::Vec3)
    {
        const std::string n = name;
        a.childNames = {n + "X", n + "Y", n + "Z"};
    }
    return a;
}

AttrDef choice(const char* name, int64_t def, const std::vector<std::string>& names, const char* doc)
{
    return {.name = name, .type = PlugType::Enum, .defaultValue = def, .flags = kStored, .enumNames = names, .doc = doc};
}

/** The solve every splineJoint reads its share of. Never stored: it is recomputed from the curve. */
struct ChainSolution final : Data
{
    struct Pose
    {
        glm::dmat3 local{1.0}; // rotation in the parent joint's solved frame (the start joint's real parent for the first)
        D3 translate{0.0};     // the first joint's translate
        double stretch = 1.0;  // the joint's ikStretch
        D3 scale{1.0};
    };
    std::vector<Pose> poses;
    double weight = 0.0;

    const char* kind() const override { return "splineChainSolution"; }
    nlohmann::json toJson() const override { return {{"kind", kind()}}; }
    size_t bytes() const override { return sizeof(*this) + poses.size() * sizeof(Pose); }
};

class SplineChainNode : public Node
{
public:
    enum Attr
    {
        kInCurve,
        kCurveName,
        kStartJoint,
        kEndJoint,
        kParentMatrix,
        kWeight,
        kLengthMode,
        kMinStretch,
        kMaxStretch,
        kVolume,
        kSlide,
        kTwistStart,
        kTwistEnd,
        kTwistRamp,
        kRoll,
        kUpMode,
        kUpVector,
        kUpStartMatrix,
        kUpEndMatrix,
        kUpStartAxis,
        kUpEndAxis,
        kTwistSpread,
        kRestLength,
        kRestChord,
        kBoneLength,
        kRestOffset,
        kScaleAxis,
        kRestCurveLength,
        kJointCount,
        kSolution,
        kDynamics,
    };

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        auto sol = std::make_shared<ChainSolution>();
        sol->weight = std::clamp(as<double>(kWeight), 0.0, 1.0);
        const size_t n = size_t(std::max<int64_t>(as<int64_t>(kJointCount), 0));
        const curve::Splines& splines = curve::splinesOf(value(kInCurve));
        const glm::dmat4 parent = as<glm::dmat4>(kParentMatrix);
        const glm::dmat3 parentRot = rotationOf(parent);
        const UpMode upMode = UpMode(std::clamp<int64_t>(as<int64_t>(kUpMode), 0, 2));
        D3 up = parentRot * as<D3>(kUpVector);
        if (n >= 2 && !splines.empty())
        {
            const void* key = as<DataPtr>(kInCurve).get();
            if (key != mRailCurve || upMode != mRailMode || up != mRailUp)
            {
                mRail = curve::Rail(splines.front(), railOptions(upMode, up));
                mRailCurve = key;
                mRailMode = upMode;
                mRailUp = up;
            }
        }
        if (n >= 2 && !splines.empty() && mRail.valid())
        {
            const solve::Rest rest = this->rest(n);
            solve::Params p = params();
            if (upMode == UpMode::UpObjects)
            {
                p.upStart = rotationOf(as<glm::dmat4>(kUpStartMatrix)) * as<D3>(kUpStartAxis);
                p.upEnd = rotationOf(as<glm::dmat4>(kUpEndMatrix)) * as<D3>(kUpEndAxis);
            }
            const solve::Solved s = solve::solve(mRail, p, rest);
            const glm::dmat4 toParent = glm::inverse(parent);
            const bool squash = p.mode == LengthMode::Squash;
            const double volume = as<double>(kVolume);
            sol->poses.resize(n);
            glm::dmat3 prev = parentRot;
            for (size_t i = 0; i < n; ++i)
            {
                ChainSolution::Pose& pose = sol->poses[i];
                const glm::dmat3 world = s.frame[i] * rest.offset[i];
                pose.local = glm::transpose(prev) * world;
                prev = world;
                if (i == 0)
                    pose.translate = D3(toParent * glm::dvec4(s.p[0], 1.0));
                else if (rest.bone[i] > 1e-9)
                    pose.stretch = glm::length(s.p[i] - s.p[i - 1]) / rest.bone[i];
                if (squash)
                {
                    const double across = std::pow(std::max(s.ratio[i], 1e-6), -0.5 * volume);
                    const int along = int(std::clamp<int64_t>(elementOr<int64_t>(kScaleAxis, int(i)), 0, 2));
                    pose.scale = D3(across);
                    pose.scale[along] = 1.0;
                }
            }
        }
        setOutput(kSolution, DataPtr(std::move(sol)));
    }

    solve::Rest rest(size_t n) const
    {
        solve::Rest r;
        r.curveLength = as<double>(kRestCurveLength);
        for (size_t i = 0; i < n; ++i)
        {
            r.s.push_back(elementOr<double>(kRestLength, int(i)));
            r.chord.push_back(elementOr<double>(kRestChord, int(i)));
            r.bone.push_back(elementOr<double>(kBoneLength, int(i)));
            r.offset.push_back(glm::dmat3(elementOr<glm::dmat4>(kRestOffset, int(i))));
        }
        return r;
    }

    solve::Params params() const
    {
        solve::Params p;
        p.mode = LengthMode(std::clamp<int64_t>(as<int64_t>(kLengthMode), 0, 2));
        p.slide = as<double>(kSlide);
        p.minStretch = as<double>(kMinStretch);
        p.maxStretch = std::max(as<double>(kMaxStretch), p.minStretch);
        p.twistStart = as<double>(kTwistStart);
        p.twistEnd = as<double>(kTwistEnd);
        p.roll = as<double>(kRoll);
        p.ramp = as<int64_t>(kTwistRamp);
        p.spread = as<double>(kTwistSpread);
        return p;
    }

    static curve::RailOptions railOptions(UpMode mode, const D3& up)
    {
        curve::RailOptions ro;
        ro.up = mode == UpMode::UpVector ? curve::UpMode::AutoNormals : curve::UpMode::CurveFrames;
        ro.upVector = glm::length(up) > 1e-12 ? glm::normalize(up) : D3(0, 1, 0);
        return ro;
    }

private:
    const void* mRailCurve = nullptr;
    UpMode mRailMode = UpMode::CurveFrames;
    D3 mRailUp{0.0};
    curve::Rail mRail;
};

class SplineJointNode : public Node
{
public:
    enum Attr
    {
        kSolution,
        kIndex,
        kOffset,
        kJointOrient,
        kRotateAxis,
        kRotateOrder,
        kRestTranslate,
        kSolvedRotate,
        kIkBlend,
        kStretch,
        kScale,
        kTranslate,
    };

    void compute(const EvalContext&) override
    {
        const auto* sol = dynamic_cast<const ChainSolution*>(as<DataPtr>(kSolution).get());
        const int64_t i = as<int64_t>(kIndex);
        const D3 rest = as<D3>(kRestTranslate);
        if (!sol || i < 0 || size_t(i) >= sol->poses.size())
        {
            setOutput(kIkBlend, 0.0);
            setOutput(kStretch, 1.0);
            setOutput(kScale, D3(1.0));
            setOutput(kTranslate, rest);
            return;
        }
        const ChainSolution::Pose& pose = sol->poses[size_t(i)];
        const double w = sol->weight;
        const auto order = RotateOrder(std::clamp<int64_t>(as<int64_t>(kRotateOrder), 0, 5));
        const glm::dmat3 jo(eulerToMatrix(as<D3>(kJointOrient), RotateOrder::XYZ));
        const glm::dmat3 ra(eulerToMatrix(as<D3>(kRotateAxis), RotateOrder::XYZ));
        const glm::dmat3 r = glm::transpose(jo) * pose.local * glm::transpose(ra) * glm::dmat3(eulerToMatrix(as<D3>(kOffset), order));
        setOutput(kSolvedRotate, eulerNear(r, order, as<D3>(kSolvedRotate)));
        setOutput(kIkBlend, w);
        setOutput(kStretch, 1.0 + (pose.stretch - 1.0) * w);
        setOutput(kScale, D3(1.0) + (pose.scale - D3(1.0)) * w);
        setOutput(kTranslate, rest + (pose.translate - rest) * w);
    }
};

void registerTypesImpl()
{
    NodeType chain{.name = kChainType,
                   .doc = "A spline chain: lays its joints along a curve by arc length, keeping their length (sliding), stretching "
                          "or stretching with a volume-keeping squash, twisted from start to end and turned by the curve's frames, an "
                          "up vector or an object at each end.",
                   .icon = "vector-spline",
                   .create = [] { return std::make_unique<SplineChainNode>(); }};
    chain.ownAttrs = {
        attr("inCurve", PlugType::Data, DataPtr(), AttrInput | AttrHidden, "The curve's worldSpace."),
        attr("curve", PlugType::String, std::string(), kStored, "The curve the joints ride."),
        attr("startJoint", PlugType::String, std::string(), kStored, "The first joint."),
        attr("endJoint", PlugType::String, std::string(), kStored, "The last joint."),
        attr("parentMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrInput | AttrHidden, "The start joint's parent world matrix."),
        attr("weight", PlugType::Float, 1.0, kKeyable, "0 leaves the joints on their own rotate, 1 lays them on the curve.", 0.0, 1.0),
        choice("lengthMode", int64_t(LengthMode::Stretch), kLengthModes,
               "keepLength: the joints keep their spacing and slide. stretch: they spread with the curve's length. squash: stretch, "
               "thinning as they lengthen to keep their volume."),
        attr("minStretch", PlugType::Float, 0.25, kKeyable, "The shortest the chain gets, as a multiple of its length.", 0.0),
        attr("maxStretch", PlugType::Float, 4.0, kKeyable, "The longest the chain gets, as a multiple of its length.", 0.0),
        attr("volume", PlugType::Float, 1.0, kKeyable, "Squash: 1 keeps the volume, 0 keeps the thickness.", 0.0),
        attr("slide", PlugType::Float, 0.0, kKeyable, "Moves the chain along the curve, cm."),
        attr("twistStart", PlugType::Float, 0.0, kKeyable, "Twist at the first joint, degrees."),
        attr("twistEnd", PlugType::Float, 0.0, kKeyable, "Twist at the last joint, degrees."),
        choice("twistRamp", 0, kRamps, "How the twist goes from start to end."),
        attr("roll", PlugType::Float, 0.0, kKeyable, "Turns the whole chain about the curve, degrees."),
        choice("upMode", int64_t(UpMode::CurveFrames), kUpModes,
               "curveFrames: twist-free frames from the up vector. upVector: up leans toward the up vector everywhere. upObjects: "
               "the frames twist to follow upStart and upEnd."),
        attr("upVector", PlugType::Vec3, D3(0, 1, 0), kKeyable, "Up, in the start joint's parent's space."),
        attr("upStartMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrInput | AttrHidden, "The start up object's world matrix."),
        attr("upEndMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrInput | AttrHidden, "The end up object's world matrix."),
        attr("upStartAxis", PlugType::Vec3, D3(0, 1, 0), kHiddenStored, "Up in the start up object's space."),
        attr("upEndAxis", PlugType::Vec3, D3(0, 1, 0), kHiddenStored, "Up in the end up object's space."),
        attr("twistSpread", PlugType::Float, 1.0, kKeyable, "upObjects: the share of the end object's twist spread along the chain.", 0.0, 1.0),
        attr("restLength", PlugType::Float, 0.0, kRestArray, "Each joint's arc length along the curve at rest."),
        attr("restChord", PlugType::Float, 0.0, kRestArray, "Each joint's distance from the previous one at rest."),
        attr("boneLength", PlugType::Float, 0.0, kRestArray, "Each joint's translate length."),
        attr("restOffset", PlugType::Matrix, glm::dmat4(1.0), kRestArray, "Each joint's rotation in its curve frame at rest."),
        attr("scaleAxis", PlugType::Int, int64_t(0), kRestArray, "Each joint's axis along its bone, kept unscaled by squash."),
        attr("restCurveLength", PlugType::Float, 0.0, kHiddenStored, "The curve's length at rest."),
        attr("jointCount", PlugType::Int, int64_t(0), kHiddenStored, "Joints in the chain."),
        attr("solution", PlugType::Data, DataPtr(), AttrOutput | AttrHidden, "The solved joints, read by the splineJoints."),
        attr("dynamics", PlugType::String, std::string(), kStored, "The simulation splineDynamics set up on the joints, if any."),
    };

    NodeType joint{.name = kJointType,
                   .doc = "Hands one joint its share of a spline chain: rotation, stretch, squash and, for the first joint, its place on "
                          "the curve. offset turns the joint on top, like FK.",
                   .icon = "bone",
                   .create = [] { return std::make_unique<SplineJointNode>(); }};
    joint.ownAttrs = {
        attr("solution", PlugType::Data, DataPtr(), AttrInput | AttrHidden, "The chain's solution."),
        attr("index", PlugType::Int, int64_t(0), kHiddenStored, "The joint's place in the chain."),
        attr("offset", PlugType::Vec3, D3(0.0), kKeyable, "FK rotation on top of the chain, degrees in the joint's rotate order."),
        attr("jointOrient", PlugType::Vec3, D3(0.0), kHiddenStored, "The joint's jointOrient."),
        attr("rotateAxis", PlugType::Vec3, D3(0.0), kHiddenStored, "The joint's rotateAxis."),
        attr("rotateOrder", PlugType::Int, int64_t(0), kHiddenStored, "The joint's rotate order.", 0.0, 5.0),
        attr("restTranslate", PlugType::Vec3, D3(0.0), kHiddenStored, "The joint's translate when the chain was made."),
        attr("solvedRotate", PlugType::Vec3, D3(0.0), AttrOutput | AttrHidden, "Rotate values for the joint."),
        attr("ikBlend", PlugType::Float, 0.0, AttrOutput | AttrHidden, "The joint's ikBlend: the chain's weight."),
        attr("stretch", PlugType::Float, 1.0, AttrOutput | AttrHidden, "The joint's ikStretch."),
        attr("scale", PlugType::Vec3, D3(1.0), AttrOutput | AttrHidden, "The joint's scale."),
        attr("translate", PlugType::Vec3, D3(0.0), AttrOutput | AttrHidden, "The first joint's translate."),
    };
    for (NodeType* t : {&chain, &joint})
        types::registerType(std::move(*t));
}

// Helpers ---------------------------------------------------------------------------------------------------------

glm::dmat4 worldOf(Graph& g, NodeId id) { return std::get<glm::dmat4>(g.value({id, Transform::kWorldMatrix, -1})); }
D3 vecOf(Graph& g, NodeId id, int attr) { return std::get<D3>(g.value({id, attr, -1})); }

std::vector<NodeId> jointsBetween(Graph& g, NodeId start, NodeId end)
{
    std::vector<NodeId> out;
    for (NodeId cur = end; cur; cur = g.parent(cur))
    {
        out.push_back(cur);
        if (cur == start)
        {
            std::reverse(out.begin(), out.end());
            return out;
        }
    }
    return {};
}

const NodeType& typeNamed(const char* name)
{
    const NodeType* t = types::find(name);
    if (!t)
        throw std::logic_error(std::string("spline chains: type ") + name + " is not registered");
    return *t;
}

/** The arc length of the polyline point nearest p. */
double arcNearest(const curve::Polyline& pl, const D3& p)
{
    double best = 1e300, at = 0.0;
    const size_t segs = pl.closed ? pl.size() : pl.size() - 1;
    for (size_t k = 0; k < segs; ++k)
    {
        const D3 a = pl.points[k], b = pl.points[(k + 1) % pl.size()], ab = b - a;
        const double len2 = glm::dot(ab, ab);
        const double f = len2 > 1e-24 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
        if (const double d = glm::length(p - (a + ab * f)); d < best)
        {
            best = d;
            const double s0 = pl.arcLength[k];
            const double s1 = k + 1 < pl.arcLength.size() ? pl.arcLength[k + 1] : pl.length;
            at = s0 + f * (s1 - s0);
        }
    }
    return at;
}

/** The rotation turning `world` least so its local `dir` points along `want`. */
glm::dmat3 aimed(const glm::dmat3& world, const D3& dir, const D3& want)
{
    const D3 now = glm::normalize(world * dir);
    const D3 to = glm::normalize(want);
    const D3 axis = glm::cross(now, to);
    const double s = glm::length(axis), c = glm::dot(now, to);
    if (s < 1e-12)
        return world;
    return glm::dmat3(glm::rotate(glm::dmat4(1.0), std::atan2(s, c), axis / s)) * world;
}

int dominantAxis(const D3& v)
{
    const D3 a = glm::abs(v);
    return a.x >= a.y && a.x >= a.z ? 0 : a.y >= a.z ? 1 : 2;
}

/** The joint's world axis most across t, as a direction in parent space. */
D3 crossAxis(const glm::dmat3& jointWorld, const glm::dmat3& parentRot, const D3& t)
{
    D3 best(0, 1, 0);
    double least = 2.0;
    for (int k = 0; k < 3; ++k)
    {
        const D3 a = glm::normalize(jointWorld[k]);
        if (const double d = std::abs(glm::dot(a, glm::normalize(t))); d < least)
            least = d, best = a;
    }
    return glm::transpose(parentRot) * best;
}

PlugRef plugOf(NodeId id, int attr) { return {id, attr, -1}; }
} // namespace

// Finding ---------------------------------------------------------------------------------------------------------

std::vector<NodeId> splineChainJoints(Graph& g, NodeId chain)
{
    const Node* c = g.node(chain);
    if (!c || !c->type().isA(kChainType))
        return {};
    const Node* s = g.find(c->as<std::string>(SplineChainNode::kStartJoint));
    const Node* e = g.find(c->as<std::string>(SplineChainNode::kEndJoint));
    return s && e ? jointsBetween(g, s->id(), e->id()) : std::vector<NodeId>{};
}

NodeId splineChainContaining(Graph& g, NodeId joint)
{
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n && n->type().isA(kChainType))
        {
            const std::vector<NodeId> js = splineChainJoints(g, id);
            if (std::find(js.begin(), js.end(), joint) != js.end())
                return id;
        }
    return kNoNode;
}

// Dynamics --------------------------------------------------------------------------------------------------------

Registry<DynamicsProvider>& dynamicsProviders()
{
    static Registry<DynamicsProvider> r;
    return r;
}

NodeId dynamicsOf(Graph& g, NodeId chain)
{
    const Node* c = g.node(chain);
    if (!c || !c->type().isA(kChainType))
        return kNoNode;
    const std::string& path = c->as<std::string>(SplineChainNode::kDynamics);
    try
    {
        const Node* n = path.empty() ? nullptr : g.find(path);
        return n ? n->id() : kNoNode;
    }
    catch (const std::exception&)
    {
        return kNoNode;
    }
}

NodeId addDynamics(Graph& g, NodeId chain, const std::string& mode, const DynamicsOptions& o)
{
    const Node* c = g.node(chain);
    if (!c || !c->type().isA(kChainType))
        throw std::invalid_argument("dynamics go on a spline chain");
    const DynamicsProvider* p = dynamicsProviders().find(mode);
    if (!p)
    {
        std::string all;
        for (const std::string& id : dynamicsProviders().ids())
            all += (all.empty() ? "" : " | ") + id;
        throw std::invalid_argument("no '" + mode + "' dynamics; " +
                                    (all.empty() ? std::string("load the Overlap or Physics Animation extension") : "the modes are " + all));
    }
    if (const NodeId d = dynamicsOf(g, chain))
        throw std::invalid_argument(c->path() + " already has dynamics, " + g.node(d)->path() + "; delete it first");
    const std::vector<NodeId> joints = splineChainJoints(g, chain);
    if (joints.size() < 2)
        throw std::invalid_argument(c->path() + " has lost its joints");
    if (o.carrier && !(g.node(o.carrier) && g.node(o.carrier)->type().isA("transform")))
        throw std::invalid_argument("the carrier is not a transform");
    g.evaluate();
    const NodeId made = p->attach(g, chain, joints, o);
    if (!g.node(made))
        throw std::invalid_argument("the " + mode + " dynamics made nothing");
    g.setValue({chain, SplineChainNode::kDynamics, -1}, g.node(made)->path());
    g.evaluate();
    return made;
}

NodeId splineJointOf(Graph& g, NodeId joint)
{
    const Node* j = g.node(joint);
    if (!j || !rig::isJoint(*j))
        return kNoNode;
    const auto src = g.source({joint, Joint::kIkBlend, -1});
    const Node* sj = src ? g.node(src->node) : nullptr;
    return sj && sj->type().isA(kJointType) ? sj->id() : kNoNode;
}

std::optional<OffsetKey> offsetFor(Graph& g, NodeId joint, const glm::dmat3& local)
{
    const NodeId sj = splineJointOf(g, joint);
    if (!sj)
        return std::nullopt;
    const double w = std::clamp(std::get<double>(g.value({joint, Joint::kIkBlend, -1})), 0.0, 1.0);
    if (w < 1e-6)
        return std::nullopt;
    const auto& j = static_cast<const Joint&>(*g.node(joint));
    const auto order = RotateOrder(std::get<int64_t>(g.value({joint, Transform::kRotateOrder, -1})));
    const glm::dmat3 want = glm::transpose(glm::dmat3(j.preRotation())) * local * glm::transpose(glm::dmat3(j.rotateAxisMatrix()));
    glm::dmat3 solved = want;
    if (w < 1.0)
    {
        // Joint::rotation slerps from the FK rotate toward solvedRotate by w; this is the solvedRotate that lands on want.
        const glm::dquat fk = glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(std::get<D3>(g.value({joint, Transform::kRotate, -1})), order))));
        glm::dquat d = glm::normalize(glm::inverse(fk) * glm::quat_cast(want));
        if (d.w < 0.0)
            d = -d;
        const D3 v(d.x, d.y, d.z);
        const double s = glm::length(v);
        const double angle = 2.0 * std::atan2(s, d.w) / w;
        const glm::dquat step = s > 1e-12 ? glm::angleAxis(angle, v / s) : glm::dquat(1.0, 0.0, 0.0, 0.0);
        solved = glm::mat3_cast(glm::normalize(fk * step));
    }
    const auto offsetOrder = RotateOrder(std::clamp<int64_t>(std::get<int64_t>(g.value({sj, SplineJointNode::kRotateOrder, -1})), 0, 5));
    const D3 now = std::get<D3>(g.value({sj, SplineJointNode::kOffset, -1}));
    const glm::dmat3 solvedNow(eulerToMatrix(std::get<D3>(g.value({joint, Joint::kSolvedRotate, -1})), order));
    const glm::dmat3 offset = glm::dmat3(eulerToMatrix(now, offsetOrder)) * glm::transpose(solvedNow) * solved;
    return OffsetKey{{sj, SplineJointNode::kOffset, -1}, eulerNear(offset, offsetOrder, now)};
}

// Creating --------------------------------------------------------------------------------------------------------

NodeId createSplineChain(Graph& g, NodeId start, NodeId end, NodeId curveId, const ChainOptions& opts)
{
    LILA_ZONE();

    if (!g.node(start) || !g.node(end) || !rig::isJoint(*g.node(start)) || !rig::isJoint(*g.node(end)))
        throw std::invalid_argument("a spline chain runs between joints");
    const std::vector<NodeId> joints = jointsBetween(g, start, end);
    if (joints.size() < 2)
        throw std::invalid_argument(g.node(end)->path() + " is not below " + g.node(start)->path());
    const Node* curveNode = g.node(curveId);
    if (!curveNode || !curveNode->type().isA("curve"))
        throw std::invalid_argument("a spline chain needs a curve");
    for (NodeId j : joints)
        if (g.source({j, Joint::kSolvedRotate, -1}))
            throw std::invalid_argument(g.node(j)->path() + " is already driven");
    if (opts.upMode == UpMode::UpObjects && (!opts.upStart || !opts.upEnd))
        throw std::invalid_argument("upObjects needs an up object at each end");

    g.evaluate();
    const curve::Splines& splines = curve::splinesOf(g.value({curveId, curve::CurveShape::kWorldSpace, -1}));
    if (splines.empty())
        throw std::invalid_argument(curveNode->path() + " has no curve");
    const NodeId parent = g.parent(start);
    const glm::dmat4 parentWorld = parent ? worldOf(g, parent) : glm::dmat4(1.0);
    const glm::dmat3 parentRot = rotationOf(parentWorld);

    const size_t n = joints.size();
    std::vector<glm::dmat3> worlds;
    std::vector<D3> pos;
    for (NodeId j : joints)
    {
        const glm::dmat4 w = worldOf(g, j);
        worlds.push_back(rotationOf(w));
        pos.push_back(D3(w[3]));
    }

    D3 upLocal(0, 1, 0);
    {
        const curve::Polyline pl = curve::tessellate(splines.front(), curve::RailOptions{}.tess);
        upLocal = opts.upVector ? *opts.upVector : crossAxis(worlds[0], parentRot, pl.size() > 1 ? pl.tangent(0) : D3(1, 0, 0));
    }
    const curve::Rail rail(splines.front(), SplineChainNode::railOptions(opts.upMode, parentRot * upLocal));
    if (!rail.valid())
        throw std::invalid_argument(curveNode->path() + " has no length");

    solve::Rest rest;
    rest.curveLength = rail.length();
    {
        const curve::Polyline pl = curve::tessellate(splines.front(), curve::RailOptions{}.tess);
        for (const D3& p : pos)
            rest.s.push_back(arcNearest(pl, p));
    }
    for (size_t i = 1; i < n; ++i)
        if (rest.s[i] < rest.s[i - 1] + 1e-6)
            throw std::invalid_argument("the joints run back along " + curveNode->path() + "; give a curve from " + g.node(start)->path() +
                                        " to " + g.node(end)->path());
    rest.chord.assign(n, 0.0);
    rest.bone.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i)
    {
        const D3 p = rail.at(rest.s[i]).p;
        if (i > 0)
        {
            rest.chord[i] = glm::length(p - rail.at(rest.s[i - 1]).p);
            rest.bone[i] = glm::length(vecOf(g, joints[i], Transform::kTranslate));
        }
    }
    rest.offset.assign(n, glm::dmat3(1.0));
    solve::Params p;
    p.mode = LengthMode::Stretch;
    p.minStretch = 0.0;
    p.maxStretch = 1e9;
    const solve::Solved frames = solve::solve(rail, p, rest);

    std::vector<int64_t> scaleAxis(n, 0);
    for (size_t i = 0; i < n; ++i)
    {
        glm::dmat3 w = worlds[i];
        D3 bone(1, 0, 0);
        if (i + 1 < n)
        {
            bone = vecOf(g, joints[i + 1], Transform::kTranslate);
            if (glm::length(bone) > 1e-9)
                w = aimed(w, glm::normalize(bone), frames.p[i + 1] - frames.p[i]);
        }
        else if (const D3 in = vecOf(g, joints[i], Transform::kTranslate); glm::length(in) > 1e-9)
            bone = glm::transpose(worlds[i]) * worlds[i - 1] * in;
        scaleAxis[i] = dominantAxis(bone);
        rest.offset[i] = glm::transpose(frames.frame[i]) * w;
    }

    const std::string base = opts.name.empty() ? g.node(start)->name() + "_spline" : opts.name;
    Node* c = g.createNode(typeNamed(kChainType), base);
    const NodeId id = c->id();
    auto set = [&](int a, PlugValue v) { g.setValue({id, a, -1}, std::move(v)); };
    set(SplineChainNode::kCurveName, curveNode->path());
    set(SplineChainNode::kStartJoint, g.node(start)->path());
    set(SplineChainNode::kEndJoint, g.node(end)->path());
    set(SplineChainNode::kLengthMode, int64_t(opts.lengthMode));
    set(SplineChainNode::kMinStretch, opts.minStretch);
    set(SplineChainNode::kMaxStretch, opts.maxStretch);
    set(SplineChainNode::kUpMode, int64_t(opts.upMode));
    set(SplineChainNode::kUpVector, upLocal);
    set(SplineChainNode::kRestCurveLength, rest.curveLength);
    set(SplineChainNode::kJointCount, int64_t(n));
    std::vector<PlugValue> s, chord, bone, offset, axis;
    for (size_t i = 0; i < n; ++i)
    {
        s.push_back(rest.s[i]);
        chord.push_back(rest.chord[i]);
        bone.push_back(rest.bone[i]);
        offset.push_back(glm::dmat4(rest.offset[i]));
        axis.push_back(scaleAxis[i]);
    }
    g.setElements(plugOf(id, SplineChainNode::kRestLength), s);
    g.setElements(plugOf(id, SplineChainNode::kRestChord), chord);
    g.setElements(plugOf(id, SplineChainNode::kBoneLength), bone);
    g.setElements(plugOf(id, SplineChainNode::kRestOffset), offset);
    g.setElements(plugOf(id, SplineChainNode::kScaleAxis), axis);
    if (opts.upMode == UpMode::UpObjects)
    {
        // The objects' axes that lie along the rest frames' up, so the rest pose has no twist from them.
        const glm::dmat3 a = rotationOf(worldOf(g, opts.upStart)), b = rotationOf(worldOf(g, opts.upEnd));
        set(SplineChainNode::kUpStartAxis, glm::transpose(a) * frames.frame.front()[1]);
        set(SplineChainNode::kUpEndAxis, glm::transpose(b) * frames.frame.back()[1]);
        g.connect({opts.upStart, Transform::kWorldMatrix, -1}, plugOf(id, SplineChainNode::kUpStartMatrix), true);
        g.connect({opts.upEnd, Transform::kWorldMatrix, -1}, plugOf(id, SplineChainNode::kUpEndMatrix), true);
    }
    g.connect({curveId, curve::CurveShape::kWorldSpace, -1}, plugOf(id, SplineChainNode::kInCurve), true);
    if (parent)
        g.connect({parent, Transform::kWorldMatrix, -1}, plugOf(id, SplineChainNode::kParentMatrix), true);

    for (size_t i = 0; i < n; ++i)
    {
        const NodeId j = joints[i];
        Node* sj = g.createNode(typeNamed(kJointType), g.node(j)->name() + "_splineJoint");
        const NodeId sid = sj->id();
        g.setValue({sid, SplineJointNode::kIndex, -1}, int64_t(i));
        g.setValue({sid, SplineJointNode::kJointOrient, -1}, vecOf(g, j, Joint::kJointOrient));
        g.setValue({sid, SplineJointNode::kRotateAxis, -1}, vecOf(g, j, Transform::kRotateAxis));
        g.setValue({sid, SplineJointNode::kRotateOrder, -1}, std::get<int64_t>(g.value({j, Transform::kRotateOrder, -1})));
        g.setValue({sid, SplineJointNode::kRestTranslate, -1}, vecOf(g, j, Transform::kTranslate));
        g.connect(plugOf(id, SplineChainNode::kSolution), plugOf(sid, SplineJointNode::kSolution), true);
        g.connect(plugOf(sid, SplineJointNode::kSolvedRotate), plugOf(j, Joint::kSolvedRotate), true);
        g.connect(plugOf(sid, SplineJointNode::kIkBlend), plugOf(j, Joint::kIkBlend), true);
        g.connect(plugOf(sid, SplineJointNode::kScale), plugOf(j, Transform::kScale), true);
        if (i == 0)
            g.connect(plugOf(sid, SplineJointNode::kTranslate), plugOf(j, Transform::kTranslate), true);
        else
            g.connect(plugOf(sid, SplineJointNode::kStretch), plugOf(j, Joint::kIkStretch), true);
        // Squash scale stays on its joint: the joints below undo it, so nothing shears and the bake can keep it on a leaf.
        for (NodeId child : g.children(j))
            if (rig::isJoint(*g.node(child)))
            {
                g.setValue({child, Joint::kSegmentScaleCompensate, -1}, true);
                g.connect(plugOf(j, Transform::kScale), plugOf(child, Joint::kInverseScale), true);
            }
    }
    g.evaluate();
    return id;
}

// Commands --------------------------------------------------------------------------------------------------------

namespace
{
using rigutil::graph;
using rigutil::nodeNamed;
using rigutil::vec3Of;

int64_t indexIn(const std::vector<std::string>& names, const std::string& v, const char* cmd, const char* what)
{
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == v)
            return int64_t(i);
    std::string all;
    for (const std::string& n : names)
        all += (all.empty() ? "" : " | ") + n;
    throw CommandError(std::string(cmd) + ": " + what + " is " + all);
}

template <class F> auto guard(const char* cmd, F&& f)
{
    try
    {
        return f();
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

/** A Bezier through count points spread evenly along the joints. */
std::string curveThroughJoints(Graph& g, const std::vector<NodeId>& joints, int count, const std::string& name)
{
    std::vector<D3> pts;
    std::vector<double> at{0.0};
    for (NodeId j : joints)
        pts.push_back(D3(worldOf(g, j)[3]));
    for (size_t i = 1; i < pts.size(); ++i)
        at.push_back(at.back() + glm::length(pts[i] - pts[i - 1]));
    nlohmann::json points = nlohmann::json::array();
    for (int k = 0; k < count; ++k)
    {
        const double s = at.back() * double(k) / double(count - 1);
        size_t i = 1;
        while (i + 1 < at.size() && at[i] < s)
            ++i;
        const double span = at[i] - at[i - 1];
        const D3 p = glm::mix(pts[i - 1], pts[i], span > 1e-12 ? std::clamp((s - at[i - 1]) / span, 0.0, 1.0) : 0.0);
        points.push_back(rigutil::toJson3(p));
    }
    return ops::execute("curve", {{"type", "bezier"}, {"points", points}, {"handleType", "auto"}, {"name", name}}).get<std::string>();
}

json splineChainCmd(const Args& a)
{
    Graph& g = graph();
    std::vector<NodeId> joints;
    NodeId curveId = kNoNode;
    std::vector<std::string> objs = a.objects();
    if (objs.empty())
        for (NodeId id : g.selection())
            objs.push_back(g.node(id)->path());
    for (const std::string& o : objs)
    {
        Node& n = nodeNamed(o, "splineChain");
        if (n.type().isA("curve"))
            curveId = n.id();
        else if (rig::isJoint(n))
            joints.push_back(n.id());
        else
            throw CommandError("splineChain: " + n.name() + " is neither a joint nor a curve");
    }
    if (a.has("curve"))
        curveId = nodeNamed(a.get<std::string>("curve"), "splineChain").id();
    if (joints.size() < 2)
        throw CommandError("splineChain: give the start joint and the end joint");
    const NodeId start = joints.front(), end = joints.back();
    const std::vector<NodeId> chain = jointsBetween(g, start, end);
    if (chain.size() < 2)
        throw CommandError("splineChain: " + g.node(end)->path() + " is not below " + g.node(start)->path());

    ChainOptions o;
    o.name = a.get<std::string>("name", "");
    o.lengthMode = LengthMode(indexIn(kLengthModes, a.get<std::string>("lengthMode", "stretch"), "splineChain", "lengthMode"));
    o.upMode = UpMode(indexIn(kUpModes, a.get<std::string>("upMode", "curveFrames"), "splineChain", "upMode"));
    if (a.has("upVector"))
        o.upVector = vec3Of(a.flags["upVector"]);
    if (a.has("upStart"))
        o.upStart = nodeNamed(a.get<std::string>("upStart"), "splineChain").id();
    if (a.has("upEnd"))
        o.upEnd = nodeNamed(a.get<std::string>("upEnd"), "splineChain").id();
    o.minStretch = a.get<double>("minStretch", 0.25);
    o.maxStretch = a.get<double>("maxStretch", 4.0);

    g.evaluate();
    if (!curveId)
    {
        const int count = std::max(2, a.get<int>("controls", 4));
        const std::string base = o.name.empty() ? g.node(start)->name() + "_spline" : o.name;
        const std::string made = curveThroughJoints(g, chain, count, base + "Curve");
        curveId = nodeNamed(made, "splineChain").id();
        if (a.get<int>("controls", 4) > 0)
            ops::execute("curveControls", {{"args", {made}}});
    }
    const NodeId c = guard("splineChain", [&] { return createSplineChain(g, start, end, curveId, o); });
    g.select({c});
    return g.node(c)->path();
}

/** Chains from the command's objects: chains, splineJoints, or joints in them. Default the selection. */
std::vector<NodeId> chainArgs(Graph& g, const Args& a, const char* cmd)
{
    std::vector<std::string> objs = a.objects();
    if (objs.empty())
        for (NodeId id : g.selection())
            objs.push_back(g.node(id)->path());
    std::vector<NodeId> out;
    for (const std::string& o : objs)
    {
        Node& n = nodeNamed(o, cmd);
        NodeId c = kNoNode;
        if (n.type().isA(kChainType))
            c = n.id();
        else if (n.type().isA(kJointType))
        {
            if (auto src = g.source({n.id(), SplineJointNode::kSolution, -1}))
                c = src->node;
        }
        else if (n.type().isA(kHoseType))
            c = hoseChain(g, n.id());
        else if (rig::isJoint(n))
            c = splineChainContaining(g, n.id());
        if (c && std::find(out.begin(), out.end(), c) == out.end())
            out.push_back(c);
    }
    if (out.empty())
        throw CommandError(std::string(cmd) + ": give spline chains or joints in them");
    return out;
}

json editSplineChainCmd(const Args& a)
{
    Graph& g = graph();
    json out = json::array();
    for (NodeId c : chainArgs(g, a, "editSplineChain"))
    {
        auto setNum = [&](const char* flag, int attr) {
            if (a.has(flag))
                g.setValue({c, attr, -1}, a.get<double>(flag));
        };
        setNum("weight", SplineChainNode::kWeight);
        setNum("minStretch", SplineChainNode::kMinStretch);
        setNum("maxStretch", SplineChainNode::kMaxStretch);
        setNum("volume", SplineChainNode::kVolume);
        setNum("slide", SplineChainNode::kSlide);
        setNum("twistStart", SplineChainNode::kTwistStart);
        setNum("twistEnd", SplineChainNode::kTwistEnd);
        setNum("roll", SplineChainNode::kRoll);
        setNum("twistSpread", SplineChainNode::kTwistSpread);
        if (a.has("lengthMode"))
            g.setValue({c, SplineChainNode::kLengthMode, -1}, indexIn(kLengthModes, a.get<std::string>("lengthMode"), "editSplineChain", "lengthMode"));
        if (a.has("twistRamp"))
            g.setValue({c, SplineChainNode::kTwistRamp, -1}, indexIn(kRamps, a.get<std::string>("twistRamp"), "editSplineChain", "twistRamp"));
        if (a.has("upMode"))
        {
            const int64_t m = indexIn(kUpModes, a.get<std::string>("upMode"), "editSplineChain", "upMode");
            if (m == int64_t(UpMode::UpObjects) && !g.source({c, SplineChainNode::kUpStartMatrix, -1}))
                throw CommandError("editSplineChain: " + g.node(c)->name() + " was made without up objects");
            g.setValue({c, SplineChainNode::kUpMode, -1}, m);
        }
        if (a.has("upVector"))
            g.setValue({c, SplineChainNode::kUpVector, -1}, vec3Of(a.flags["upVector"]));
        out.push_back(g.node(c)->path());
    }
    return out;
}

json splineDynamicsCmd(const Args& a)
{
    Graph& g = graph();
    DynamicsOptions o;
    o.full = a.get<std::string>("drive", "additive") == "full";
    auto opt = [&](const char* flag, std::optional<double>& into) {
        if (a.has(flag))
            into = a.get<double>(flag);
    };
    opt("stiffness", o.stiffness);
    opt("damping", o.damping);
    opt("gravity", o.gravity);
    opt("radius", o.radius);
    if (a.has("carrier"))
        o.carrier = nodeNamed(a.get<std::string>("carrier"), "splineDynamics").id();
    o.carrierInertia = a.get<double>("carrierInertia", 1.0);
    if (a.has("colliders"))
        for (const json& c : a.flags["colliders"])
            o.colliders.push_back(nodeNamed(c.get<std::string>(), "splineDynamics").id());
    o.name = a.get<std::string>("name", "");
    const std::string mode = a.get<std::string>("mode", "overlap");
    json out = json::array();
    for (NodeId c : chainArgs(g, a, "splineDynamics"))
        out.push_back(g.node(guard("splineDynamics", [&] { return addDynamics(g, c, mode, o); }))->path());
    return out;
}

void registerOptionBoxes()
{
    using ops::options::Override;
    Override c;
    c.order = {"lengthMode", "minStretch", "maxStretch", "controls", "upMode", "upVector", "upStart", "upEnd", "curve", "name"};
    c.hidden = {"name", "curve", "upStart", "upEnd"};
    c.labels = {{"lengthMode", "Length"}, {"minStretch", "Min Stretch"}, {"maxStretch", "Max Stretch"}, {"upMode", "Up"}};
    c.choices = {{"lengthMode", {{"keepLength", "Keep Length (slide)"}, {"stretch", "Stretch"}, {"squash", "Stretch and Squash"}}},
                 {"upMode", {{"curveFrames", "Curve Frames"}, {"upVector", "Up Vector"}, {"upObjects", "Up Objects"}}}};
    c.ranges = {{"minStretch", {0.0, 1.0, 0.05, ""}}, {"maxStretch", {1.0, 10.0, 0.1, ""}}, {"controls", {0.0, 12.0, 1.0, ""}}};
    ops::options::setOverride("splineChain", std::move(c));
}
} // namespace

void registerHoseTypes();
void registerHoseCommands();

void registerTypes()
{
    if (types::find(kChainType))
        return;
    registerTypesImpl();
    registerHoseTypes();
}

void registerCommands()
{
    if (ops::find("splineChain"))
        return;
    using T = ArgType;
    ops::registerCommand({
        .name = "splineChain",
        .doc = "Lay a run of joints along a curve: the joints from the first given to the last ride the curve by arc length, keeping "
               "their length, stretching, or stretching with a volume-keeping squash. Without a curve one is made through the joints "
               "with controls on it. Returns the splineChain.",
        .positionalDoc = "start joint, end joint, and optionally the curve (default the selection)",
        .flags = {
            nodeFlag({"curve", "c", T::String, nullptr, "The curve the joints ride; default a new one through them."}, "curve"),
            {"controls", "ct", T::Int, 4, "With no curve: points on the new curve, each with a control; 0 makes a curve of 2 points and no controls.",
             {}, {}, 0.0},
            {"lengthMode", "lm", T::String, "stretch", "keepLength | stretch | squash.", {"keepLength", "stretch", "squash"}},
            {"minStretch", "mns", T::Float, 0.25, "The shortest the chain gets, as a multiple of its length.", {}, {}, 0.0},
            {"maxStretch", "mxs", T::Float, 4.0, "The longest the chain gets, as a multiple of its length.", {}, {}, 0.0},
            {"upMode", "um", T::String, "curveFrames", "curveFrames | upVector | upObjects.", {"curveFrames", "upVector", "upObjects"}},
            {"upVector", "upv", T::Vec3, nullptr, "Up in the start joint's parent's space; default the start joint's axis most across the curve."},
            nodeFlag({"upStart", "ups", T::String, nullptr, "upObjects: the object whose turn the start follows."}, "transform"),
            nodeFlag({"upEnd", "upe", T::String, nullptr, "upObjects: the object whose turn the end follows."}, "transform"),
            {"name", "n", T::String, nullptr, "Name for the chain; default <start>_spline."},
        },
        .run = splineChainCmd,
        .examples = {"splineChain(\"spine_01\", \"neck_01\", lengthMode=\"squash\")", "splineChain(\"tail_01\", \"tail_12\", \"TailCurve\")"},
    });
    ops::registerCommand({
        .name = "editSplineChain",
        .doc = "Change spline chains: give the chains, joints in them or their splineJoints. Returns the chains.",
        .positionalDoc = "spline chains (default the selection)",
        .flags = {
            {"weight", "w", T::Float, nullptr, "0 leaves the joints on their rotate, 1 lays them on the curve.", {}, {}, 0.0, 1.0},
            {"lengthMode", "lm", T::String, nullptr, "keepLength | stretch | squash.", {"keepLength", "stretch", "squash"}},
            {"minStretch", "mns", T::Float, nullptr, "The shortest the chain gets, as a multiple of its length.", {}, {}, 0.0},
            {"maxStretch", "mxs", T::Float, nullptr, "The longest the chain gets.", {}, {}, 0.0},
            {"volume", "v", T::Float, nullptr, "Squash: 1 keeps the volume, 0 the thickness.", {}, {}, 0.0},
            {"slide", "sl", T::Float, nullptr, "Moves the chain along the curve, cm."},
            {"twistStart", "ts", T::Float, nullptr, "Twist at the first joint, degrees."},
            {"twistEnd", "te", T::Float, nullptr, "Twist at the last joint, degrees."},
            {"twistRamp", "tr", T::String, nullptr, "linear | easeIn | easeOut | smooth.", {"linear", "easeIn", "easeOut", "smooth"}},
            {"roll", "r", T::Float, nullptr, "Turn of the whole chain about the curve, degrees."},
            {"upMode", "um", T::String, nullptr, "curveFrames | upVector | upObjects.", {"curveFrames", "upVector", "upObjects"}},
            {"upVector", "upv", T::Vec3, nullptr, "Up in the start joint's parent's space."},
            {"twistSpread", "tsp", T::Float, nullptr, "upObjects: share of the end object's twist spread along the chain.", {}, {}, 0.0, 1.0},
        },
        .run = editSplineChainCmd,
        .examples = {"editSplineChain(\"spine_01_spline\", lengthMode=\"squash\", twistEnd=45)"},
    });
    ops::registerCommand({
        .name = "splineDynamics",
        .doc = "Dynamics on spline chains (tails, tentacles, antennae, hose limbs): overlap is stylised, cheap and loops; jolt is a "
               "rigid body per segment that collides with the scene. Additive follows the animation with follow-through; full "
               "simulates with only the root animated. Record it with recordSimulation: the motion keys the splineJoints' "
               "offsets on an additive layer, which bakeToDeformSkeleton carries to plain keys. Returns the simulation nodes.",
        .positionalDoc = "spline chains, joints in them, their splineJoints or hoseLimbs (default the selection)",
        .flags = {
            {"mode", "m", T::String, "overlap", "overlap | jolt.", {"overlap", "jolt"}},
            {"drive", "dr", T::String, "additive", "additive: follow-through on the animation. full: simulated from the root.", {"additive", "full"}},
            {"stiffness", "st", T::Float, nullptr, "Pull back to the animation: overlap rotation stiffness, jolt pose match Hz.", {}, {}, 0.0},
            {"damping", "d", T::Float, nullptr, "overlap: damping. jolt: damping ratio, 1 settles without bouncing.", {}, {}, 0.0},
            {"gravity", "g", T::Float, nullptr, "Share of gravity the chain feels; default 0 additive, 1 full."},
            {"radius", "r", T::Float, nullptr, "Collision radius of each segment, cm; default from the bone length.", {}, {}, 0.0},
            nodeFlag({"colliders", "co", T::List, nullptr, "jolt: meshes the chain collides with. overlap: overlap colliders (createOverlapCollider)."}, "mesh"),
            nodeFlag({"carrier", "ca", T::String, nullptr, "Carried motion: the transform (saddle, seat) the chain simulates relative to."}, "transform"),
            {"carrierInertia", "ci", T::Float, 1.0, "Share of the carrier's acceleration the chain feels: 1 like the world, 0 carried rigidly.",
             {}, {}, 0.0},
            {"name", "n", T::String, nullptr, "Name of the simulation node."},
        },
        .run = splineDynamicsCmd,
        .examples = {"splineDynamics(\"tail_01_spline\")", "splineDynamics(\"tail_01\", mode=\"jolt\", colliders=[\"Ground\"])",
                     "splineDynamics(\"tail_01_spline\", carrier=\"horse_saddle\", carrierInertia=0.5)"},
    });
    registerOptionBoxes();
    registerHoseCommands();
}
} // namespace lila::scene::spline
