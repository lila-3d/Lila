// Puppeteer's rig element commands: thin wrappers over base Lila's IK chains, reverse feet, lookers and spaces that put
// new elements under the joint's character. Also the types older Puppeteer scenes saved, loaded only to be migrated.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Anim.h>
#include <lila/AnimCurve.h>
#include <lila/Events.h>
#include <lila/Prefs.h>
#include <lila/Spaces.h>
#include <lila/Undo.h>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

using namespace lila;
using namespace lila::scene;
namespace rigel = lila::scene::rigel;

namespace puppeteer
{
namespace
{
constexpr int kMaxSpaces = 8;

/** Index of the first own attr of each type: its base type's attr count, known once the base is registered. */
int gTransformAttrs = Transform::kCount;

glm::dmat3 orientMatrix(const glm::dvec3& degrees) { return glm::dmat3(eulerToMatrix(degrees, RotateOrder::XYZ)); }

glm::dvec3 solvedRotateFor(const glm::dmat3& parentWorld, const glm::dvec3& jointOrient, const glm::dvec3& rotateAxis, int64_t order,
                           const glm::dmat3& world)
{
    const glm::dmat3 r = math::localRotationFor(parentWorld, orientMatrix(jointOrient), world) * glm::transpose(orientMatrix(rotateAxis));
    return matrixToEuler(r, RotateOrder(order));
}

// Older scenes' types ---------------------------------------------------------------------------------------------
// They evaluate as they did when saved, so the migration can read the pose they made.

/** An IK chain whose keyed ik channel switched it between IK and FK per key. */
class LegacyChainNode : public Transform
{
public:
    enum Own
    {
        kStartJoint,
        kEndJoint,
        kHandle,
        kIk,
        kIkBlend,
        kLock,
        kAlign,
    };
    static int at(int own) { return gTransformAttrs + own; }

    void compute(const EvalContext& ctx) override
    {
        Transform::compute(ctx);
        setOutput(at(kIkBlend), math::ikWeight(as<double>(at(kIk))));
    }
};

class LegacyFootNode : public Transform
{
public:
    enum Own
    {
        kAnkle,
        kBall,
        kToe,
        kChain,
        kAlign,
        kRoll,
        kTwist,
        kBank,
        kPoleAngle,
        kBallLimit,
        kToeLimit,
        kHeelPivot,
        kBallPivot,
        kToePivot,
        kInnerPivot,
        kOuterPivot,
        kRestAnkle,
        kRestBall,
        kBallOrient,
        kBallAxis,
        kBallRotateOrder,
        kIkWeight,
        kAnkleTarget,
        kAnkleMatrix,
        kBallRotate,
        kBallBlend,
        kRollBackMax,
        kRollForwardMax,
        kBankMax,
        kTwistMax,
        kZeroSnap,
    };
    static int at(int own) { return gTransformAttrs + own; }

    void compute(const EvalContext& ctx) override
    {
        Transform::compute(ctx);
        const math::FootPivots p{as<glm::dvec3>(at(kHeelPivot)), as<glm::dvec3>(at(kBallPivot)), as<glm::dvec3>(at(kToePivot)),
                                 as<glm::dvec3>(at(kInnerPivot)), as<glm::dvec3>(at(kOuterPivot))};
        const math::FootLimits l{.ballLimit = as<double>(at(kBallLimit)),
                                 .toeLimit = as<double>(at(kToeLimit)),
                                 .heelMax = as<double>(at(kRollBackMax)),
                                 .toeMax = as<double>(at(kRollForwardMax)),
                                 .bankMax = as<double>(at(kBankMax)),
                                 .twistMax = as<double>(at(kTwistMax))};
        const math::FootSolve s = math::solveFoot(p, {as<double>(at(kRoll)), as<double>(at(kTwist)), as<double>(at(kBank))}, l);
        const glm::dmat4 control = as<glm::dmat4>(kWorldMatrix);
        const glm::dmat4 ankle = control * s.ankle * as<glm::dmat4>(at(kRestAnkle));
        const glm::dmat4 ball = control * s.ball * as<glm::dmat4>(at(kRestBall));
        setOutput(at(kAnkleTarget), glm::dvec3(ankle[3]));
        setOutput(at(kAnkleMatrix), ankle);
        setOutput(at(kBallRotate), solvedRotateFor(math::rotationOf(ankle), as<glm::dvec3>(at(kBallOrient)), as<glm::dvec3>(at(kBallAxis)),
                                                   as<int64_t>(at(kBallRotateOrder)), math::rotationOf(ball)));
        setOutput(at(kBallBlend), std::clamp(as<double>(at(kIkWeight)), 0.0, 1.0));
    }
};

class LegacyAlignNode : public Node
{
public:
    enum Own
    {
        kTarget,
        kParentMatrix,
        kWeight,
        kJointOrient,
        kRotateAxis,
        kRotateOrder,
        kSolvedRotate,
        kIkBlend,
        kOffset,
    };

