// Rig element commands: making IK chains, reverse feet, lookers and twist drivers, posing through their controllers,
// locks, and the clean bake onto a deform-only skeleton.

#include "scene/RigElements.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"
#include "scene/UnrealCheck.h"

#include <algorithm>

namespace lila::scene::rigel
{
namespace
{
using rigutil::graph;
using rigutil::guarded;
using rigutil::nodeNamed;
using rigutil::vec3Of;

std::vector<std::string> objectArgs(const Args& a)
{
    std::vector<std::string> out;
    for (const std::string& s : a.objects())
        if (!s.empty())
            out.push_back(s);
    return out;
}

Node& jointNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!rig::isJoint(n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a joint");
    return n;
}

NodeId parentArg(const Args& a, const char* cmd)
{
    const std::string p = a.get<std::string>("parent", "");
    return p.empty() ? kNoNode : nodeNamed(p, cmd).id();
}

/** Chains from the command's objects: chains, their controllers or joints in them. */
std::vector<NodeId> chainArgs(Graph& g, const Args& a, const char* command)
{
    std::vector<NodeId> out;
    for (const std::string& s : objectArgs(a))
        if (NodeId c = chainFrom(g, nodeNamed(s, command).id()); c && std::find(out.begin(), out.end(), c) == out.end())
            out.push_back(c);
    if (out.empty())
        throw CommandError(std::string(command) + ": give IK chains, their controllers or joints in them");
    return out;
}

std::vector<NodeId> footArgs(Graph& g, const Args& a, const char* command)
{
    std::vector<NodeId> out;
    for (NodeId c : chainArgs(g, a, command))
        if (NodeId f = footForChain(g, c))
            out.push_back(f);
    if (out.empty())
        throw CommandError(std::string(command) + ": give reverse feet, their chains or ankles");
    return out;
}

json names(Graph& g, const std::vector<NodeId>& ids)
{
    json out = json::array();
    for (NodeId id : ids)
        out.push_back(g.node(id)->path());
    return out;
}

template <class F> auto elementGuard(const char* cmd, F&& f)
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

json createChainCmd(const Args& a)
{
    Graph& g = graph();
    std::vector<std::string> objs = objectArgs(a);
    const std::string start = a.get<std::string>("startJoint", objs.size() >= 1 ? objs[0] : "");
    const std::string end = a.get<std::string>("endJoint", objs.size() >= 2 ? objs[1] : "");
    if (start.empty() || end.empty())
        throw CommandError("ikChain: give the start joint and the end effector");
    ChainOptions o{.name = a.get<std::string>("name", ""), .solver = a.get<std::string>("solver", ""), .parent = parentArg(a, "ikChain")};
    if (a.has("poleVector"))
        o.poleVector = vec3Of(a.flags["poleVector"]);
    const NodeId s = jointNamed(start, "ikChain").id(), e = jointNamed(end, "ikChain").id();
    const NodeId c = elementGuard("ikChain", [&] { return createIkChain(g, s, e, o); });
    g.select({controllerOf(g, c)});
    return g.node(c)->path();
}

json createFootCmd(const Args& a)
{
    Graph& g = graph();
    std::vector<std::string> objs = objectArgs(a);
    const std::string ankle = a.get<std::string>("ankle", objs.empty() ? "" : objs[0]);
    if (ankle.empty())
        throw CommandError("reverseFoot: give the ankle, or its IK chain");
    const NodeId chain = chainFrom(g, nodeNamed(ankle, "reverseFoot").id());
    if (!chain)
        throw CommandError("reverseFoot: the ankle needs an IK chain first (ikChain)");
    FootOptions o{.name = a.get<std::string>("name", ""), .parent = parentArg(a, "reverseFoot")};
    if (a.has("ball"))
        o.ball = jointNamed(a.get<std::string>("ball"), "reverseFoot").id();
    if (a.has("toe"))
        o.toe = jointNamed(a.get<std::string>("toe"), "reverseFoot").id();
    if (a.has("pivots"))
    {
        const json& p = a.flags["pivots"];
        std::array<glm::dvec3, 5> pv;
        const char* keys[5] = {"heel", "ball", "toe", "inner", "outer"};
        for (int i = 0; i < 5; ++i)
        {
            if (!p.is_object() || !p.contains(keys[i]))
                throw CommandError("reverseFoot: pivots needs heel, ball, toe, inner and outer world positions");
            pv[size_t(i)] = vec3Of(p[keys[i]]);
        }
        o.pivots = pv;
    }
    const NodeId f = elementGuard("reverseFoot", [&] { return createReverseFoot(g, chain, o); });
    g.select({f});
    return g.node(f)->path();
}

json lockCmd(const Args& a)
{
    Graph& g = graph();
    const double t = a.get<double>("time", g.time());
    json out = json::array();
    for (NodeId c : chainArgs(g, a, "ikLock"))
    {
        const bool on = a.has("state") ? a.get<bool>("state") : !locked(g, c, t);
        setLock(g, c, t, on);
        out.push_back({{"chain", g.node(c)->path()}, {"locked", on}});
    }
    return out;
}

json footPivotCmd(const Args& a)
{
    Graph& g = graph();
    static const char* pivots[] = {"heel", "ball", "toe", "inner", "outer"};
    const std::string which = a.get<std::string>("pivot", "");
    if (std::find(std::begin(pivots), std::end(pivots), which) == std::end(pivots))
        throw CommandError("footPivot: pivot is heel, ball, toe, inner or outer");
    if (!a.has("position"))
        throw CommandError("footPivot: give the world position");
    json out = json::array();
    for (NodeId f : footArgs(g, a, "footPivot"))
    {
        g.evaluate();
        // Kept on the sole: only where it sits across the foot counts.
        const glm::dmat4 w = g.node(f)->as<glm::dmat4>(Transform::kWorldMatrix);
        glm::dvec3 local(glm::inverse(w) * glm::dvec4(vec3Of(a.flags["position"]), 1.0));
        local.y = 0.0;
        g.setValue({f, g.node(f)->type().attrIndex((which + "Pivot").c_str()), -1}, local);
        out.push_back(g.node(f)->path());
    }
    return out;
}

json poseCmd(const Args& a)
{
    Graph& g = graph();
    std::vector<NodeId> controllers;
    for (NodeId c : chainArgs(g, a, "ikPose"))
        controllers.push_back(controllerOf(g, c));
    const std::vector<NodeId> posed = poseControllers(g, controllers, [&] {
        for (NodeId c : controllers)
        {
            const glm::dmat4 w = g.node(c)->as<glm::dmat4>(Transform::kWorldMatrix);
            glm::dmat3 r(w);
            glm::dvec3 p(w[3]);
            if (a.has("rotation"))
            {
                const glm::dmat3 want(eulerToMatrix(vec3Of(a.flags["rotation"]), RotateOrder::XYZ));
                for (int i = 0; i < 3; ++i)
                    r[i] = want[i] * glm::length(glm::dvec3(w[i]));
            }
            if (a.has("rotateBy"))
                r = glm::dmat3(eulerToMatrix(vec3Of(a.flags["rotateBy"]), RotateOrder::XYZ)) * r;
            if (a.has("position"))
                p = vec3Of(a.flags["position"]);
            if (a.has("moveBy"))
                p += vec3Of(a.flags["moveBy"]);
            glm::dmat4 m(r);
            m[3] = glm::dvec4(p, 1.0);
            const glm::dmat4 parent = g.node(c)->as<glm::dmat4>(Transform::kParentMatrix);
            Transform::setLocalMatrix(g, c, glm::inverse(parent) * m);
        }
    });
    return names(g, posed);
}

json bakeCmd(const Args& a)
{
    Graph& g = graph();
    std::vector<NodeId> roots;
    for (const std::string& s : objectArgs(a))
        roots.push_back(jointNamed(s, "bakeToDeformSkeleton").id());
    if (roots.empty())
        for (NodeId id : g.selection())
            if (rig::isJoint(*g.node(id)))
                roots.push_back(id);
    if (roots.empty())
        throw CommandError("bakeToDeformSkeleton: give a joint of each skeleton to bake");
    const anim::Playback& pb = anim::playback();
    BakeOptions o{.start = a.get<double>("start", pb.minTime), .end = a.get<double>("end", pb.maxTime), .fps = a.get<double>("fps", pb.fps),
                  .suffix = a.get<std::string>("suffix", "_bake"), .scaleSafe = a.get<bool>("scaleSafe", true)};
    if (o.end < o.start)
        throw CommandError("bakeToDeformSkeleton: end is before start");
    return names(g, bakeToDeformSkeleton(g, roots, o));
}
} // namespace

void registerCommands()
{
    uecheck::registerCommands();
    if (ops::find("ikChain"))
        return;
    using T = ArgType;
    const FlagDef parentFlag{"parent", "p", T::String, nullptr, "Node to put the element under, such as a character."};

    ops::registerCommand({
        .name = "ikChain",
        .doc = "Make an IK chain from a start joint to an end effector, solved by a base ikHandle. The joints keep their keyed "
               "rotate: grabbing the controller solves the chain and writes (keys) the rotations, and a locked key holds the "
               "controller until the chain's next key. Returns the chain.",
        .positionalDoc = "startJoint endJoint",
        .flags = {
            {"startJoint", "sj", T::String, nullptr, "First joint."},
            {"endJoint", "ej", T::String, nullptr, "End effector."},
            {"solver", "sol", T::String, nullptr, "IK solver; default ik2BoneSolver for three joints, else FABRIK."},
            {"name", "n", T::String, nullptr, "Chain name; default <effector>_ik."},
            {"poleVector", "pv", T::Vec3, nullptr, "Bend direction from the start joint; default the chain's current bend."},
            parentFlag,
        },
        .run = createChainCmd,
    });

    ops::registerCommand({
        .name = "reverseFoot",
        .doc = "Make a reverse foot on an IK chain's ankle: a control on the sole that poses the leg and rolls the foot over "
               "heel, ball and toe (roll), swivels on the ball (twist) and tips over its edges (bank). Pivots come from the "
               "skinned sole when the foot has a mesh, else from the joints, unless given. Returns it.",
        .positionalDoc = "ankle or its IK chain",
        .flags = {
            {"ankle", "an", T::String, nullptr, "The ankle."},
            {"ball", "b", T::String, nullptr, "Ball joint; default the ankle's first child joint."},
            {"toe", "to", T::String, nullptr, "Toe joint; default the ball's first child joint."},
            {"name", "n", T::String, nullptr, "Name; default <ankle>_reverseFoot."},
            {"pivots", "pvs", T::Any, nullptr, "World positions {heel, ball, toe, inner, outer}, dropped onto the sole."},
            parentFlag,
        },
        .run = createFootCmd,
    });

    ops::registerCommand({
        .name = "snapReverseFoot",
        .doc = "Put reverse foot controls on the sole of the current pose with roll, twist and bank zeroed, or (align) level "
               "them with the pose moved into the channels. The pose is kept either way.",
        .positionalDoc = "reverse feet, their chains or ankles",
        .flags = {
            {"align", "al", T::Bool, false, "Level the control instead."},
            {"key", "k", T::Bool, true, "Key the control's channels at the current time."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            for (NodeId f : footArgs(g, a, "snapReverseFoot"))
            {
                elementGuard("snapReverseFoot", [&] {
                    snapReverseFoot(g, f, a.get<bool>("align"));
                    return 0;
                });
                const std::string n = g.node(f)->path();
                if (a.get<bool>("key", true))
                    ops::execute("setKeyframe", {{"args", {n + ".translate", n + ".rotate", n + ".roll", n + ".twist", n + ".bank"}}});
                out.push_back(n);
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "footChannels",
        .doc = "Roll, twist and bank reverse feet by degrees (or to them, with absolute) within each foot's limits, posing "
               "the leg's joints (keyed when the leg is animated).",
        .positionalDoc = "reverse feet, their chains or ankles",
        .flags = {
            {"roll", "r", T::Float, nullptr, "Roll: positive onto the toe (heel up), negative onto the heel."},
            {"twist", "tw", T::Float, nullptr, "Twist around the ball."},
            {"bank", "b", T::Float, nullptr, "Bank: positive onto the outer edge."},
            {"absolute", "abs", T::Bool, false, "Set the values instead of adding to them."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<NodeId> feet = footArgs(g, a, "footChannels");
            auto opt = [&](const char* k) { return a.has(k) ? std::optional<double>(a.get<double>(k)) : std::nullopt; };
            poseControllers(g, feet, [&] {
                for (NodeId f : feet)
                    addFootChannels(g, f, opt("roll"), opt("twist"), opt("bank"), a.get<bool>("absolute"));
            });
            return names(g, feet);
        },
    });

    ops::registerCommand({
        .name = "footPivot",
        .doc = "Move one of reverse feet's pivots (heel, ball, toe, inner or outer edge) to a world position on the sole.",
        .positionalDoc = "reverse feet, their chains or ankles",
        .flags = {
            {"pivot", "pv", T::String, nullptr, "Which pivot.", {"heel", "ball", "toe", "inner", "outer"}},
            {"position", "p", T::Vec3, nullptr, "World position; its height is dropped onto the sole."},
        },
        .run = footPivotCmd,
    });

    ops::registerCommand({
        .name = "ikPole",
        .doc = "Swing IK chains' pole by degrees (the knee or elbow turns around the line from hip to foot), posing the "
               "joints. turn also turns the controller with it, so the foot or hand follows the knee or elbow.",
        .positionalDoc = "IK chains, their controllers or joints in them",
        .flags = {
            {"by", "by", T::Float, 0.0, "Degrees added to the pole angle."},
            {"turn", "tu", T::Bool, false, "Turn the controller with the pole."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<NodeId> cs = chainArgs(g, a, "ikPole");
            poseControllers(g, cs, [&] {
                for (NodeId c : cs)
                    elementGuard("ikPole", [&] {
                        swingPole(g, c, a.get<double>("by", 0.0), a.get<bool>("turn", false));
                        return 0;
                    });
            });
            return names(g, cs);
        },
    });

    ops::registerCommand({
        .name = "ikPose",
        .doc = "Put IK controllers at a world position and rotation (or move and turn them by an amount) and pose their "
               "chains' joints there, keyed when the chain is animated. Returns the chains.",
        .positionalDoc = "IK chains, their controllers or joints in them",
        .flags = {
            {"position", "p", T::Vec3, nullptr, "World position, cm."},
            {"rotation", "ro", T::Vec3, nullptr, "World rotation, degrees XYZ."},
            {"moveBy", "mb", T::Vec3, nullptr, "World offset, cm."},
            {"rotateBy", "rb", T::Vec3, nullptr, "World rotation added, degrees XYZ."},
        },
        .run = poseCmd,
    });

    ops::registerCommand({
        .name = "ikMatch",
        .doc = "Put IK controllers (and their poles) onto the joints' current pose, which grabbing one also does. Returns the "
               "chains.",
        .positionalDoc = "IK chains, their controllers or joints in them",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<NodeId> cs = chainArgs(g, a, "ikMatch");
            for (NodeId c : cs)
                matchController(g, c);
            return names(g, cs);
        },
    });

    ops::registerCommand({
        .name = "ikLock",
        .doc = "Lock (plant) or unlock the key at a time on IK and reverse foot controllers: a locked key holds the "
               "controller where it is, and the solver holds the hand or foot there, until the chain's next key. Toggles "
               "without state. Returns each chain's new state.",
        .positionalDoc = "IK chains, their controllers or joints in them",
        .flags = {
            {"time", "t", T::Float, nullptr, "Seconds; default the current time."},
            {"state", "s", T::Bool, nullptr, "Locked or not; default the opposite of now."},
        },
        .run = lockCmd,
    });

    ops::registerCommand({
        .name = "looker",
        .doc = "Make a looker: the joint aims at it (aimAxis at the looker, upAxis toward upVector), blended by its keyable "
               "weight. Returns it.",
        .positionalDoc = "joint",
        .flags = {
            {"aimAxis", "aa", T::String, "x", "Joint axis that aims."},
            {"upAxis", "ua", T::String, "y", "Joint axis leaned toward upVector."},
            {"upVector", "uv", T::Vec3, nullptr, "World up; default +Y."},
            {"distance", "d", T::Float, 50.0, "How far in front of the joint the looker starts, cm.", {}, {}, 0.0},
            {"weight", "w", T::Float, 1.0, "Blend of the aim over the joint's own rotation.", {}, {}, 0.0, 1.0},
            {"name", "n", T::String, nullptr, "Name; default <joint>_looker."},
            parentFlag,
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<std::string> objs = objectArgs(a);
            if (objs.empty())
                throw CommandError("looker: give the joint that looks");
            LookerOptions o{.name = a.get<std::string>("name", ""), .aimAxis = a.get<std::string>("aimAxis", "x"),
                            .upAxis = a.get<std::string>("upAxis", "y"), .distance = a.get<double>("distance", 50.0),
                            .weight = a.get<double>("weight", 1.0), .parent = parentArg(a, "looker")};
            if (a.has("upVector"))
                o.upVector = vec3Of(a.flags["upVector"]);
            const NodeId j = jointNamed(objs.front(), "looker").id();
            const NodeId l = elementGuard("looker", [&] { return createLooker(g, j, o); });
            g.select({l});
            return g.node(l)->path();
        },
    });

    ops::registerCommand({
        .name = "twistDriver",
        .doc = "Drive twist joints from another joint's twist about the bone: a twist joint under the upper arm or thigh "
               "counters its parent's twist (source = the parent), one under the forearm or calf takes the hand's or foot's "
               "(source = that joint). The default weight is how far along the bone the twist joint sits. Returns the drivers.",
        .positionalDoc = "twist joints",
        .flags = {
            {"source", "src", T::String, nullptr, "Joint whose twist is read; default the twist joint's parent."},
            {"weight", "w", T::Float, nullptr, "Share of the twist; negative counters it."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<std::string> objs = objectArgs(a);
            if (objs.empty())
                throw CommandError("twistDriver: give the twist joints");
            json out = json::array();
            for (const std::string& s : objs)
            {
                TwistOptions o;
                if (a.has("source"))
                    o.source = jointNamed(a.get<std::string>("source"), "twistDriver").id();
                if (a.has("weight"))
                    o.weight = a.get<double>("weight");
                const NodeId j = jointNamed(s, "twistDriver").id();
                out.push_back(g.node(elementGuard("twistDriver", [&] { return createTwistDriver(g, j, o); }))->path());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "bakeToDeformSkeleton",
        .doc = "Make a clean copy of the deform joints (names plus suffix, hierarchy closed over non-deform joints) keyed every "
               "frame with the evaluated world transform of each original, so IK, spaces, layers, constraints and physics end "
               "up as plain translate, rotate and scale keys, as exporters write them. Returns the copy's top joints.",
        .positionalDoc = "a joint of each skeleton (default the selection)",
        .flags = {
            {"start", "st", T::Float, nullptr, "Seconds; default the playback start."},
            {"end", "et", T::Float, nullptr, "Seconds; default the playback end."},
            {"fps", "fps", T::Float, nullptr, "Frames per second; default the scene's.", {}, {}, 1.0},
            {"suffix", "sfx", T::String, "_bake", "Added to each copied joint's name."},
            {"scaleSafe", "ss", T::Bool, true,
             "Where a joint with deform children scales non-uniformly, write it as an unscaled driver (name_drv) its children "
             "follow plus a leaf carrying the scale, so Unreal matches."},
        },
        .run = bakeCmd,
    });
}
} // namespace lila::scene::rigel
