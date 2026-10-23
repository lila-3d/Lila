#include "scene/Ik.h"

#include "core/Profile.h"
#include "scene/IkSolver.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lila::scene
{
using namespace rigutil;

namespace
{
constexpr const char* kTwoBone = "ik2BoneSolver";
constexpr const char* kFabrik = "ikFabrikSolver";

glm::dvec3 positionOf(const glm::dmat4& m) { return glm::dvec3(m[3]); }

glm::dmat3 axisAngle(const glm::dvec3& axis, double radians) { return glm::mat3_cast(glm::angleAxis(radians, glm::normalize(axis))); }

/** Shortest rotation taking direction a onto b (both unit). */
glm::dmat3 arc(const glm::dvec3& a, const glm::dvec3& b)
{
    double c = std::clamp(glm::dot(a, b), -1.0, 1.0);
    glm::dvec3 axis = glm::cross(a, b);
    if (glm::length(axis) < 1e-12)
    {
        if (c > 0.0)
            return glm::dmat3(1.0);
        glm::dvec3 any = std::abs(a.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
        return axisAngle(glm::cross(a, any), std::numbers::pi);
    }
    return axisAngle(axis, std::atan2(glm::length(axis), c));
}

glm::dvec3 perpendicular(const glm::dvec3& v, const glm::dvec3& axis)
{
    glm::dvec3 p = v - glm::dot(v, axis) * axis;
    return glm::length(p) > 1e-9 ? glm::normalize(p) : glm::dvec3(0.0);
}

/** The bend direction around dir: toward the pole when there is one, else fkBend, rolled by twist about dir. */
glm::dvec3 bendDirection(const ik::Problem& p, const glm::dvec3& root, const glm::dvec3& dir, const glm::dvec3& fkBend)
{
    glm::dvec3 u = p.pole ? perpendicular(*p.pole - root, dir) : glm::dvec3(0.0);
    if (u == glm::dvec3(0.0))
        u = perpendicular(fkBend, dir);
    if (u == glm::dvec3(0.0))
        u = perpendicular(std::abs(dir.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0), dir);
    if (p.twist != 0.0)
        u = axisAngle(dir, p.twist) * u;
    return u;
}

/** Solved world rotations (one per joint but the end) as rotations relative to each parent's solved frame. */
ik::Solution toLocal(const ik::Problem& p, const std::vector<glm::dmat3>& world)
{
    ik::Solution out;
    glm::dmat3 parent = orthonormal(glm::dmat3(p.parentWorld));
    for (const glm::dmat3& w : world)
    {
        out.push_back(glm::normalize(glm::quat_cast(glm::transpose(parent) * w)));
        parent = w;
    }
    return out;
}

/** Turns each joint by the shortest arc from where its bone points after its parent turned to where it should point. */
std::vector<glm::dmat3> followPositions(const std::vector<glm::dmat4>& world, const std::vector<glm::dvec3>& pos)
{
    std::vector<glm::dmat3> out(world.size() - 1);
    glm::dmat3 inherited(1.0);
    for (size_t i = 0; i + 1 < world.size(); ++i)
    {
        glm::dvec3 fkDir = positionOf(world[i + 1]) - positionOf(world[i]);
        glm::dvec3 newDir = pos[i + 1] - pos[i];
        glm::dmat3 turn = inherited;
        if (glm::length(fkDir) > 1e-12 && glm::length(newDir) > 1e-12)
            turn = arc(glm::normalize(inherited * glm::normalize(fkDir)), glm::normalize(newDir)) * inherited;
        out[i] = turn * orthonormal(glm::dmat3(world[i]));
        inherited = turn;
    }
    return out;
}

/** Analytic two-bone solve (law of cosines), bending toward the pole; exact whenever the target is in reach. */
ik::Solution solveTwoBone(const ik::Problem& p)
{
    LILA_ZONE();

    const glm::dvec3 p0 = positionOf(p.world[0]), p1 = positionOf(p.world[1]), p2 = positionOf(p.world[2]);
    const double a = glm::length(p1 - p0), b = glm::length(p2 - p1);
    const glm::dvec3 toTarget = p.target - p0, fkToEnd = p2 - p0;
    const double d = glm::length(toTarget);
    std::vector<glm::dmat3> world = {orthonormal(glm::dmat3(p.world[0])), orthonormal(glm::dmat3(p.world[1]))};
    if (d < 1e-12 || a < 1e-12 || b < 1e-12 || glm::length(fkToEnd) < 1e-12)
        return toLocal(p, world);

    glm::dvec3 dir = toTarget / d;
    glm::dvec3 fkDir = glm::normalize(fkToEnd);
    glm::dvec3 u = bendDirection(p, p0, dir, p1 - p0);
    glm::dvec3 fkU = perpendicular(p1 - p0, fkDir);
    if (fkU == glm::dvec3(0.0))
        // A straight FK chain has no bend plane; take the one that turns the chain least.
        fkU = perpendicular(arc(dir, fkDir) * u, fkDir);
    glm::dvec3 n = glm::cross(dir, u);
    glm::dmat3 align = glm::dmat3(dir, u, n) * glm::transpose(glm::dmat3(fkDir, fkU, glm::cross(fkDir, fkU)));

    double reach = std::clamp(d, std::abs(a - b), a + b);
    double angleNew = std::acos(std::clamp((a * a + reach * reach - b * b) / (2.0 * a * reach), -1.0, 1.0));
    glm::dvec3 upper = glm::normalize(p1 - p0);
    double angleFk = std::atan2(glm::dot(upper, fkU), glm::dot(upper, fkDir));
    glm::dmat3 turn0 = axisAngle(n, angleNew - angleFk) * align;

    glm::dvec3 elbow = p0 + turn0 * (p1 - p0);
    glm::dvec3 want = p0 + dir * reach - elbow;
    glm::dmat3 turn1 = turn0;
    if (glm::length(want) > 1e-12)
        turn1 = arc(glm::normalize(turn0 * (p2 - p1)), glm::normalize(want)) * turn0;
    return toLocal(p, {turn0 * world[0], turn1 * world[1]});
}

/** Forward And Backward Reaching IK over any number of bones, then swung about the root-end axis toward the pole. */
ik::Solution solveFabrik(const ik::Problem& p)
{
    LILA_ZONE();

    size_t n = p.world.size();
    std::vector<glm::dvec3> pos(n);
    std::vector<double> len(n - 1);
    double reach = 0.0;
    for (size_t i = 0; i < n; ++i)
        pos[i] = positionOf(p.world[i]);
    for (size_t i = 0; i + 1 < n; ++i)
        reach += len[i] = glm::length(pos[i + 1] - pos[i]);
    const glm::dvec3 root = pos[0];
    auto safeDir = [](const glm::dvec3& v) { return glm::length(v) > 1e-12 ? glm::normalize(v) : glm::dvec3(1, 0, 0); };

    if (glm::length(p.target - root) >= reach)
    {
        glm::dvec3 dir = safeDir(p.target - root);
        for (size_t i = 0; i + 1 < n; ++i)
            pos[i + 1] = pos[i] + dir * len[i];
    }
    else
        for (int it = 0; it < p.maxIterations && glm::length(pos[n - 1] - p.target) > p.tolerance; ++it)
        {
            pos[n - 1] = p.target;
            for (size_t i = n - 1; i-- > 0;)
                pos[i] = pos[i + 1] + safeDir(pos[i] - pos[i + 1]) * len[i];
            pos[0] = root;
            for (size_t i = 0; i + 1 < n; ++i)
                pos[i + 1] = pos[i] + safeDir(pos[i + 1] - pos[i]) * len[i];
        }

    glm::dvec3 axis = pos[n - 1] - root;
    if (n > 2 && glm::length(axis) > 1e-12 && (p.pole || p.twist != 0.0))
    {
        axis = glm::normalize(axis);
        glm::dvec3 mid(0.0);
        for (size_t i = 1; i + 1 < n; ++i)
            mid += pos[i] - root;
        glm::dvec3 bend = perpendicular(mid, axis);
        if (bend != glm::dvec3(0.0))
        {
            glm::dvec3 want = bendDirection(p, root, axis, bend);
            glm::dmat3 swing = axisAngle(axis, std::atan2(glm::dot(glm::cross(bend, want), axis), glm::dot(bend, want)));
            for (size_t i = 1; i + 1 < n; ++i)
                pos[i] = root + swing * (pos[i] - root);
        }
    }
    return toLocal(p, followPositions(p.world, pos));
}

AttrDef vec3Attr(std::string name, std::string shortName, glm::dvec3 def, uint32_t flags, std::string doc)
{
    AttrDef a{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = def, .flags = flags, .doc = std::move(doc)};
    for (int i = 0; i < 3; ++i)
    {
        a.childNames[i] = name + char('X' + i);
        a.childShort[i] = shortName.empty() ? "" : shortName + char('x' + i);
    }
    return a;
}

NodeType makeHandleType(const NodeType& base)
{
    const uint32_t hiddenStored = AttrInput | AttrStorable | AttrHidden;
    const uint32_t keyable = AttrInput | AttrStorable | AttrKeyable;
    NodeType t;
    t.name = "ikHandle";
    t.icon = "target-arrow";
    t.base = &base;
    t.dag = true;
    t.doc = "Moves a joint chain's end to the handle's position by solving the joints' rotations (IK).";
    t.ownAttrs = {
        {.name = "ikSolver", .shortName = "ikso", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "Solver id (ik2BoneSolver, ikFabrikSolver, or an extension's). Empty picks 2-bone for three joints, else FABRIK."},
        vec3Attr("poleVector", "pv", glm::dvec3(0.0), keyable, "Direction from the start joint the chain bends toward. Zero keeps the rest bend plane."),
        {.name = "poleAngle", .shortName = "pa", .type = PlugType::Float, .defaultValue = 0.0, .flags = keyable,
         .doc = "Roll of the bend plane about the start-to-handle axis, degrees."},
        {.name = "ikBlend", .shortName = "ikb", .type = PlugType::Float, .defaultValue = 1.0, .flags = keyable,
         .doc = "0 leaves the chain on its FK rotate, 1 is fully solved."},
        {.name = "jointCount", .shortName = "jc", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = hiddenStored,
         .doc = "Joints in the chain, end effector included."},
        {.name = "chainParentMatrix", .shortName = "cpm", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
         .flags = AttrInput | AttrHidden, .doc = "World matrix of the start joint's parent."},
        {.name = "endEffector", .shortName = "ee", .type = PlugType::String, .defaultValue = std::string(), .flags = hiddenStored,
         .doc = "Name of the chain's last joint."},
    };
    for (int i = 0; i < IkHandle::kMaxJoints; ++i)
    {
        std::string n = std::to_string(i);
        t.ownAttrs.push_back({.name = "restLocal" + n, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hiddenStored,
                              .doc = "The joint's FK local matrix at rest."});
        t.ownAttrs.push_back({.name = "orient" + n, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hiddenStored,
                              .doc = "The joint's jointOrient."});
        t.ownAttrs.push_back({.name = "axis" + n, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hiddenStored,
                              .doc = "The joint's rotateAxis."});
        t.ownAttrs.push_back({.name = "rotateOrder" + n, .type = PlugType::Int, .defaultValue = int64_t(0), .flags = hiddenStored});
        t.ownAttrs.push_back(vec3Attr("restRotate" + n, "", glm::dvec3(0.0), hiddenStored, "Rest rotate, the Euler branch solutions stay near."));
        t.ownAttrs.push_back(vec3Attr("solvedRotate" + n, "", glm::dvec3(0.0), AttrOutput | AttrHidden, "Solved rotate for the joint."));
    }
    t.create = [] { return std::make_unique<IkHandle>(); };
    return t;
}

std::string defaultSolver(int joints) { return joints == 3 ? kTwoBone : kFabrik; }

/** Joints from start down to end, start first; empty when end is not below start through joints. */
std::vector<NodeId> chainBetween(Graph& g, NodeId start, NodeId end)
{
    std::vector<NodeId> chain;
    for (NodeId j = end; j != kNoNode && rig::isJoint(*g.node(j)); j = g.parent(j))
    {
        chain.push_back(j);
        if (j == start)
        {
            std::reverse(chain.begin(), chain.end());
            return chain;
        }
    }
    return {};
}

/** The rest pole: the chain's average bend away from the start-to-end line, so a new handle doesn't move anything. */
glm::dvec3 restPole(Graph& g, const std::vector<NodeId>& chain)
{
    g.evaluate();
    glm::dvec3 root = positionOf(worldOf(*g.node(chain.front())));
    glm::dvec3 end = positionOf(worldOf(*g.node(chain.back())));
    if (glm::length(end - root) < 1e-12)
        return glm::dvec3(0.0);
    glm::dvec3 axis = glm::normalize(end - root);
    glm::dvec3 mid(0.0);
    for (size_t i = 1; i + 1 < chain.size(); ++i)
        mid += positionOf(worldOf(*g.node(chain[i]))) - root;
    return clean(perpendicular(mid, axis));
}
} // namespace

void IkHandle::compute(const EvalContext& ctx)
{
    LILA_ZONE();

    Transform::compute(ctx);
    int count = int(std::clamp<int64_t>(as<int64_t>(kJointCount), 0, kMaxJoints));
    if (count < 2)
        return;

    ik::Problem p;
    p.parentWorld = as<glm::dmat4>(kChainParentMatrix);
    glm::dmat4 w = p.parentWorld;
    for (int i = 0; i < count; ++i)
        p.world.push_back(w = w * as<glm::dmat4>(slotLocal(i)));
    p.target = glm::dvec3(as<glm::dmat4>(kWorldMatrix)[3]);
    glm::dvec3 pv = as<glm::dvec3>(kPoleVector);
    if (glm::length(pv) > 1e-12)
        p.pole = positionOf(p.world[0]) + pv;
    p.twist = glm::radians(as<double>(kPoleAngle));
    p.tolerance = 1e-9;
    p.maxIterations = 256;

    std::string id = as<std::string>(kSolver);
    if (id.empty())
        id = defaultSolver(count);
    const ik::Solver* solver = ik::findSolver(id);
    if (!solver || count < solver->minJoints || (solver->maxJoints > 0 && count > solver->maxJoints))
        solver = ik::findSolver(kFabrik);
    if (!solver)
        return;
    ik::Solution sol = solver->solve(p);

    for (int i = 0; i + 1 < count && size_t(i) < sol.size(); ++i)
    {
        // World = parent * orient * R * axis, and the solution is parent^-1 * world, so R = orient^-1 * solution * axis^-1.
        glm::dmat3 orient(as<glm::dmat4>(slotOrient(i))), axis(as<glm::dmat4>(slotAxis(i)));
        glm::dmat3 r = glm::transpose(orient) * glm::mat3_cast(sol[size_t(i)]) * glm::transpose(axis);
        auto order = RotateOrder(std::clamp<int64_t>(as<int64_t>(slotOrder(i)), 0, 5));
        setOutput(slotSolved(i), xformops::closestEuler(r, order, as<glm::dvec3>(slotRest(i))));
    }
}

namespace ik
{
std::vector<NodeId> chainOf(Graph& g, NodeId handle)
{
    Node* h = g.node(handle);
    if (!h || !h->type().isA("ikHandle"))
        return {};
    int count = int(std::clamp<int64_t>(h->as<int64_t>(IkHandle::kJointCount), 0, IkHandle::kMaxJoints));
    std::vector<NodeId> chain;
    for (int i = 0; i + 1 < count; ++i)
    {
        std::vector<PlugRef> dst = g.destinations({handle, IkHandle::slotSolved(i), -1});
        if (dst.empty())
            return {};
        chain.push_back(dst.front().node);
    }
    if (chain.empty())
        return {};
    // The effector is stored by name and told apart from same-named nodes by being the chain's last child.
    Node* end = nullptr;
    for (NodeId id : g.findAll(h->as<std::string>(IkHandle::kEndEffector)))
        if (g.parent(id) == chain.back())
            end = g.node(id);
    if (!end)
    {
        // Renamed: the child whose offset matches the stored rest offset.
        glm::dvec3 want = positionOf(h->as<glm::dmat4>(IkHandle::slotLocal(count - 1)));
        end = nullptr;
        double best = 1e300;
        for (NodeId c : g.children(chain.back()))
            if (rig::isJoint(*g.node(c)))
                if (double d = glm::length(g.node(c)->as<glm::dvec3>(Transform::kTranslate) - want); d < best)
                    best = d, end = g.node(c);
    }
    if (!end)
        return {};
    chain.push_back(end->id());
    return chain;
}

std::vector<NodeId> handlesOf(Graph& g, NodeId joint)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().isA("ikHandle"))
        {
            auto chain = chainOf(g, id);
            if (std::find(chain.begin(), chain.end(), joint) != chain.end())
                out.push_back(id);
        }
    return out;
}

void refresh(Graph& g, NodeId handle)
{
    std::vector<NodeId> chain = chainOf(g, handle);
    if (chain.size() < 2)
        return;
    g.setValue({handle, IkHandle::kJointCount}, int64_t(chain.size()));
    g.setValue({handle, IkHandle::kEndEffector}, g.node(chain.back())->name());
    for (size_t i = 0; i < chain.size(); ++i)
    {
        Node* j = g.node(chain[i]);
        const auto* xf = static_cast<const Transform*>(j);
        int k = int(i);
        // FK only: whatever IK currently blends in must not feed back into the rest layout.
        auto order = RotateOrder(j->as<int64_t>(Transform::kRotateOrder));
        glm::dmat4 local = Transform::localMatrix(*j, eulerToMatrix(j->as<glm::dvec3>(Transform::kRotate), order));
        g.setValue({handle, IkHandle::slotLocal(k)}, local);
        g.setValue({handle, IkHandle::slotOrient(k)}, xf->preRotation());
        g.setValue({handle, IkHandle::slotAxis(k)}, xf->rotateAxisMatrix());
        g.setValue({handle, IkHandle::slotOrder(k)}, j->as<int64_t>(Transform::kRotateOrder));
        g.setValue({handle, IkHandle::slotRest(k)}, j->as<glm::dvec3>(Transform::kRotate));
    }
}
} // namespace ik