    void compute(const EvalContext&) override
    {
        setOutput(kSolvedRotate, solvedRotateFor(math::rotationOf(as<glm::dmat4>(kParentMatrix)), as<glm::dvec3>(kJointOrient),
                                                 as<glm::dvec3>(kRotateAxis), as<int64_t>(kRotateOrder),
                                                 math::rotationOf(as<glm::dmat4>(kTarget)) * math::rotationOf(as<glm::dmat4>(kOffset))));
        setOutput(kIkBlend, std::clamp(as<double>(kWeight), 0.0, 1.0));
    }
};

class LegacyLookerNode : public Transform
{
public:
    enum Own
    {
        kJoint,
        kAimAxis,
        kUpAxis,
        kUpVector,
        kWeight,
        kJointParentMatrix,
        kRestTranslate,
        kJointOrient,
        kRotateAxis,
        kRotateOrder,
        kSolvedRotate,
        kIkBlend,
    };
    static int at(int own) { return gTransformAttrs + own; }

    void compute(const EvalContext& ctx) override
    {
        Transform::compute(ctx);
        const glm::dmat4 parent = as<glm::dmat4>(at(kJointParentMatrix));
        const glm::dvec3 from(parent * glm::dvec4(as<glm::dvec3>(at(kRestTranslate)), 1.0));
        const glm::dvec3 target(as<glm::dmat4>(kWorldMatrix)[3]);
        glm::dvec3 rotate = as<glm::dvec3>(at(kSolvedRotate));
        try
        {
            const glm::dvec3 aim = target - from;
            if (glm::length(aim) > 1e-9)
                rotate = solvedRotateFor(math::rotationOf(parent), as<glm::dvec3>(at(kJointOrient)), as<glm::dvec3>(at(kRotateAxis)),
                                         as<int64_t>(at(kRotateOrder)),
                                         rig::aimFrame(rig::axisVector(as<std::string>(at(kAimAxis))), rig::axisVector(as<std::string>(at(kUpAxis))),
                                                       glm::normalize(aim), as<glm::dvec3>(at(kUpVector))));
        }
        catch (const std::exception&)
        {
        }
        setOutput(at(kSolvedRotate), rotate);
        setOutput(at(kIkBlend), std::clamp(as<double>(at(kWeight)), 0.0, 1.0));
    }
};

/** A joint's spaces before base spaces: its channel picked the parent (0) or a space, driving a parent constraint. */
class LegacySpaceNode : public Node
{
public:
    enum Own
    {
        kJoint,
        kSpaces,
        kSpace,
        kKeyedTranslate,
        kKeyedRotate,
        kJointOrient,
        kRotateAxis,
        kRotateOrder,
        kOffset,
        kBlendWeight,
        kWeights,
    };
    void compute(const EvalContext&) override
    {
        const int count = int(std::count(as<std::string>(kSpaces).begin(), as<std::string>(kSpaces).end(), ' ')) + 1;
        const int space = std::clamp(int(std::lround(as<double>(kSpace))), 0, std::min(count, kMaxSpaces));
        glm::dmat4 k = glm::translate(glm::dmat4(1.0), as<glm::dvec3>(kKeyedTranslate));
        k = k * glm::dmat4(orientMatrix(as<glm::dvec3>(kJointOrient))) *
            glm::dmat4(glm::dmat3(eulerToMatrix(as<glm::dvec3>(kKeyedRotate), RotateOrder(as<int64_t>(kRotateOrder))))) *
            glm::dmat4(orientMatrix(as<glm::dvec3>(kRotateAxis)));
        setOutput(kOffset, k);
        setOutput(kBlendWeight, space > 0 ? 1.0 : 0.0);
        // Slot 0 stays weighted in the parent space so the unused constraint result stays finite.
        for (int i = 0; i < kMaxSpaces; ++i)
            setOutput(kWeights + i, (space > 0 ? i == space - 1 : i == 0) ? 1.0 : 0.0);
    }
};

std::vector<std::string> words(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;)
        out.push_back(w);
    return out;
}

/** The legacy types, with the attrs older scenes saved, so they load and evaluate as they did. */
bool registerLegacyTypes(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    const NodeType* chainBase = types::find(rigel::kIkChainType);
    const NodeType* footBase = types::find(rigel::kReverseFootType);
    const NodeType* alignBase = types::find(rigel::kAlignType);
    const NodeType* lookerBase = types::find(rigel::kLookerType);
    if (!transform || !chainBase || !footBase || !alignBase || !lookerBase)
        return false;
    gTransformAttrs = int(transform->attrs.size());
    auto own = [&](const NodeType& t) {
        return std::vector<AttrDef>(t.attrs.begin() + (t.base ? std::ptrdiff_t(t.base->attrs.size()) : 0), t.attrs.end());
    };
    auto byName = [](std::vector<AttrDef> attrs, const char* name) {
        for (const AttrDef& a : attrs)
            if (a.name == name)
                return a;
        throw std::logic_error(std::string("legacy Puppeteer types: no attr ") + name);
    };
    const uint32_t legacy = AttrHidden;

    NodeType chain{.name = kLegacyIkChainType, .base = transform, .dag = true, .doc = "An older Puppeteer IK chain, migrated to ikChain on load.",
                   .icon = "line-dashed", .create = [] { return std::make_unique<LegacyChainNode>(); }};
    {
        const std::vector<AttrDef> b = own(*chainBase);
        AttrDef ik{.name = "ik", .type = PlugType::Float, .defaultValue = 1.0, .flags = AttrInput | AttrStorable | AttrKeyable | legacy,
                   .doc = "The per-key IK/FK flag older scenes keyed."};
        chain.ownAttrs = {byName(b, "startJoint"), byName(b, "endJoint"), byName(b, "handle"), ik, byName(b, "ikBlend"),
                          byName(b, "lock"), byName(b, "align")};
    }
    NodeType foot{.name = kLegacyReverseFootType, .base = transform, .dag = true, .doc = "An older Puppeteer reverse foot, migrated on load.",
                  .icon = "shoe", .create = [] { return std::make_unique<LegacyFootNode>(); }};
    foot.ownAttrs = own(*footBase);
    NodeType align{.name = kLegacyAlignType, .doc = "An older Puppeteer align node, migrated on load.", .icon = "arrows-diagonal",
                   .create = [] { return std::make_unique<LegacyAlignNode>(); }};
    align.ownAttrs = alignBase->attrs;
    NodeType looker{.name = kLegacyLookerType, .base = transform, .dag = true, .doc = "An older Puppeteer looker, migrated on load.",
                    .icon = "eye", .create = [] { return std::make_unique<LegacyLookerNode>(); }};
    looker.ownAttrs = own(*lookerBase);

    NodeType space{.name = kSpaceType, .doc = "An older Puppeteer joint space, migrated to base spaces on load.", .icon = "world",
                   .create = [] { return std::make_unique<LegacySpaceNode>(); }};
    auto vec = [](const char* n, const char* doc) {
        AttrDef a{.name = n, .type = PlugType::Vec3, .defaultValue = glm::dvec3(0.0), .flags = AttrInput | AttrStorable, .doc = doc};
        a.childNames = {std::string(n) + "X", std::string(n) + "Y", std::string(n) + "Z"};
        return a;
    };
    space.ownAttrs = {
        {.name = "joint", .type = PlugType::String, .defaultValue = std::string(), .doc = "The joint."},
        {.name = "spaces", .type = PlugType::String, .defaultValue = std::string(), .doc = "The spaces, space separated."},
        {.name = "space", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable | AttrKeyable, .doc = "The active space."},
        vec("keyedTranslate", "The joint's keyed translate."),
        vec("keyedRotate", "The joint's keyed rotate."),
        vec("jointOrient", "The joint's jointOrient."),
        vec("rotateAxis", "The joint's rotateAxis."),
        {.name = "rotateOrder", .type = PlugType::Int, .defaultValue = int64_t(0), .doc = "The joint's rotate order."},
        {.name = "offset", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrOutput | AttrHidden, .doc = "The keyed local matrix."},
        {.name = "blendWeight", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput | AttrHidden, .doc = "1 outside the parent space."},
    };
    for (int i = 0; i < kMaxSpaces; ++i)
        space.ownAttrs.push_back({.name = "weight" + std::to_string(i), .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput | AttrHidden,
                                  .doc = "Weight of the constraint's target slot."});

    for (NodeType* t : {&chain, &foot, &align, &looker, &space})
        if (!types::registerType(ctx, std::move(*t)))
            return false;
    return true;
}

// Commands --------------------------------------------------------------------------------------------------------

Graph& graph() { return Graph::current(); }

/** The base command's call with the parent set to the first object's character, when it has one. */
json withCharacter(Graph& g, json call, const std::string& node)
{
    if (call.contains("parent"))
        return call;
    if (Node* n = g.find(node))
        if (NodeId c = elementParentFor(g, n->id()))
            call["parent"] = g.node(c)->name();
    return call;
}

/** The command's own call with its objects and flags passed on as they came. */
json forwarded(const Args& a)
{
    json call = a.flags;
    call["args"] = objectArgs(a);
    return call;
}

json createIkChain(const Args& a)
{
    Graph& g = graph();
    json call = forwarded(a);
    const std::vector<std::string> objs = objectArgs(a);
    const std::string start = a.get<std::string>("startJoint", objs.empty() ? "" : objs[0]);
    return cmds::execute("ikChain", withCharacter(g, call, start));
}

json createReverseFoot(const Args& a)
{
    Graph& g = graph();
    const std::vector<std::string> objs = objectArgs(a);
    const std::string ankle = a.get<std::string>("ankle", objs.empty() ? "" : objs[0]);
    const json r = cmds::execute("reverseFoot", withCharacter(g, forwarded(a), ankle));
    // The foot's limits come from Puppeteer's preferences.
    const std::string foot = r.get<std::string>();
    const std::pair<const char*, double> limits[] = {{"ballLimit", 30.0},    {"toeLimit", 60.0}, {"rollBackMax", 60.0}, {"rollForwardMax", 90.0},
                                                     {"bankMax", 45.0},      {"twistMax", 90.0}, {"zeroSnap", 0.5}};
    for (const auto& [attr, fallback] : limits)
    {
        const double v = PrefStore::get().value<double>(kPrefs, std::string("/reverseFoot/") + attr, fallback);
        if (Node* f = g.find(foot); f && f->as<double>(attrOf(*f, attr)) != v)
            g.setValue(plugOf(*f, attr), v);
    }
    return r;
}

json createSpace(const Args& a)
{
    Graph& g = graph();
    std::vector<std::string> objs = objectArgs(a);
    if (objs.empty())
        throw CommandError("puppeteerSpace: give the joint");
    const NodeId j = jointNamed(g, objs.front(), "puppeteerSpace").id();
    const json list = a.flags.value("spaces", json::array());
    if (!list.is_array() || list.empty())
        throw CommandError("puppeteerSpace: give the spaces ('world' or other nodes)");
    cmds::execute("addSpace", {{"args", {g.node(j)->name()}}, {"space", list}});
    g.select({j});
    return g.node(lila::scene::spaces::switchOf(g, j))->name();
}

json switchSpace(const Args& a)
{
    Graph& g = graph();
    std::vector<std::string> objs = objectArgs(a);
    if (objs.empty())
        throw CommandError("puppeteerSwitchSpace: give the joint");
    const NodeId j = jointNamed(g, objs.front(), "puppeteerSwitchSpace").id();
    if (!lila::scene::spaces::hasSpaces(g, j))
        throw CommandError("puppeteerSwitchSpace: " + g.node(j)->name() + " has no spaces (puppeteerSpace)");
    json call = {{"args", {g.node(j)->name()}}, {"space", a.flags.value("space", json())}};
    if (a.has("time"))
        call["time"] = a.get<double>("time");
    return cmds::execute("switchSpace", call);
}

// Migration -------------------------------------------------------------------------------------------------------

/**
 * Remakes a node as another type with the same name, parent, values (by attr name) and connections (by attr name).
 * Attrs the new type lacks are dropped, curves driving them deleted. Returns the new node.
 */
NodeId retype(Graph& g, NodeId old, const NodeType& type)
{
    Node& o = *g.node(old);
    const std::string name = o.name();
    const NodeId parent = g.parent(old);
    const std::vector<NodeId> kids = g.children(old);
    Node* n = g.createNode(type, name + "_migrating", parent, false);
    const NodeId id = n->id();
    auto attrIn = [&](const Node& node, const std::string& attr) { return node.type().attrIndex(attr); };
    for (int a = 0; a < int(o.type().attrs.size()); ++a)
    {
        const AttrDef& def = o.type().attrs[size_t(a)];
        const int to = attrIn(*n, def.name);
        if (to >= 0 && (def.flags & AttrInput) && !g.source({old, a, -1}) && o.value(a) != n->value(to))
            g.setValue({id, to, -1}, o.value(a));
    }
    std::vector<std::pair<PlugRef, PlugRef>> in, out;
    std::vector<NodeId> orphanCurves;
    for (const auto& [dst, src] : g.connections())
    {
        if (dst.node == old)
        {
            const int to = attrIn(*n, o.type().attrs[size_t(dst.attr)].name);
            if (to >= 0)
                in.push_back({src, {id, to, dst.component}});
            else if (Node* s = g.node(src.node); s && lila::scene::anim::isCurve(*s))
                orphanCurves.push_back(src.node);
        }
        else if (src.node == old)
            if (const int from = attrIn(*n, o.type().attrs[size_t(src.attr)].name); from >= 0)
                out.push_back({{id, from, src.component}, dst});
    }
    for (NodeId k : kids)
        g.setParent(k, id, false);
    g.deleteNode(old);
    for (NodeId c : orphanCurves)
        if (g.node(c))
            g.deleteNode(c);
    for (const auto& [src, dst] : in)
        if (g.node(src.node))
            g.connect(src, dst, true);
    for (const auto& [src, dst] : out)
        if (g.node(dst.node))
            g.connect(src, dst, true);
    g.rename(id, name);
    return id;
}

/** Sorted, unique key times of the curves driving any input of the nodes. */
std::vector<double> keyTimesOf(Graph& g, const std::vector<NodeId>& nodes)
{
    std::set<double> times;
    std::set<NodeId> wanted(nodes.begin(), nodes.end());
    for (const auto& [dst, src] : g.connections())
        if (wanted.contains(dst.node))
            if (Node* s = g.node(src.node); s && (lila::scene::anim::isCurve(*s) || lila::scene::anim::isRotationCurve(*s)))
                for (double t : lila::scene::anim::curveKeyTimes(*s))
                    times.insert(t);
    return {times.begin(), times.end()};
}

/** What a legacy chain posed at each of its keys while in IK: joint world rotations, parents first. */
struct ChainSamples
{
    std::string chain;
    std::vector<std::string> joints;
    std::vector<std::pair<double, std::vector<glm::dmat3>>> poses;
    bool keyed = false;
};

ChainSamples sampleLegacyChain(Graph& g, NodeId chain)
{
    ChainSamples s;
    const Node& c = *g.node(chain);
    s.chain = c.name();
    Node* start = g.find(textOf(c, "startJoint"));
    Node* end = g.find(textOf(c, "endJoint"));
    if (!start || !end)
        return s;
    std::vector<NodeId> joints;
    for (NodeId cur = end->id(); cur; cur = g.parent(cur))
    {
        joints.push_back(cur);
        if (cur == start->id())
            break;
    }
    std::reverse(joints.begin(), joints.end());
    std::vector<NodeId> elements{chain};
    if (Node* h = g.find(textOf(c, "handle")))
        elements.push_back(h->id());
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(kLegacyReverseFootType) && textOf(*n, "chain") == c.name())
        {
            elements.push_back(id);
            if (Node* ball = g.find(textOf(*n, "ball")))
                joints.push_back(ball->id());
        }
    for (NodeId j : joints)
        s.joints.push_back(g.node(j)->name());
    std::vector<NodeId> all = elements;
    all.insert(all.end(), joints.begin(), joints.end());
    std::vector<double> times = keyTimesOf(g, all);
    s.keyed = !times.empty();
    if (times.empty())
        times.push_back(g.time());
    const PlugRef blend{chain, LegacyChainNode::at(LegacyChainNode::kIkBlend), -1};
    const double was = g.time();
    for (double t : times)
    {
        if (std::abs(g.time() - t) > 1e-9)
            g.setTime(t);
        g.evaluate();
        if (std::get<double>(g.value(blend)) <= 1e-9)
            continue;
        std::vector<glm::dmat3> w;
        for (NodeId j : joints)
            w.push_back(math::rotationOf(worldOf(g, j)));
        s.poses.push_back({t, std::move(w)});
    }
    if (std::abs(g.time() - was) > 1e-9)
        g.setTime(was);
    g.evaluate();
    return s;
}

