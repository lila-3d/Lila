#include "scene/Drivers.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>

namespace lila::scene
{
using namespace rigutil;

namespace
{
using drivers::kChannelOutput;
using drivers::kCurveName;
using drivers::kInMatrix;
using drivers::kInput;
using drivers::kInWeight;
using drivers::kKeys;
using drivers::kOutput;
using drivers::kPostInfinity;
using drivers::kPreInfinity;
using drivers::kRadius;
using drivers::kRestMatrix;
using drivers::kTargetMatrix;
using drivers::kTwistAxis;
using drivers::kTwistWeight;

double wrapDegrees(double d)
{
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0.0)
        d += 360.0;
    return d - 180.0;
}

glm::dvec3 axisVec(int axis)
{
    glm::dvec3 a(0.0);
    a[std::clamp(axis, 0, 2)] = 1.0;
    return a;
}

/** Signed twist about axis of the rotation, degrees, from its swing-twist split. */
double twistAngle(const glm::dmat3& r, const glm::dvec3& axis)
{
    glm::dquat q = glm::quat_cast(r);
    return wrapDegrees(glm::degrees(2.0 * std::atan2(glm::dot(glm::dvec3(q.x, q.y, q.z), axis), q.w)));
}

class PoseDriverNode : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const glm::dmat3 rest = orthonormal(glm::dmat3(as<glm::dmat4>(kRestMatrix)));
        const glm::dmat3 cur = orthonormal(glm::dmat3(as<glm::dmat4>(kInMatrix)));
        const glm::dmat3 target = orthonormal(glm::dmat3(as<glm::dmat4>(kTargetMatrix)));
        drivers::PoseReading r = drivers::readPose(rest, cur, target, int(as<int64_t>(kTwistAxis)), as<double>(kTwistWeight));
        const auto f = drivers::Falloff(std::clamp<int64_t>(as<int64_t>(drivers::kFalloff), 0, 2));
        setOutput(kOutput, drivers::falloffWeight(r.distance, as<double>(kRadius), f) * as<double>(kInWeight));
    }
};

class DrivenChannelNode : public Node
{
public:
    void compute(const EvalContext&) override
    {
        const std::string& text = as<std::string>(kKeys);
        if (text != mText)
        {
            mText = text;
            try
            {
                mCurve.keys = text.empty() ? std::vector<anim::Key>{} : anim::Curve::keysFromJson(json::parse(text));
            }
            catch (const std::exception&)
            {
                mCurve.keys.clear();
            }
        }
        mCurve.pre = anim::Infinity(std::clamp<int64_t>(as<int64_t>(kPreInfinity), 0, 4));
        mCurve.post = anim::Infinity(std::clamp<int64_t>(as<int64_t>(kPostInfinity), 0, 4));
        const double in = as<double>(kInput);
        setOutput(kChannelOutput, mCurve.keys.empty() ? in : mCurve.evaluate(in));
    }

private:
    std::string mText;
    anim::Curve mCurve;
};

Graph& g() { return Graph::current(); }

