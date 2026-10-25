// What the Spinner commits, as commands so a drag is one undo step and scripts and agents can do the same: rotate
// with or without the children inheriting it, toon scale, aim moves, and the breakdown slider.

#include "Puppeteer.h"
#include "gizmo/Spinner.h"
#include "rig/Rig.h"

#include <algorithm>
#include <map>

using namespace lila;
using namespace lila::scene;

namespace puppeteer
{
namespace
{
constexpr double kTimeEps = 1e-6;

Graph& graph() { return Graph::current(); }

int depthOf(Graph& g, NodeId id)
{
    int d = 0;
    for (NodeId cur = g.parent(id); cur; cur = g.parent(cur))
        ++d;
    return d;
}

bool isTransform(const Node& n) { return n.type().isA("transform"); }

/** The command's transforms, parents before children. */
std::vector<NodeId> transformArgs(Graph& g, const Args& a, const char* command)
{
    std::vector<NodeId> out;
    for (const std::string& s : objectArgs(a))
    {
        Node& n = nodeNamed(g, s, command);
        if (isTransform(n) && std::find(out.begin(), out.end(), n.id()) == out.end())
            out.push_back(n.id());
    }
    if (out.empty())
        throw CommandError(std::string(command) + ": give transforms or joints");
    std::stable_sort(out.begin(), out.end(), [&](NodeId x, NodeId y) { return depthOf(g, x) < depthOf(g, y); });
    return out;
}

/** Children of the nodes that aren't among them themselves. */
std::vector<NodeId> outsideChildren(Graph& g, const std::vector<NodeId>& nodes)
{
    std::vector<NodeId> out;
    for (NodeId n : nodes)
        for (NodeId c : g.children(n))
            if (Node* cn = g.node(c); cn && isTransform(*cn) && std::find(nodes.begin(), nodes.end(), c) == nodes.end())
                out.push_back(c);
    return out;
}

glm::dvec3 centerOf(Graph& g, const std::vector<NodeId>& nodes)
{
    glm::dvec3 c(0.0);
    for (NodeId n : nodes)
        c += glm::dvec3(worldOf(g, n)[3]);
    return c / double(std::max<size_t>(nodes.size(), 1));
}

json rotateNodes(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> nodes = transformArgs(g, a, "puppeteerRotate");
    if (!a.has("axis"))
        throw CommandError("puppeteerRotate: give the world axis");
    glm::dvec3 axis = jsonVec(a.flags["axis"]);
    if (glm::length(axis) < 1e-12)
        throw CommandError("puppeteerRotate: the axis is zero");
    axis = glm::normalize(axis);
    const glm::dmat3 r = glm::dmat3(glm::rotate(glm::dmat4(1.0), math::radians(a.get<double>("angle", 0.0)), axis));
    const bool center = a.get<std::string>("pivot", "each") == "center";
    const bool keepChildren = a.get<bool>("inherit", false);
    g.evaluate();
    const glm::dvec3 c = centerOf(g, nodes);
    std::map<NodeId, glm::dmat4> before;
    const std::vector<NodeId> children = keepChildren ? outsideChildren(g, nodes) : std::vector<NodeId>{};
    for (NodeId n : nodes)
        before[n] = worldOf(g, n);
    for (NodeId n : children)
        before[n] = worldOf(g, n);
    auto turn = [&] {
        for (NodeId n : nodes)
        {
            // A controller turns from where it sits on the pose, which posing it first put it on.
            if (lila::scene::rigel::chainOfController(g, n))
                before[n] = worldOf(g, n);
            // A node under another one being turned has already turned with it.
            bool carried = false;
            for (NodeId p = g.parent(n); p && !carried; p = g.parent(p))
                carried = std::find(nodes.begin(), nodes.end(), p) != nodes.end();
            if (carried)
                continue;
            setWorldRotation(g, n, r * math::rotationOf(before[n]));
            if (center)
                setWorldPosition(g, n, c + r * (glm::dvec3(before[n][3]) - c));
        }
        for (NodeId n : children)
            setWorldRotation(g, n, math::rotationOf(before[n]));
    };
    // Controllers among them pose their chains' joints.
    lila::scene::rigel::poseControllers(g, nodes, turn);
    return int(nodes.size());
}

json scaleNodes(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> nodes = transformArgs(g, a, "puppeteerScale");
    const glm::dvec3 by = a.has("by") ? jsonVec(a.flags["by"]) : glm::dvec3(1.0);
    const bool compensate = a.get<bool>("compensate", false);
    const std::vector<NodeId> children = compensate ? outsideChildren(g, nodes) : std::vector<NodeId>{};
    std::map<NodeId, glm::dvec3> childScale;
    for (NodeId c : children)
        childScale[c] = vecOf(g, c, Transform::kScale);
    for (NodeId n : nodes)
        g.setValue({n, Transform::kScale, -1}, vecOf(g, n, Transform::kScale) * by);
    for (NodeId c : children)
    {
        const glm::dmat3 local = orientOf(g, c) * glm::dmat3(eulerToMatrix(vecOf(g, c, Transform::kRotate), rotateOrderOf(g, c)));
        g.setValue({c, Transform::kScale, -1}, math::compensatedScale(childScale[c], by, local));
    }
    return int(nodes.size());
}

bool isJointNode(Graph& g, NodeId id) { return id && g.node(id) && rig::isJoint(*g.node(id)); }

/** Sets a joint's jointOrient so its world rotation becomes world, its rotate untouched. Records undo. */
void setOrientForWorld(Graph& g, NodeId id, const glm::dmat3& world)
{
    const glm::dmat3 parent = math::rotationOf(parentWorldOf(g, id));
    const glm::dmat3 r(eulerToMatrix(vecOf(g, id, Transform::kRotate), rotateOrderOf(g, id)));
    const glm::dmat3 axis(eulerToMatrix(vecOf(g, id, Transform::kRotateAxis), RotateOrder::XYZ));
    const glm::dmat3 orient = glm::transpose(parent) * world * glm::transpose(axis) * glm::transpose(r);
    g.setValue({id, Joint::kJointOrient, -1}, matrixToEuler(orient, RotateOrder::XYZ));
}

NodeId firstChildJoint(Graph& g, NodeId id)
{
    for (NodeId c : g.children(id))
        if (isJointNode(g, c))
            return c;
    return kNoNode;
}

/**
 * Sticky aim: turns a joint's orient so its aim axis (the local axis nearest the target now) points at target, its
 * children staying where they are in the world.
 */
void reaim(Graph& g, NodeId joint, const glm::dvec3& target, bool sticky2nd, bool keepDirection)
{
    g.evaluate();
    const glm::dmat4 w = worldOf(g, joint);
    const glm::dvec3 pos(w[3]);
    const glm::dvec3 dir = target - pos;
    if (glm::length(dir) < 1e-9)
        return;
    const glm::dmat3 rot = math::rotationOf(w);
    const glm::dvec3 local = glm::transpose(rot) * glm::normalize(dir);
    int k = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(local[i]) > std::abs(local[k]))
            k = i;
    glm::dvec3 primary(0.0), secondary(0.0);
    primary[k] = local[k] < 0.0 ? -1.0 : 1.0;
    secondary[(k + 1) % 3] = 1.0;
    std::optional<glm::dvec3> plane;
    if (sticky2nd)
        if (NodeId p = g.parent(joint); isJointNode(g, p))
        {
            // The chain's plane: the secondary axis lies in it, across the aim, on the side away from the parent.
            const glm::dvec3 back = glm::dvec3(worldOf(g, p)[3]) - pos;
            const glm::dvec3 a = glm::normalize(dir);
            plane = -(back - a * glm::dot(back, a));
        }
    const glm::dmat3 aimed = math::stickyAimFrame(primary, secondary, dir, rot * secondary, plane ? &*plane : nullptr, keepDirection);
    std::vector<std::pair<NodeId, glm::dmat4>> children;
    for (NodeId c : g.children(joint))
        if (Node* cn = g.node(c); cn && cn->type().isA("transform"))
            children.push_back({c, worldOf(g, c)});
    setOrientForWorld(g, joint, aimed);
    for (const auto& [c, cw] : children)
    {
        setWorldPosition(g, c, glm::dvec3(cw[3]));
        if (isJointNode(g, c))
            setOrientForWorld(g, c, math::rotationOf(cw));
        else
            setWorldRotation(g, c, math::rotationOf(cw));
    }
}

json moveNodes(const Args& a)
{
    Graph& g = graph();
    const glm::dvec3 by = a.has("by") ? jsonVec(a.flags["by"]) : glm::dvec3(0.0);
    const bool sticky = a.get<bool>("stickyAim", false);
    std::vector<NodeId> nodes = transformArgs(g, a, "puppeteerMove");
    if (a.get<bool>("fullBody", false))
    {
        const std::vector<NodeId> posed = fullBodyMove(g, nodes, by, a.get<bool>("autoPose", false));
        std::erase_if(nodes, [&](NodeId n) { return std::find(posed.begin(), posed.end(), n) != posed.end(); });
        if (nodes.empty())
            return int(posed.size());
    }
    if (!a.get<bool>("aim", false))
    {
        g.evaluate();
        if (a.get<bool>("keepChildren", false))
            for (NodeId n : nodes)
                rig::moveNodeTo(g, n, glm::dvec3(worldOf(g, n)[3]) + by, true);
        else
        {
            json names = json::array();
            for (NodeId n : nodes)
                names.push_back(g.node(n)->path());
            ops::execute("move", {{"args", names}, {"by", vecJson(by)}, {"space", "world"}});
        }
        if (sticky)
            for (NodeId n : nodes)
            {
                if (!isJointNode(g, n))
                    continue;
                g.evaluate();
                const glm::dvec3 at(worldOf(g, n)[3]);
                if (NodeId p = g.parent(n); isJointNode(g, p))
                    reaim(g, p, at, a.get<bool>("sticky2nd", false), a.get<bool>("keepDirection", false));
                if (NodeId c = firstChildJoint(g, n))
                    reaim(g, n, glm::dvec3(worldOf(g, c)[3]), a.get<bool>("sticky2nd", false), a.get<bool>("keepDirection", false));
            }
        return int(nodes.size());
    }
    // Sticky aim: the parent turns to point at where the node is dragged, keeping the bone's length.
    g.evaluate();
    for (NodeId n : nodes)
    {
        const NodeId p = g.parent(n);
        Node* pn = p ? g.node(p) : nullptr;
        if (!pn || !isTransform(*pn))
            continue;
        const glm::dvec3 pivot(worldOf(g, p)[3]);
        const glm::dvec3 from = glm::dvec3(worldOf(g, n)[3]) - pivot;
        const glm::dmat3 turn = math::aimDelta(from, from + by);
        setWorldRotation(g, p, turn * math::rotationOf(worldOf(g, p)));
    }
    return int(nodes.size());
}

/** The node's keyed TRS channels, by attr. */
std::vector<std::pair<int, std::string>> animatedChannels(const std::string& node)
{
    std::vector<std::pair<int, std::string>> out;
    for (auto [attr, name] : {std::pair{int(Transform::kTranslate), "translate"}, {int(Transform::kRotate), "rotate"}, {int(Transform::kScale), "scale"}})
    {
        const json n = ops::execute("keyframe", {{"args", {node + "." + name}}, {"query", true}, {"keyCount", true}});
        if (n.is_number() && n.get<int>() > 0)
            out.push_back({attr, name});
    }
    return out;
}

glm::dquat quatOf(Graph& g, NodeId id, const glm::dvec3& euler) { return glm::quat_cast(glm::dmat3(eulerToMatrix(euler, rotateOrderOf(g, id)))); }

json breakdownCommand(const Args& a)
{
    Graph& g = graph();
    const bool all = a.get<bool>("all", false);
    const std::vector<NodeId> nodes = breakdownNodes(g, objectArgs(a), all);
    if (nodes.empty() && !all)
        throw CommandError("puppeteerBreakdown: give transforms or joints");
    const double w = std::clamp(a.get<double>("weight", 0.0), -3.0, 3.0);
    const double now = a.get<double>("time", g.time());
    const std::vector<BreakdownSample> samples = sampleBreakdown(g, nodes, now);
    int keyed = 0;
    for (const BreakdownSample& s : samples)
        for (const auto& [attr, ch] : s.channels)
        {
            keyVec(s.name, ch, breakdownValue(g, s, attr, w), now);
            ++keyed;
        }
    return keyed;
}
} // namespace