/** Keys (or, unanimated, sets) the sampled IK poses into the joints' rotate. */
void bakeSamples(Graph& g, const ChainSamples& s)
{
    const double was = g.time();
    for (const auto& [t, worlds] : s.poses)
    {
        if (std::abs(g.time() - t) > 1e-9)
            g.setTime(t);
        g.evaluate();
        std::vector<std::string> plugs;
        for (size_t i = 0; i < s.joints.size() && i < worlds.size(); ++i)
            if (Node* j = g.find(s.joints[i]))
            {
                setWorldRotation(g, j->id(), worlds[i]);
                plugs.push_back(j->name() + ".rotate");
            }
        if (s.keyed && !plugs.empty())
            cmds::execute("setKeyframe", {{"args", plugs}, {"time", t}});
    }
    if (std::abs(g.time() - was) > 1e-9)
        g.setTime(was);
    g.evaluate();
}
} // namespace

int migrateSpaces(Graph& g)
{
    std::vector<NodeId> legacy;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(kSpaceType))
            legacy.push_back(id);
    if (legacy.empty())
        return 0;
    UndoChunk chunk("Migrate Puppeteer Spaces");
    for (NodeId sp : legacy)
    {
        const Node& s = *g.node(sp);
        Node* j = g.find(s.as<std::string>(LegacySpaceNode::kJoint));
        const std::vector<std::string> names = words(s.as<std::string>(LegacySpaceNode::kSpaces));
        const std::optional<PlugRef> curve = g.source({sp, LegacySpaceNode::kSpace, -1});
        const double value = s.as<double>(LegacySpaceNode::kSpace);
        std::vector<std::string> constraints;
        for (int i = 0; i < kMaxSpaces; ++i)
            for (const PlugRef& d : g.destinations({sp, LegacySpaceNode::kWeights + i, -1}))
                if (std::find(constraints.begin(), constraints.end(), g.node(d.node)->name()) == constraints.end())
                    constraints.push_back(g.node(d.node)->name());
        // The constraint held the joint in its space; the joint's keys go back to driving it directly.
        for (const std::string& c : constraints)
            cmds::execute("removeConstraint", {{"args", {c}}});
        if (j)
        {
            // Old space keys were read in the space's own frame, which is a base space without an offset.
            std::vector<int> index{0};
            for (const std::string& n : names)
                index.push_back(g.find(n) ? lila::scene::spaces::addSpace(g, j->id(), n, false) : 0);
            auto mapped = [&](double v) { return index[size_t(std::clamp(int(std::lround(v)), 0, int(index.size()) - 1))]; };
            if (curve)
            {
                g.disconnect({sp, LegacySpaceNode::kSpace, -1});
                if (Node* cn = g.node(curve->node); cn && lila::scene::anim::isCurve(*cn))
                {
                    lila::scene::anim::Curve c = lila::scene::anim::curveOf(*cn);
                    for (lila::scene::anim::Key& k : c.keys)
                        k.value = mapped(k.value);
                    lila::scene::anim::setCurve(g, curve->node, c);
                }
                g.connect(*curve, {j->id(), Transform::kSpace, -1});
            }
            else
                g.setValue({j->id(), Transform::kSpace, -1}, int64_t(mapped(value)));
        }
        g.deleteNode(sp);
    }
    return int(legacy.size());
}