PlugRef plugNamed(Graph& graph, const std::string& path, const char* cmd)
{
    try
    {
        return graph.plug(path);
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

bool scalarPlug(Graph& graph, const PlugRef& p)
{
    if (p.component >= 0)
        return true;
    PlugType t = graph.attr(p).type;
    return t == PlugType::Float;
}

double currentValue(Graph& graph, const PlugRef& p)
{
    PlugValue v = graph.value(p);
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    return 0.0;
}

anim::Tangent tangentNamed(const std::string& s, const char* cmd)
{
    try
    {
        return anim::tangentFromName(s);
    }
    catch (const std::invalid_argument&)
    {
        throw CommandError(std::string(cmd) + ": unknown interpolation '" + s + "' (linear, spline, flat, step, auto, clamped)");
    }
}

/** [[in, out], [in, out, "linear"], ...] into keys; tangent applies where a key names none. */
std::vector<anim::Key> keysFrom(const json& j, anim::Tangent tangent, const char* cmd)
{
    std::vector<anim::Key> keys;
    if (!j.is_array())
        throw CommandError(std::string(cmd) + ": keys must be a list of [driver value, driven value]");
    for (const json& k : j)
    {
        if (!k.is_array() || k.size() < 2)
            throw CommandError(std::string(cmd) + ": each key is [driver value, driven value] or [driver, driven, interpolation]");
        anim::Key key;
        key.time = k[0].get<double>();
        key.value = k[1].get<double>();
        key.in = key.out = k.size() > 2 ? tangentNamed(k[2].get<std::string>(), cmd) : tangent;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), [](const anim::Key& a, const anim::Key& b) { return a.time < b.time; });
    return keys;
}

json keysJson(const anim::Curve& c)
{
    json out = json::array();
    for (const anim::Key& k : c.keys)
        out.push_back({k.time, k.value, anim::tangentName(k.out)});
    return out;
}

Node& channelNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!drivers::isChannel(n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a drivenChannel");
    return n;
}

/** A material scalar's curve name as Unreal's material curves expect it: the parameter LilaLink names, suffixed _mat. */
std::string defaultCurveName(Graph& graph, const PlugRef& driven)
{
    Node* n = graph.node(driven.node);
    if (!n || !n->type().isA("material"))
        return {};
    std::string attr = driven.component >= 0 ? graph.attr(driven).childNames[size_t(driven.component)] : graph.attr(driven).name;
    if (!attr.empty())
        attr[0] = char(std::toupper(static_cast<unsigned char>(attr[0])));
    return attr + "_mat";
}

json channelInfo(Graph& graph, NodeId id)
{
    Node* n = graph.node(id);
    auto src = graph.source({id, kInput, -1});
    std::vector<PlugRef> dsts = graph.destinations({id, kChannelOutput, -1});
    json driven = json::array();
    for (const PlugRef& d : dsts)
        driven.push_back(graph.plugName(d));
    anim::Curve c = drivers::channelCurve(*n);
    return {{"node", n->name()},
            {"driver", src ? json(graph.plugName(*src)) : json(nullptr)},
            {"driven", driven},
            {"keys", keysJson(c)},
            {"preInfinity", anim::infinityName(c.pre)},
            {"postInfinity", anim::infinityName(c.post)},
            {"curveName", n->as<std::string>(kCurveName)},
            {"input", std::get<double>(graph.value({id, kInput, -1}))},
            {"output", std::get<double>(graph.value({id, kChannelOutput, -1}))}};
}

json quatJson(const glm::dmat3& m)
{
    glm::dquat q = glm::quat_cast(m);
    return json::array({q.x, q.y, q.z, q.w});
}

json poseInfo(Graph& graph, NodeId id)
{
    Node* n = graph.node(id);
    auto src = graph.source({id, kInMatrix, -1});
    auto weightSrc = graph.source({id, kInWeight, -1});
    json drives = json::array();
    for (const PlugRef& d : graph.destinations({id, kOutput, -1}))
        drives.push_back(graph.plugName(d));
    static constexpr const char* kAxes[] = {"x", "y", "z"};
    static constexpr const char* kFalloffs[] = {"linear", "smooth", "smoother"};
    return {{"node", n->name()},
            {"transform", src ? json(graph.node(src->node)->name()) : json(nullptr)},
            {"twistAxis", kAxes[std::clamp<int64_t>(n->as<int64_t>(kTwistAxis), 0, 2)]},
            {"radius", n->as<double>(kRadius)},
            {"twistWeight", n->as<double>(kTwistWeight)},
            {"falloff", kFalloffs[std::clamp<int64_t>(n->as<int64_t>(drivers::kFalloff), 0, 2)]},
            {"restRotation", quatJson(orthonormal(glm::dmat3(n->as<glm::dmat4>(kRestMatrix))))},
            {"targetRotation", quatJson(orthonormal(glm::dmat3(n->as<glm::dmat4>(kTargetMatrix))))},
            {"multipliedBy", weightSrc ? json(graph.plugName(*weightSrc)) : json(nullptr)},
            {"drives", drives},
            {"output", std::get<double>(graph.value({id, kOutput, -1}))}};
}
} // namespace