namespace
{
Node& jointArg(Graph& g, const std::string& name)
{
    Node& n = nodeNamed(name, "ikHandle");
    if (!rig::isJoint(n))
        throw CommandError("ikHandle: " + name + " is not a joint");
    (void)g;
    return n;
}

json createHandle(const Args& a)
{
    Graph& g = graph();
    std::string startName = a.get<std::string>("startJoint"), endName = a.get<std::string>("endEffector");
    if (startName.empty() || endName.empty())
    {
        std::vector<std::string> joints;
        for (NodeId id : g.selection())
            if (rig::isJoint(*g.node(id)))
                joints.push_back(g.node(id)->path());
        if (joints.size() < 2)
            throw CommandError("ikHandle: give startJoint and endEffector, or select the start and end joints");
        startName = startName.empty() ? joints.front() : startName;
        endName = endName.empty() ? joints.back() : endName;
    }
    NodeId start = jointArg(g, startName).id(), end = jointArg(g, endName).id();
    std::vector<NodeId> chain = chainBetween(g, start, end);
    if (chain.size() < 2)
        throw CommandError("ikHandle: " + endName + " is not a joint below " + startName);
    if (chain.size() > size_t(IkHandle::kMaxJoints))
        throw CommandError("ikHandle: chains are limited to " + std::to_string(IkHandle::kMaxJoints) + " joints");
    for (size_t i = 0; i + 1 < chain.size(); ++i)
        if (g.source({chain[i], Joint::kSolvedRotate, -1}))
            throw CommandError("ikHandle: " + g.node(chain[i])->path() + " is already solved by another handle");

    std::string solver = a.get<std::string>("solver");
    if (!solver.empty())
    {
        const ik::Solver* s = ik::findSolver(solver);
        if (!s)
            throw CommandError("ikHandle: no IK solver named '" + solver + "'");
        int n = int(chain.size());
        if (n < s->minJoints || (s->maxJoints > 0 && n > s->maxJoints))
            throw CommandError("ikHandle: " + solver + " does not take a chain of " + std::to_string(n) + " joints");
    }

    g.evaluate();
    glm::dvec3 target = positionOf(worldOf(*g.node(end)));
    Node* h = guarded([&] { return g.createNode(*types::find("ikHandle"), a.get<std::string>("name", "ikHandle"), kNoNode); });
    NodeId id = h->id();
    g.setValue({id, Transform::kTranslate}, target);
    g.setValue({id, IkHandle::kSolver}, solver);
    g.setValue({id, IkHandle::kPoleVector}, a.has("poleVector") ? vec3Of(a.flags["poleVector"]) : restPole(g, chain));
    g.setValue({id, IkHandle::kPoleAngle}, a.get<double>("poleAngle", 0.0));
    g.setValue({id, IkHandle::kJointCount}, int64_t(chain.size()));
    g.setValue({id, IkHandle::kEndEffector}, g.node(end)->name());
    if (NodeId parent = g.parent(start); parent != kNoNode)
        g.connect({parent, Transform::kWorldMatrix, -1}, {id, IkHandle::kChainParentMatrix, -1});
    for (size_t i = 0; i + 1 < chain.size(); ++i)
    {
        g.connect({id, IkHandle::slotSolved(int(i)), -1}, {chain[i], Joint::kSolvedRotate, -1});
        g.connect({id, IkHandle::kIkBlend, -1}, {chain[i], Joint::kIkBlend, -1});
    }
    ik::refresh(g, id);
    g.select({id});
    return g.node(id)->path();
}

void registerIkCommandsImpl()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "ikHandle",
        .doc = "Create an IK handle that solves the chain from startJoint down to endEffector (or the first and last "
               "selected joints); the handle starts on the end joint, so nothing moves. The joints keep their keyable "
               "rotate as FK and the handle's ikBlend crossfades to the solution. With refresh=True, re-reads the rest "
               "layout of the given handles' chains after joints were edited. Returns the handle.",
        .positionalDoc = "handles (refresh only)",
        .flags = {
            {"startJoint", "sj", T::String, nullptr, "First joint of the chain."},
            {"endEffector", "ee", T::String, nullptr, "Last joint of the chain; the one placed on the handle."},
            {"solver", "sol", T::String, "", "IK solver id; empty picks ik2BoneSolver for three joints, else ikFabrikSolver.",
             {"", kTwoBone, kFabrik}},
            {"name", "n", T::String, nullptr, "Handle name."},
            {"poleVector", "pv", T::Vec3, nullptr, "Bend direction from the start joint; defaults to the chain's current bend."},
            {"poleAngle", "pa", T::Float, nullptr, "Roll of the bend plane, degrees."},
            {"refresh", "rf", T::Bool, false, "Re-read the rest layout of the given handles' chains."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            if (!a.get<bool>("refresh"))
                return createHandle(a);
            Graph& g = graph();
            json out = json::array();
            for (const std::string& name : a.objects())
            {
                Node& h = nodeNamed(name, "ikHandle");
                if (!h.type().isA("ikHandle"))
                    throw CommandError("ikHandle: " + name + " is not an ikHandle");
                ik::refresh(g, h.id());
                out.push_back(name);
            }
            return out;
        },
    });
}
} // namespace

void registerIkCommands()
{
    if (ops::find("ikHandle"))
        return;
    registerIkCommandsImpl();
}

void registerIkTypes()
{
    if (!ik::findSolver(kTwoBone))
        ik::registerSolver({.name = kTwoBone, .doc = "Exact two-bone solve for limbs (three joints), bending toward the pole vector.",
                            .minJoints = 3, .maxJoints = 3, .solve = solveTwoBone});
    if (!ik::findSolver(kFabrik))
        ik::registerSolver({.name = kFabrik, .doc = "Iterative FABRIK solve for chains of any length; the bend swings toward the pole vector.",
                            .minJoints = 2, .solve = solveFabrik});
    if (types::find("ikHandle"))
        return;
    const NodeType* transform = types::find("transform");
    if (!transform)
        throw std::logic_error("registerIkTypes needs the built-in types first");
    types::registerType(makeHandleType(*transform));
}
} // namespace lila::scene