int migrateRigElements(Graph& g)
{
    std::vector<NodeId> chains, feet, aligns, lookers;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id))
        {
            if (n->type().isA(kLegacyIkChainType))
                chains.push_back(id);
            else if (n->type().isA(kLegacyReverseFootType))
                feet.push_back(id);
            else if (n->type().isA(kLegacyAlignType))
                aligns.push_back(id);
            else if (n->type().isA(kLegacyLookerType))
                lookers.push_back(id);
        }
    const size_t count = chains.size() + feet.size() + aligns.size() + lookers.size();
    if (!count)
        return 0;
    UndoChunk chunk("Migrate Puppeteer Rig Elements");
    // The IK pose each chain made at its keys, read while the old types still evaluate the old way.
    std::vector<ChainSamples> samples;
    for (NodeId c : chains)
        samples.push_back(sampleLegacyChain(g, c));
    for (NodeId id : aligns)
        retype(g, id, *types::find(rigel::kAlignType));
    for (NodeId id : feet)
        retype(g, id, *types::find(rigel::kReverseFootType));
    for (NodeId id : lookers)
        retype(g, id, *types::find(rigel::kLookerType));
    for (NodeId id : chains)
        retype(g, id, *types::find(rigel::kIkChainType));
    for (const ChainSamples& s : samples)
        bakeSamples(g, s);
    return int(count);
}