namespace drivers
{
PoseReading readPose(const glm::dmat3& rest, const glm::dmat3& current, const glm::dmat3& target, int twistAxis, double twistWeight)
{
    const glm::dvec3 axis = axisVec(twistAxis);
    const glm::dmat3 restT = glm::transpose(rest);
    const glm::dmat3 relCur = restT * current, relTarget = restT * target;
    const glm::dvec3 a = glm::normalize(relCur * axis), b = glm::normalize(relTarget * axis);
    PoseReading r;
    r.swing = glm::degrees(std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b)));
    r.twist = std::abs(wrapDegrees(twistAngle(relCur, axis) - twistAngle(relTarget, axis)));
    r.distance = r.swing + std::max(0.0, twistWeight) * r.twist;
    return r;
}

double falloffWeight(double distance, double radius, Falloff f)
{
    if (radius <= 0.0)
        return distance <= 1e-9 ? 1.0 : 0.0;
    const double x = std::clamp(1.0 - distance / radius, 0.0, 1.0);
    switch (f)
    {
    case Falloff::Linear:
        return x;
    case Falloff::Smooth:
        return x * x * (3.0 - 2.0 * x);
    case Falloff::Smoother:
        return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
    }
    return x;
}

bool isPoseDriver(const Node& n) { return n.type().isA("poseDriver"); }
bool isChannel(const Node& n) { return n.type().isA("drivenChannel"); }

std::vector<NodeId> poseDrivers(Graph& graph)
{
    std::vector<NodeId> out;
    for (NodeId id : graph.nodes())
        if (isPoseDriver(*graph.node(id)))
            out.push_back(id);
    return out;
}

std::vector<NodeId> channels(Graph& graph)
{
    std::vector<NodeId> out;
    for (NodeId id : graph.nodes())
        if (isChannel(*graph.node(id)))
            out.push_back(id);
    return out;
}

anim::Curve channelCurve(const Node& n)
{
    anim::Curve c;
    try
    {
        const std::string& text = n.as<std::string>(kKeys);
        if (!text.empty())
            c.keys = anim::Curve::keysFromJson(json::parse(text));
    }
    catch (const std::exception&)
    {
        c.keys.clear();
    }
    c.pre = anim::Infinity(std::clamp<int64_t>(n.as<int64_t>(kPreInfinity), 0, 4));
    c.post = anim::Infinity(std::clamp<int64_t>(n.as<int64_t>(kPostInfinity), 0, 4));
    return c;
}

void setChannelCurve(Graph& graph, NodeId id, const anim::Curve& c)
{
    anim::Curve sorted = c;
    sorted.sort();
    std::string text = sorted.keysToJson().dump();
    Node* n = graph.node(id);
    if (text != n->as<std::string>(kKeys))
        graph.setValue({id, kKeys, -1}, text);
    if (int64_t(c.pre) != n->as<int64_t>(kPreInfinity))
        graph.setValue({id, kPreInfinity, -1}, int64_t(c.pre));
    if (int64_t(c.post) != n->as<int64_t>(kPostInfinity))
        graph.setValue({id, kPostInfinity, -1}, int64_t(c.post));
}