void registerSpinnerOps(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "puppeteerRotate",
        .doc = "Turn transforms by angle degrees around a world axis. inherit keeps their children's world rotation, so "
               "only the selected joints turn. pivot center turns them around their common centre. Returns the count.",
        .positionalDoc = "transforms (default the selection)",
        .flags = {
            {"axis", "ax", T::Vec3, nullptr, "World axis."},
            {"angle", "a", T::Float, 0.0, "Degrees."},
            {"pivot", "pv", T::String, "each", "each (own origin) or center (their centre).", {"each", "center"}},
            {"inherit", "inh", T::Bool, false, "Children keep their world rotation (inherit rotate off)."},
        },
        .run = rotateNodes,
    });
    ctx.registerCommand({
        .name = "puppeteerScale",
        .doc = "Multiply transforms' scale. compensate (toon scale) keeps their children's world size, so only the "
               "selected segment squashes or stretches. Returns the count.",
        .positionalDoc = "transforms (default the selection)",
        .flags = {
            {"by", "by", T::Vec3, nullptr, "Multipliers along the local axes."},
            {"compensate", "cmp", T::Bool, false, "Children keep their world size."},
        },
        .run = scaleNodes,
    });
    ctx.registerCommand({
        .name = "puppeteerMove",
        .doc = "Move transforms by a world offset. aim (sticky aim) turns each one's parent to point at the new position "
               "instead, keeping the bone's length. Returns the count.",
        .positionalDoc = "transforms (default the selection)",
        .flags = {
            {"by", "by", T::Vec3, nullptr, "World offset, cm."},
            {"aim", "aim", T::Bool, false, "Turn the parent instead of translating."},
            {"keepChildren", "kc", T::Bool, false, "Leave the children where they are (rigging a joint's pivot)."},
            {"stickyAim", "sa", T::Bool, false, "Rigging: re-aim the parent's and the joint's orients along the bones."},
            {"sticky2nd", "s2", T::Bool, false, "With sticky aim: the secondary axis lies in the chain's plane."},
            {"keepDirection", "kd", T::Bool, false, "With sticky 2nd: the secondary axis keeps pointing the way it did."},
            {"fullBody", "fb", T::Bool, false,
             "Joints in a character move by posing the whole body: their pins and locked hands and feet hold."},
            {"autoPose", "ap", T::Bool, false, "With fullBody: the auto-poser places the unpinned body (UE5 Manny joint names)."},
        },
        .run = moveNodes,
    });
    ctx.registerCommand({
        .name = "puppeteerBreakdown",
        .doc = "Breakdown: key the pose at the current time part way toward the previous key (weight -1) or the next key "
               "(weight 1), for the selected joints or (all) every joint of their characters. Returns the channels keyed.",
        .positionalDoc = "transforms (default the selection)",
        .flags = {
            {"weight", "w", T::Float, 0.0, "-1 previous key, 0 unchanged, 1 next key."},
            {"all", "a", T::Bool, false, "Every joint of the selection's characters (of the scene with no character)."},
            {"time", "t", T::Float, nullptr, "Seconds; default the current time."},
        },
        .run = breakdownCommand,
    });
}
std::vector<NodeId> breakdownNodes(Graph& g, const std::vector<std::string>& names, bool all)
{
    std::vector<NodeId> nodes;
    if (!all)
    {
        for (const std::string& s : names)
            if (Node* n = g.find(s); n && isTransform(*n) && std::find(nodes.begin(), nodes.end(), n->id()) == nodes.end())
                nodes.push_back(n->id());
        return nodes;
    }
    std::vector<NodeId> roots;
    for (const std::string& s : names)
        if (Node* n = g.find(s))
            if (NodeId c = characterFor(g, n->id()))
                if (Node* r = named(g, textOf(*g.node(c), "root")))
                    roots.push_back(r->id());
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !rig::isJoint(*n))
            continue;
        bool wanted = roots.empty();
        for (NodeId r : roots)
            for (NodeId cur = id; cur && !wanted; cur = g.parent(cur))
                wanted = cur == r;
        if (wanted)
            nodes.push_back(id);
    }
    return nodes;
}