bool registerRigElements(ext::Context& ctx)
{
    if (!registerLegacyTypes(ctx))
        return false;
    using T = ArgType;
    const FlagDef parent{"parent", "p", T::String, nullptr, "Node to put it under; default the joint's character."};

    ctx.registerCommand({
        .name = "puppeteerIkChain",
        .doc = "Make an IK chain (ikChain) from a start joint to an end effector, under the joint's character. Grabbing its "
               "controller poses the joints' rotations; there is no IK/FK switch. Returns the chain.",
        .positionalDoc = "startJoint endJoint (default the selection)",
        .flags = {
            {"startJoint", "sj", T::String, nullptr, "First joint."},
            {"endJoint", "ej", T::String, nullptr, "End effector."},
            {"solver", "sol", T::String, nullptr, "IK solver; default ik2BoneSolver for three joints, else FABRIK."},
            {"name", "n", T::String, nullptr, "Chain name; default <effector>_ik."},
            parent,
        },
        .run = createIkChain,
    });

    ctx.registerCommand({
        .name = "puppeteerReverseFoot",
        .doc = "Make a reverse foot (reverseFoot) on an IK chain's ankle, with Puppeteer's foot limits: a control on the sole "
               "that poses the leg and rolls the foot over heel, ball and toe, swivels on the ball and tips over its edges.",
        .positionalDoc = "ankle or its IK chain (default the selection)",
        .flags = {
            {"ankle", "an", T::String, nullptr, "The ankle."},
            {"ball", "b", T::String, nullptr, "Ball joint; default the ankle's first child joint."},
            {"toe", "to", T::String, nullptr, "Toe joint; default the ball's first child joint."},
            {"name", "n", T::String, nullptr, "Name; default <ankle>_reverseFoot."},
            parent,
        },
        .run = createReverseFoot,
    });

    ctx.registerCommand({
        .name = "puppeteerSnapReverseFoot",
        .doc = "Put reverse foot controls on the sole of the current pose with roll, twist and bank zeroed, or (align) "
               "level them with the pose moved into the channels (snapReverseFoot). The pose is kept either way.",
        .positionalDoc = "reverse feet, their chains or ankles (default the selection)",
        .flags = {
            {"align", "al", T::Bool, false, "Level the control instead."},
            {"key", "k", T::Bool, true, "Key the control's channels at the current time."},
        },
        .run = [](const Args& a) -> json { return cmds::execute("snapReverseFoot", forwarded(a)); },
    });

    ctx.registerCommand({
        .name = "puppeteerLooker",
        .doc = "Make a looker (looker) under the joint's character: the joint aims at it, blended by its keyable weight.",
        .positionalDoc = "joint (default the selection)",
        .flags = {
            {"aimAxis", "aa", T::String, "x", "Joint axis that aims."},
            {"upAxis", "ua", T::String, "y", "Joint axis leaned toward upVector."},
            {"upVector", "uv", T::Vec3, nullptr, "World up; default +Y."},
            {"distance", "d", T::Float, 50.0, "How far in front of the joint the looker starts, cm."},
            {"name", "n", T::String, nullptr, "Name; default <joint>_looker."},
            parent,
        },
        .run = [](const Args& a) -> json {
            const std::vector<std::string> objs = objectArgs(a);
            return cmds::execute("looker", withCharacter(graph(), forwarded(a), objs.empty() ? "" : objs[0]));
        },
    });

    ctx.registerCommand({
        .name = "puppeteerSpace",
        .doc = "Give a joint spaces to animate in (addSpace): its keyed space channel picks the parent (0) or one of the "
               "spaces, and its keys are read in that space. Returns the spaceSwitch.",
        .positionalDoc = "joint (default the selection)",
        .flags = {
            {"spaces", "sp", T::List, nullptr, "'world' or nodes to use as spaces, in order."},
        },
        .run = createSpace,
    });

    ctx.registerCommand({
        .name = "puppeteerSwitchSpace",
        .doc = "Switch a joint to another space at a time (switchSpace), keyed stepped, re-keying its translate and rotate "
               "so it keeps its world pose there. Returns the space index.",
        .positionalDoc = "joint (default the selection)",
        .flags = {
            {"space", "sp", T::Any, nullptr, "'parent', a space's name, or its index (0 = parent)."},
            {"time", "t", T::Float, nullptr, "Seconds; default the current time."},
        },
        .run = switchSpace,
    });

    ctx.registerCommand({
        .name = "puppeteerMigrateSpaces",
        .doc = "Turn the scene's puppeteerSpace nodes (older Puppeteer scenes) into base spaces, keeping every key. Runs on "
               "its own when a scene opens. Returns how many were converted.",
        .run = [](const Args&) -> json { return migrateSpaces(graph()); },
    });

    ctx.registerCommand({
        .name = "puppeteerMigrateRig",
        .doc = "Turn older Puppeteer IK chains, reverse feet and lookers into base rig elements. Chains that were in IK get "
               "their IK pose keyed as joint rotations at their keys, and the per-key IK/FK flag goes. Runs on its own when "
               "a scene opens. Returns how many elements were converted.",
        .run = [](const Args&) -> json { return migrateRigElements(graph()); },
    });

    auto migrate = [] {
        Graph& g = Graph::current();
        migrateSpaces(g);
        migrateRigElements(g);
    };
    ctx.subscribe(events::Event::SceneOpened, [migrate](const events::EventData&) { migrate(); });
    migrate();
    return true;
}
} // namespace puppeteer