NodeId createPoseDriver(Graph& graph, NodeId transform, const std::string& name, int twistAxis, double radius, double twistWeight, Falloff falloff,
                        const glm::dvec3& restRotate)
{
    Node* t = graph.node(transform);
    if (!t || !t->type().isA("transform"))
        throw CommandError("poseDriver: give a joint or transform to read");
    graph.evaluate();
    const auto order = RotateOrder(std::clamp<int64_t>(t->as<int64_t>(Transform::kRotateOrder), 0, 5));
    const glm::dmat4 rest = Transform::localMatrix(*t, eulerToMatrix(restRotate, order));
    const glm::dmat4 target = t->as<glm::dmat4>(Transform::kMatrix);
    const glm::dmat3 r3 = orthonormal(glm::dmat3(rest)), t3 = orthonormal(glm::dmat3(target));
    if (radius <= 0.0)
    {
        radius = readPose(r3, r3, t3, twistAxis, twistWeight).distance;
        if (radius < 1.0)
            radius = 45.0;
    }
    Node* d = guarded([&] { return graph.createNode(*types::find("poseDriver"), name.empty() ? t->name() + "PoseDriver" : name, kNoNode, true); });
    graph.setValue({d->id(), kRestMatrix, -1}, rest);
    graph.setValue({d->id(), kTargetMatrix, -1}, target);
    graph.setValue({d->id(), kTwistAxis, -1}, int64_t(std::clamp(twistAxis, 0, 2)));
    graph.setValue({d->id(), kRadius, -1}, radius);
    graph.setValue({d->id(), kTwistWeight, -1}, twistWeight);
    graph.setValue({d->id(), drivers::kFalloff, -1}, int64_t(falloff));
    graph.connect({transform, Transform::kMatrix, -1}, {d->id(), kInMatrix, -1});
    return d->id();
}

NodeId createChannel(Graph& graph, const PlugRef& driver, const PlugRef& driven, const anim::Curve& curve, const std::string& curveName,
                     const std::string& name)
{
    std::string label = name;
    if (label.empty())
        label = driven.valid() ? graph.node(driven.node)->name() + "_" + graph.attr(driven).name + "Driven" : "drivenChannel";
    if (driven.valid() && driven.component >= 0)
        label = graph.node(driven.node)->name() + "_" + graph.attr(driven).childNames[size_t(driven.component)] + "Driven";
    Node* c = guarded([&] { return graph.createNode(*types::find("drivenChannel"), label, kNoNode, true); });
    setChannelCurve(graph, c->id(), curve);
    graph.setValue({c->id(), kCurveName, -1}, curveName);
    guarded([&] {
        if (driver.valid())
            graph.connect(driver, {c->id(), kInput, -1});
        if (driven.valid())
            graph.connect({c->id(), kChannelOutput, -1}, driven);
        return 0;
    });
    return c->id();
}

json describe(Graph& graph)
{
    graph.evaluate();
    json poses = json::array(), chans = json::array(), correctives = json::array();
    for (NodeId id : poseDrivers(graph))
        poses.push_back(poseInfo(graph, id));
    for (NodeId id : channels(graph))
        chans.push_back(channelInfo(graph, id));
    for (NodeId id : graph.nodes())
    {
        Node* n = graph.node(id);
        if (!n->type().isA("morph"))
            continue;
        NodeId mesh = modifiers::meshOf(graph, id);
        for (NodeId t : morphs::targetNodes(graph, id))
        {
            auto src = graph.source({t, morphs::kWeight, -1});
            if (!src)
                continue;
            correctives.push_back({{"mesh", mesh != kNoNode ? json(graph.node(mesh)->name()) : json(nullptr)},
                                   {"morph", n->name()},
                                   {"target", graph.node(t)->as<std::string>(morphs::kMorphName)},
                                   {"weightFrom", graph.plugName(*src)}});
        }
    }
    return {{"version", 1},
            {"space", "Lila local rotations (Y up, right handed), quaternions [x, y, z, w]; angles in degrees"},
            {"poseDrivers", poses},
            {"drivenChannels", chans},
            {"drivenMorphTargets", correctives}};
}