std::vector<BreakdownSample> sampleBreakdown(Graph& g, const std::vector<NodeId>& nodes, double now)
{
    std::vector<BreakdownSample> out;
    for (NodeId id : nodes)
    {
        BreakdownSample s{.id = id, .name = g.node(id)->path(), .channels = animatedChannels(g.node(id)->path())};
        if (s.channels.empty())
            continue;
        const json prevT = ops::execute("findKeyframe", {{"args", {s.name}}, {"which", "previous"}, {"time", now}});
        const json nextT = ops::execute("findKeyframe", {{"args", {s.name}}, {"which", "next"}, {"time", now}});
        // findKeyframe wraps around; a key on the wrong side means there is none that way.
        const double tp = prevT.is_number() && prevT.get<double>() < now - kTimeEps ? prevT.get<double>() : now;
        const double tn = nextT.is_number() && nextT.get<double>() > now + kTimeEps ? nextT.get<double>() : now;
        s.previousTime = tp;
        s.nextTime = tn;
        out.push_back(std::move(s));
    }
    const double was = g.time();
    auto sampleAt = [&](auto member, auto timeOf) {
        for (BreakdownSample& s : out)
        {
            const double at = timeOf(s);
            if (std::abs(g.time() - at) > kTimeEps)
                g.setTime(at);
            g.evaluate();
            for (const auto& [attr, ch] : s.channels)
                (s.*member)[attr] = vecOf(g, s.id, attr);
        }
    };
    sampleAt(&BreakdownSample::current, [&](const BreakdownSample&) { return now; });
    sampleAt(&BreakdownSample::previous, [](const BreakdownSample& s) { return s.previousTime; });
    sampleAt(&BreakdownSample::next, [](const BreakdownSample& s) { return s.nextTime; });
    if (std::abs(g.time() - was) > kTimeEps)
        g.setTime(was);
    g.evaluate();
    return out;
}

glm::dvec3 breakdownValue(Graph& g, const BreakdownSample& s, int attr, double weight)
{
    const glm::dvec3 cur = s.current.at(attr), prev = s.previous.at(attr), next = s.next.at(attr);
    if (attr != Transform::kRotate)
        return math::breakdown(cur, prev, next, weight);
    const glm::dquat q = math::breakdown(quatOf(g, s.id, cur), quatOf(g, s.id, prev), quatOf(g, s.id, next), weight);
    return eulerNear(glm::mat3_cast(q), rotateOrderOf(g, s.id), cur);
}
} // namespace puppeteer