void registerTypes()
{
    if (types::find("poseDriver"))
        return;
    auto stored = [](std::string name, PlugType type, PlugValue def, std::string doc, uint32_t extra = 0, std::optional<double> lo = {},
                     std::optional<double> hi = {}) {
        return AttrDef{.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | extra,
                       .doc = std::move(doc), .minValue = lo, .maxValue = hi};
    };

    NodeType pose;
    pose.name = "poseDriver";
    pose.icon = "target-arrow";
    pose.doc = "Reads a joint's rotation against its rest and outputs 1 at a target pose, falling to 0 over a radius: the pose "
               "reader behind corrective morphs. Connect output to any keyable plug.";
    AttrDef axis = stored("twistAxis", PlugType::Enum, int64_t(0), "The bone axis twist is measured about.");
    axis.enumNames = {"x", "y", "z"};
    AttrDef fall = stored("falloff", PlugType::Enum, int64_t(1), "How the output eases from 1 at the pose to 0 at the radius.");
    fall.enumNames = {"linear", "smooth", "smoother"};
    pose.ownAttrs = {
        {.name = "inMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "The read transform's local matrix."},
        stored("restMatrix", PlugType::Matrix, glm::dmat4(1.0), "Local matrix at rest.", AttrHidden),
        stored("targetMatrix", PlugType::Matrix, glm::dmat4(1.0), "Local matrix at the target pose.", AttrHidden),
        axis,
        stored("radius", PlugType::Float, 90.0, "Degrees away from the pose where the output reaches 0.", AttrKeyable, 0.0, 180.0),
        stored("twistWeight", PlugType::Float, 0.0, "How much twist about the axis counts toward the distance; 0 ignores twist.", AttrKeyable, 0.0),
        fall,
        stored("inWeight", PlugType::Float, 1.0, "Multiplies the output; connect another driver for a combination.", AttrKeyable),
        {.name = "output", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput, .doc = "0 to 1, 1 at the target pose."},
    };
    pose.create = [] { return std::make_unique<PoseDriverNode>(); };
    types::registerType(std::move(pose));

    std::vector<std::string> inf = {"constant", "linear", "cycle", "cycleRelative", "oscillate"};
    NodeType ch;
    ch.name = "drivenChannel";
    ch.icon = "link";
    ch.doc = "Drives one plug from another through a curve of (driver value, driven value) keys, set driven key style. A "
             "curveName also bakes the output into an Unreal animation curve on export.";
    AttrDef pre = stored("preInfinity", PlugType::Enum, int64_t(0), "Before the first key.");
    pre.enumNames = inf;
    AttrDef post = stored("postInfinity", PlugType::Enum, int64_t(0), "After the last key.");
    post.enumNames = inf;
    ch.ownAttrs = {
        stored("input", PlugType::Float, 0.0, "The driver's value.", AttrKeyable),
        stored("keys", PlugType::String, std::string(), "Keys as anim curve JSON, time being the driver value; none passes the input through.",
               AttrHidden),
        pre,
        post,
        stored("curveName", PlugType::String, std::string(), "Unreal animation curve this bakes into on export; empty for none."),
        {.name = "output", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput, .doc = "The driven value."},
    };
    ch.create = [] { return std::make_unique<DrivenChannelNode>(); };
    types::registerType(std::move(ch));
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "poseDriver",
        .doc = "Make a pose driver reading a joint (or any transform) against its rest: output is 1 at the joint's pose now and "
               "falls to 0 over radius degrees. Connect its output with connectAttr or driveChannel. Returns the driver.",
        .positionalDoc = "joint",
        .flags = {{"axis", "", T::String, "x", "Twist axis, the joint's bone axis.", {"x", "y", "z"}},
                  {"radius", "", T::Float, 0.0, "Degrees from the pose where the output reaches 0; 0 uses the rest-to-pose angle.", {}, {}, 0.0, 180.0},
                  {"twistWeight", "", T::Float, 0.0, "How much twist counts, 0 ignoring it.", {}, {}, 0.0},
                  {"falloff", "", T::String, "smooth", "linear | smooth | smoother", {"linear", "smooth", "smoother"}},
                  {"rest", "", T::Vec3, json::array({0, 0, 0}), "The rest rotate, degrees."},
                  {"name", "n", T::String, nullptr, "Name for the driver."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("poseDriver: give a joint");
            const std::string ax = a.get<std::string>("axis"), fall = a.get<std::string>("falloff");
            const int axis = ax == "y" ? 1 : ax == "z" ? 2 : 0;
            const Falloff f = fall == "linear" ? Falloff::Linear : fall == "smoother" ? Falloff::Smoother : Falloff::Smooth;
            NodeId d = createPoseDriver(graph, nodeNamed(objs.back(), "poseDriver").id(), a.get<std::string>("name"), axis, a.get<double>("radius"),
                                        a.get<double>("twistWeight"), f, vec3Of(a.flags["rest"]));
            return graph.node(d)->name();
        },
    });

    ops::registerCommand({
        .name = "setPoseDriverTarget",
        .doc = "Make the read joint's pose now the driver's target (output 1 here); rest=true makes it the rest instead.",
        .positionalDoc = "driver",
        .flags = {{"rest", "", T::Bool, false, "Capture the rest instead of the target."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("setPoseDriverTarget: no driver");
            Node& d = nodeNamed(objs.back(), "setPoseDriverTarget");
            if (!isPoseDriver(d))
                throw CommandError("setPoseDriverTarget: " + d.name() + " is not a poseDriver");
            auto src = graph.source({d.id(), kInMatrix, -1});
            if (!src)
                throw CommandError("setPoseDriverTarget: " + d.name() + " reads no transform");
            graph.evaluate();
            graph.setValue({d.id(), a.get<bool>("rest") ? kRestMatrix : kTargetMatrix, -1}, std::get<glm::dmat4>(graph.value(*src)));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "poseDrivers",
        .doc = "The pose drivers: [{node, transform, twistAxis, radius, twistWeight, falloff, restRotation, targetRotation, "
               "multipliedBy, drives, output}].",
        .undoable = false,
        .run = [](const Args&) -> json {
            Graph& graph = g();
            graph.evaluate();
            json out = json::array();
            for (NodeId id : poseDrivers(graph))
                out.push_back(poseInfo(graph, id));
            return out;
        },
    });

    ops::registerCommand({
        .name = "driveChannel",
        .doc = "Drive plugs from a driver plug through a curve of [driver value, driven value] keys (set driven key). Any keyable "
               "Float plug or vector component works: morph weights, material parameters (roughness), transform channels, node "
               "group inputs. With no driven plug the channel only bakes curveName on export (an Unreal material or morph curve "
               "with no Lila counterpart). Material plugs default curveName to <Param>_mat. Returns the channels.",
        .positionalDoc = "driven plugs",
        .flags = {{"driver", "d", T::String, nullptr, "The driving plug, e.g. Elbow_corrDriver.output or ctrl.ty."},
                  {"keys", "k", T::List, nullptr, "[[driver, driven], ...]; default [[0, 0], [1, 1]]."},
                  {"interpolation", "i", T::String, "linear", "Key interpolation: linear, spline, flat, step, auto, clamped."},
                  {"preInfinity", "", T::String, "constant", "Before the first key.", {"constant", "linear", "cycle", "cycleRelative", "oscillate"}},
                  {"postInfinity", "", T::String, "constant", "After the last key.", {"constant", "linear", "cycle", "cycleRelative", "oscillate"}},
                  {"curveName", "", T::String, nullptr, "Unreal curve to bake into on export."},
                  {"force", "f", T::Bool, false, "Replace an existing connection (keys) on the driven plug."},
                  {"name", "n", T::String, nullptr, "Name for the channel."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            PlugRef driver;
            if (a.has("driver"))
            {
                driver = plugNamed(graph, a.get<std::string>("driver"), "driveChannel");
                if (!scalarPlug(graph, driver) && graph.attr(driver).type != PlugType::Int && graph.attr(driver).type != PlugType::Bool &&
                    graph.attr(driver).type != PlugType::Enum)
                    throw CommandError("driveChannel: the driver must be a number plug");
            }
            const anim::Tangent tangent = tangentNamed(a.get<std::string>("interpolation"), "driveChannel");
            anim::Curve c;
            c.keys = keysFrom(a.has("keys") ? a.flags["keys"] : json::array({json::array({0, 0}), json::array({1, 1})}), tangent, "driveChannel");
            c.pre = anim::infinityFromName(a.get<std::string>("preInfinity"));
            c.post = anim::infinityFromName(a.get<std::string>("postInfinity"));
            std::vector<PlugRef> driven;
            for (const json& p : a.positional)
                driven.push_back(plugNamed(graph, p.get<std::string>(), "driveChannel"));
            if (driven.empty() && !a.has("curveName"))
                throw CommandError("driveChannel: give a driven plug or a curveName");
            if (driven.empty())
                driven.push_back({});
            json out = json::array();
            for (const PlugRef& d : driven)
            {
                if (d.valid())
                {
                    if (!scalarPlug(graph, d))
                        throw CommandError("driveChannel: " + graph.plugName(d) + " is not a Float plug or vector component");
                    if (graph.source(d))
                    {
                        if (!a.get<bool>("force"))
                            throw CommandError("driveChannel: " + graph.plugName(d) + " is already driven; pass force=True to replace it");
                        graph.disconnect(d);
                    }
                }
                std::string curve = a.has("curveName") ? a.get<std::string>("curveName") : d.valid() ? defaultCurveName(graph, d) : std::string();
                out.push_back(graph.node(createChannel(graph, driver, d, c, curve, a.get<std::string>("name")))->name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "setDrivenKey",
        .doc = "Add or replace a key on a driven channel: at the driver's value now (or input=), the driven value value=.",
        .positionalDoc = "channel",
        .flags = {{"input", "", T::Float, nullptr, "Driver value of the key; default the driver's value now."},
                  {"value", "v", T::Float, nullptr, "Driven value of the key."},
                  {"interpolation", "i", T::String, nullptr, "Key interpolation; default the neighbouring keys'."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty() || !a.has("value"))
                throw CommandError("setDrivenKey: give the channel and value=");
            Node& n = channelNamed(objs.back(), "setDrivenKey");
            anim::Curve c = channelCurve(n);
            anim::Key k;
            k.time = a.has("input") ? a.get<double>("input") : std::get<double>(graph.value({n.id(), kInput, -1}));
            k.value = a.get<double>("value");
            if (a.has("interpolation"))
                k.in = k.out = tangentNamed(a.get<std::string>("interpolation"), "setDrivenKey");
            else if (!c.keys.empty())
            {
                k.in = c.keys.back().in;
                k.out = c.keys.back().out;
            }
            else
                k.in = k.out = anim::Tangent::Linear;
            c.insert(k);
            setChannelCurve(graph, n.id(), c);
            return keysJson(c);
        },
    });

    ops::registerCommand({
        .name = "removeDrivenKey",
        .doc = "Remove a driven channel's key at the given index (keys sorted by driver value).",
        .positionalDoc = "channel",
        .flags = {{"index", "", T::Int, 0, "Key index.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("removeDrivenKey: no channel");
            Node& n = channelNamed(objs.back(), "removeDrivenKey");
            anim::Curve c = channelCurve(n);
            int i = a.get<int>("index");
            if (i < 0 || i >= int(c.keys.size()))
                throw CommandError(std::format("removeDrivenKey: {} has {} keys", n.name(), c.keys.size()));
            c.keys.erase(c.keys.begin() + i);
            setChannelCurve(graph, n.id(), c);
            return keysJson(c);
        },
    });

    ops::registerCommand({
        .name = "editDrivenChannel",
        .doc = "Change a driven channel: replace its keys, infinity, interpolation of every key, curveName or driver.",
        .positionalDoc = "channel",
        .flags = {{"keys", "k", T::List, nullptr, "[[driver, driven], ...]."},
                  {"interpolation", "i", T::String, nullptr, "Set every key's interpolation."},
                  {"preInfinity", "", T::String, nullptr, "Before the first key.", {"constant", "linear", "cycle", "cycleRelative", "oscillate"}},
                  {"postInfinity", "", T::String, nullptr, "After the last key.", {"constant", "linear", "cycle", "cycleRelative", "oscillate"}},
                  {"curveName", "", T::String, nullptr, "Unreal curve to bake into; empty for none."},
                  {"driver", "d", T::String, nullptr, "A new driving plug; empty disconnects it."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("editDrivenChannel: no channel");
            Node& n = channelNamed(objs.back(), "editDrivenChannel");
            anim::Curve c = channelCurve(n);
            if (a.has("keys"))
                c.keys = keysFrom(a.flags["keys"], anim::Tangent::Linear, "editDrivenChannel");
            if (a.has("interpolation"))
            {
                const anim::Tangent t = tangentNamed(a.get<std::string>("interpolation"), "editDrivenChannel");
                for (anim::Key& k : c.keys)
                    k.in = k.out = t;
            }
            if (a.has("preInfinity"))
                c.pre = anim::infinityFromName(a.get<std::string>("preInfinity"));
            if (a.has("postInfinity"))
                c.post = anim::infinityFromName(a.get<std::string>("postInfinity"));
            setChannelCurve(graph, n.id(), c);
            if (a.has("curveName"))
                graph.setValue({n.id(), kCurveName, -1}, a.get<std::string>("curveName"));
            if (a.has("driver"))
            {
                PlugRef in{n.id(), kInput, -1};
                if (graph.source(in))
                    graph.disconnect(in);
                if (std::string d = a.get<std::string>("driver"); !d.empty())
                    guarded([&] {
                        graph.connect(plugNamed(graph, d, "editDrivenChannel"), in);
                        return 0;
                    });
            }
            return channelInfo(graph, n.id());
        },
    });

    ops::registerCommand({
        .name = "drivenChannels",
        .doc = "Driven channels: all, or those touching the given nodes or plugs (as driver or driven). [{node, driver, driven, keys, "
               "preInfinity, postInfinity, curveName, input, output}]; keys are [driver value, driven value, interpolation].",
        .positionalDoc = "nodes or plugs?",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            graph.evaluate();
            std::vector<std::string> filter;
            for (const json& p : a.positional)
                filter.push_back(p.get<std::string>());
            json out = json::array();
            for (NodeId id : channels(graph))
            {
                json info = channelInfo(graph, id);
                if (!filter.empty())
                {
                    auto touches = [&](const std::string& plug) {
                        for (const std::string& f : filter)
                            if (plug == f || plug.starts_with(f + "."))
                                return true;
                        return false;
                    };
                    bool hit = std::find(filter.begin(), filter.end(), info["node"].get<std::string>()) != filter.end();
                    hit = hit || (info["driver"].is_string() && touches(info["driver"].get<std::string>()));
                    for (const json& d : info["driven"])
                        hit = hit || touches(d.get<std::string>());
                    if (!hit)
                        continue;
                }
                out.push_back(std::move(info));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "removeDrivenChannel",
        .doc = "Delete driven channels; each driven plug keeps the value it has now.",
        .positionalDoc = "channels",
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            graph.evaluate();
            for (const std::string& name : a.objects())
            {
                Node& n = channelNamed(name, "removeDrivenChannel");
                const double v = std::get<double>(graph.value({n.id(), kChannelOutput, -1}));
                std::vector<PlugRef> dsts = graph.destinations({n.id(), kChannelOutput, -1});
                for (const PlugRef& d : dsts)
                    graph.disconnect(d);
                for (const PlugRef& d : dsts)
                    guarded([&] {
                        graph.setValue(d, v);
                        return 0;
                    });
                graph.deleteNode(n.id());
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "exportDriverData",
        .doc = "Write the pose drivers, driven channels and driven morph targets to a JSON file, for rebuilding them in Unreal "
               "(a Control Rig or Pose Asset setup). Returns the data.",
        .positionalDoc = "path",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            json data = describe(graph);
            if (!a.positional.empty())
            {
                const std::string path = a.positional[0].get<std::string>();
                std::ofstream f(paths::fromUtf8(path), std::ios::binary);
                if (!f)
                    throw CommandError("exportDriverData: can't write " + path);
                f << data.dump(2);
            }
            return data;
        },
    });
}
} // namespace drivers
} // namespace lila::scene
