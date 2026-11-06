// Overlap: procedural secondary motion for joint chains, on the base simulation framework. The node types hold the
// setup, OverlapSolver below reads them into the framework's Solver interface, and OverlapSolver.h does the maths.

#include "Overlap.h"
#include "OverlapSolver.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Relocate.h>
#include <lila/Simulation.h>
#include <lila/SplineChain.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace lila;
using namespace lila::scene;

namespace overlap
{
namespace
{
enum class Space : int64_t
{
    World,
    CharacterRoot,
    Parent,
};

// Attribute helpers: Overlap's types are found by name, so these look attributes up by name too.
int attr(const Node& n, const char* name) { return n.type().attrIndex(name); }
double num(const Node& n, const char* name) { return n.as<double>(attr(n, name)); }
int64_t integer(const Node& n, const char* name) { return n.as<int64_t>(attr(n, name)); }
bool flag(const Node& n, const char* name) { return n.as<bool>(attr(n, name)); }
const std::string& text(const Node& n, const char* name) { return n.as<std::string>(attr(n, name)); }

/** The node a text attr names (a full path, or a name in older scenes); null when it is gone or ambiguous. */
Node* named(Graph& g, const std::string& path)
{
    try
    {
        return path.empty() ? nullptr : g.find(path);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}
glm::dvec3 vec(const Node& n, const char* name) { return n.as<glm::dvec3>(attr(n, name)); }

AttrDef number(const char* name, double def, const char* doc, uint32_t extra = 0)
{
    return {.name = name, .type = PlugType::Float, .defaultValue = def, .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
AttrDef whole(const char* name, int64_t def, const char* doc)
{
    return {.name = name, .type = PlugType::Int, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
}
AttrDef limited(AttrDef a, std::optional<double> lo, std::optional<double> hi = {})
{
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}
AttrDef boolean(const char* name, bool def, const char* doc)
{
    return {.name = name, .type = PlugType::Bool, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
}
AttrDef string(const char* name, const char* doc, uint32_t extra = 0)
{
    return {.name = name, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
AttrDef choice(const char* name, int64_t def, std::vector<std::string> names, const char* doc)
{
    return {.name = name, .type = PlugType::Enum, .defaultValue = def, .flags = AttrInput | AttrStorable, .enumNames = std::move(names), .doc = doc};
}
AttrDef vector3(const char* name, glm::dvec3 def, const char* doc)
{
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
    const std::string n = name;
    a.childNames = {n + "X", n + "Y", n + "Z"};
    return a;
}

struct Rigid
{
    glm::dvec3 p{0.0};
    glm::dquat q{1.0, 0.0, 0.0, 0.0};
};

Rigid rigidOf(const glm::dmat4& m)
{
    glm::dmat3 r(m);
    for (int i = 0; i < 3; ++i)
        r[i] /= std::max(glm::length(r[i]), 1e-300);
    return {glm::dvec3(m[3]), glm::normalize(glm::quat_cast(r))};
}

glm::dmat4 matrixOf(const Rigid& r)
{
    glm::dmat4 m = glm::mat4_cast(r.q);
    m[3] = glm::dvec4(r.p, 1.0);
    return m;
}

glm::dvec3 scaleOf(const glm::dmat4& m) { return {glm::length(glm::dvec3(m[0])), glm::length(glm::dvec3(m[1])), glm::length(glm::dvec3(m[2]))}; }

glm::dmat4 withScale(glm::dmat4 m, const glm::dvec3& s)
{
    for (int i = 0; i < 3; ++i)
        m[i] *= s[i];
    return m;
}

void putMatrix(std::vector<double>& out, const glm::dmat4& m)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out.push_back(m[c][r]);
}

glm::dmat4 getMatrix(const double* in)
{
    glm::dmat4 m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m[c][r] = in[c * 4 + r];
    return m;
}

/** The node's own attribute values, for the parameters hash. Driven attributes are left out: they're sampled. */
uint64_t hashOwn(Graph& g, const Node& n, uint64_t h)
{
    h = sim::hashBytes(n.name().data(), n.name().size(), h);
    for (const AttrDef& a : n.type().ownAttrs)
    {
        const int i = n.type().attrIndex(a.name);
        if (i < 0 || !(a.flags & AttrStorable) || (a.flags & AttrOutput) || a.name == sim::kRecordingsAttr || (a.flags & AttrKeyable))
            continue;
        if (g.source({n.id(), i, -1}))
            continue;
        const std::string v = toJson(n.value(i)).dump();
        h = sim::hashBytes(v.data(), v.size(), h);
    }
    return h;
}

bool isJoint(const Node& n) { return n.type().isA("joint"); }

std::vector<NodeId> descendantsOfType(Graph& g, NodeId root, const char* type)
{
    std::vector<NodeId> out;
    for (NodeId d : g.descendants(root))
        if (Node* n = g.node(d); n && n->type().isA(type))
            out.push_back(d);
    std::sort(out.begin(), out.end());
    return out;
}

NodeId firstChildJoint(Graph& g, NodeId id)
{
    for (NodeId c : g.children(id))
        if (Node* n = g.node(c); n && isJoint(*n))
            return c;
    return kNoNode;
}

/** Root to end, or root down first children to the end of the branch when end is empty. Empty when end isn't below root. */
std::vector<NodeId> chainJoints(Graph& g, NodeId root, NodeId end)
{
    std::vector<NodeId> out;
    if (end)
    {
        for (NodeId cur = end; cur; cur = g.parent(cur))
        {
            out.push_back(cur);
            if (cur == root)
            {
                std::reverse(out.begin(), out.end());
                return out;
            }
        }
        return {};
    }
    for (NodeId cur = root; cur && out.size() < 4096; cur = firstChildJoint(g, cur))
        out.push_back(cur);
    return out;
}

int depth(Graph& g, NodeId id)
{
    int d = 0;
    for (NodeId cur = g.parent(id); cur; cur = g.parent(cur))
        ++d;
    return d;
}

BoneSettings chainSettings(const Node& c)
{
    BoneSettings b;
    b.translation = {num(c, "translationWeight"), num(c, "stiffness"), num(c, "damping"), num(c, "maxOffset")};
    b.rotation = {num(c, "rotationWeight"), num(c, "rotationStiffness"), num(c, "rotationDamping"), num(c, "maxAngle")};
    b.scale.amount = num(c, "squash");
    b.scale.reference = num(c, "squashReference");
    b.scale.smoothing = num(c, "squashSmoothing");
    b.scale.restAdapt = num(c, "squashRestAdapt");
    b.scale.axis = StretchAxis(integer(c, "stretchAxis"));
    b.scale.preserveVolume = flag(c, "preserveVolume");
    b.scale.minScale = num(c, "squashMin");
    b.scale.maxScale = num(c, "squashMax");
    b.response = {num(c, "inertia"), num(c, "velocityInfluence"), num(c, "accelerationInfluence"), num(c, "gravityScale"), num(c, "windScale")};
    b.style = {num(c, "exaggeration"), num(c, "blend")};
    return b;
}

ChainBias chainBias(const Node& c)
{
    return {num(c, "bias"), num(c, "falloff"), num(c, "tipStiffness"), num(c, "tipExaggeration"), flag(c, "distribute")};
}

Global globalSettings(const Node& o)
{
    Global g;
    g.stiffness = num(o, "stiffnessScale");
    g.damping = num(o, "dampingScale");
    g.maxOffset = num(o, "maxOffsetScale");
    g.maxAngle = num(o, "maxAngleScale");
    g.inertia = num(o, "inertiaScale");
    g.gravity = num(o, "gravityMultiplier");
    g.exaggeration = num(o, "exaggerationScale");
    g.squash = num(o, "squashScale");
    return g;
}

/** Cardinal axis down the bone toward its first child joint, in the joint's own space. */
glm::dvec3 autoStretchAxis(Graph& g, NodeId joint)
{
    NodeId child = firstChildJoint(g, joint);
    if (!child)
        return {1.0, 0.0, 0.0};
    const glm::dvec3 d = glm::abs(g.node(child)->as<glm::dvec3>(attr(*g.node(child), "translate")));
    if (d.x >= d.y && d.x >= d.z)
        return {1.0, 0.0, 0.0};
    return d.y >= d.z ? glm::dvec3(0.0, 1.0, 0.0) : glm::dvec3(0.0, 0.0, 1.0);
}

class OverlapSolver final : public sim::Solver
{
public:
    explicit OverlapSolver(NodeId node) : mNode(node) {}

    sim::Setup configure(Graph& g) override
    {
        Node* o = g.node(mNode);
        if (!o)
            throw std::invalid_argument("the overlap node is gone");
        mEntries.clear();
        mColliders.clear();
        mSpace = Space(integer(*o, "space"));
        mGravity = vec(*o, "gravity");
        mWindDirection = vec(*o, "windDirection");
        mWindStrength = num(*o, "windStrength");
        mTurbulence = num(*o, "windTurbulence");
        mWindFrequency = num(*o, "windFrequency");
        mSeed = uint64_t(integer(*o, "seed"));
        mWeightAttr = attr(*o, "weight");
        mEnabled = flag(*o, "enabled");
        Node* carrier = named(g, text(*o, "carrier"));
        mCarrier = carrier ? carrier->id() : kNoNode;
        mCarrierInertia = num(*o, "carrierInertia");
        if (mCarrier)
            mSpace = Space::CharacterRoot;
        const Global global = globalSettings(*o);
        const int substeps = int(std::clamp<int64_t>(integer(*o, "substeps"), 1, 64));

        uint64_t h = hashOwn(g, *o, 0x9ae16a3b2f90404full);

        // Per-joint overrides, by the joint they name.
        std::map<NodeId, NodeId> overrides;
        for (NodeId j : descendantsOfType(g, mNode, kJointType))
        {
            if (Node* joint = named(g, text(*g.node(j), "joint")))
                overrides[joint->id()] = j;
            h = hashOwn(g, *g.node(j), h);
        }

        std::set<NodeId> taken;
        std::vector<std::pair<int, size_t>> order;
        for (NodeId c : descendantsOfType(g, mNode, kChainType))
        {
            const Node& chain = *g.node(c);
            h = hashOwn(g, chain, h);
            if (!flag(chain, "enabled"))
                continue;
            Node* root = named(g, text(chain, "root"));
            Node* end = named(g, text(chain, "end"));
            if (!root || (!text(chain, "end").empty() && !end))
                continue;
            std::vector<NodeId> joints = chainJoints(g, root->id(), end ? end->id() : kNoNode);
            const BoneSettings base = chainSettings(chain);
            const ChainBias bias = chainBias(chain);
            const int count = int(joints.size());
            for (int i = 0; i < count; ++i)
            {
                // The first chain to claim a joint wins, so a joint is never simulated against its own output.
                if (!taken.insert(joints[size_t(i)]).second)
                    continue;
                Entry e;
                e.joint = joints[size_t(i)];
                const Node& jn = *g.node(e.joint);
                e.settings = global.apply(chainBone(base, bias, count > 1 ? double(i) / double(count - 1) : 0.0, count));
                double delay = num(chain, "delay");
                e.collide = flag(chain, "collide");
                e.radius = num(chain, "radius");
                if (auto ov = overrides.find(jn.id()); ov != overrides.end())
                {
                    const Node& j = *g.node(ov->second);
                    if (!flag(j, "enabled"))
                    {
                        taken.erase(e.joint);
                        continue;
                    }
                    e.override = ov->second;
                    e.pinAttr = attr(j, "pin");
                    if (num(j, "delay") >= 0.0)
                        delay = num(j, "delay");
                    const double stiff = num(j, "stiffnessScale");
                    e.settings.translation.stiffness *= stiff;
                    e.settings.rotation.stiffness *= stiff;
                    e.settings.translation.damping *= std::sqrt(std::max(stiff, 0.0));
                    e.settings.rotation.damping *= std::sqrt(std::max(stiff, 0.0));
                    e.settings.style.exaggeration *= num(j, "exaggerationScale");
                }
                e.delaySteps = int(std::clamp<int64_t>(int64_t(std::llround(std::max(delay, 0.0) * substeps)), 0, 4096));
                e.aim = flag(chain, "aim");
                if (i + 1 < count)
                    e.next = joints[size_t(i) + 1];
                switch (e.settings.scale.axis)
                {
                case StretchAxis::X: e.stretchAxis = {1, 0, 0}; break;
                case StretchAxis::Y: e.stretchAxis = {0, 1, 0}; break;
                case StretchAxis::Z: e.stretchAxis = {0, 0, 1}; break;
                default: e.stretchAxis = autoStretchAxis(g, e.joint); break;
                }
                order.push_back({depth(g, e.joint), mEntries.size()});
                mEntries.push_back(e);
            }
        }
        // Parents before children; ties keep the order the chains listed them in.
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<Entry> sorted;
        for (const auto& [d, i] : order)
            sorted.push_back(mEntries[i]);
        mEntries = std::move(sorted);

        std::map<NodeId, int> index;
        for (size_t i = 0; i < mEntries.size(); ++i)
            index[mEntries[i].joint] = int(i);
        size_t offset = 0;
        sim::Setup su;
        for (size_t i = 0; i < mEntries.size(); ++i)
        {
            Entry& e = mEntries[i];
            e.parent = g.parent(e.joint);
            for (NodeId a = e.parent; a && e.ancestor < 0; a = g.parent(a))
                if (auto it = index.find(a); it != index.end())
                    e.ancestor = it->second;
            e.offset = offset;
            offset += BoneState::kDoubles + (e.delaySteps > 0 ? 1 + size_t(e.delaySteps) * 4 : 0);
            using C = sim::Channel;
            su.layout.insert(su.layout.end(), {{C::Position, 3}, {C::Velocity, 3}, {C::Rotation, 3}, {C::AngularVelocity, 3},
                                               {C::Ignore, 6}, {C::Other, 1}, {C::Position, 3}, {C::Ignore, 13}});
            if (e.delaySteps > 0)
                su.layout.push_back({C::Ignore, uint32_t(1 + e.delaySteps * 4)});
            su.outputs.push_back(e.joint);
            h = sim::hashBytes(&e.joint, sizeof e.joint, h);
            h = sim::hashBytes(&e.parent, sizeof e.parent, h);
            h = sim::hashBytes(&e.stretchAxis, sizeof e.stretchAxis, h);
        }
        for (Entry& e : mEntries)
            if (auto it = index.find(e.next); e.aim && it != index.end() && mEntries[size_t(it->second)].ancestor == int(&e - mEntries.data()))
                e.child = it->second;
        // The carrier's last position and velocity, and how many of them are known yet.
        mCarrierBase = offset;
        if (mCarrier)
            su.layout.push_back({sim::Channel::Ignore, 7});

        if (mCarrier)
            mRoot = mCarrier;
        else if (mSpace == Space::CharacterRoot)
        {
            Node* root = named(g, text(*o, "root"));
            mRoot = root ? root->id() : kNoNode;
            if (!mRoot && !mEntries.empty())
                for (NodeId cur = mEntries.front().joint; cur; cur = g.parent(cur))
                    if (Node* n = g.node(cur); n && isJoint(*n))
                        mRoot = cur;
        }
        else
            mRoot = kNoNode;
        h = sim::hashBytes(&mRoot, sizeof mRoot, h);

        for (NodeId c : g.nodes())
            if (Node* n = g.node(c); n && n->type().isA(kColliderType) && flag(*n, "enabled"))
                mColliders.push_back({c, Shape(integer(*n, "shape")), num(*n, "radius"), num(*n, "length")});
        std::sort(mColliders.begin(), mColliders.end(), [](const ColliderNode& a, const ColliderNode& b) { return a.node < b.node; });
        for (const ColliderNode& c : mColliders)
            h = hashOwn(g, *g.node(c.node), h);

        su.startFrame = integer(*o, "startFrame");
        su.substeps = substeps;
        su.stepped = int(std::clamp<int64_t>(integer(*o, "stepped"), 1, 12));
        su.paramsHash = h;
        su.reads = {mNode};
        if (mRoot)
            su.reads.push_back(mRoot);
        if (mCarrier)
            su.sources.push_back(mCarrier);
        for (const ColliderNode& c : mColliders)
            su.reads.push_back(c.node);
        for (const Entry& e : mEntries)
        {
            su.reads.insert(su.reads.end(), {e.joint, e.parent});
            if (e.override)
                su.reads.push_back(e.override);
        }
        if (!mEnabled)
        {
            // Disabled: nothing simulates, every output follows the animation.
            for (Entry& e : mEntries)
                e.settings.style.weight = 0.0;
        }
        return su;
    }

    void sample(Graph& g, std::vector<double>& in) override
    {
        auto world = [&](NodeId id) -> glm::dmat4 {
            Node* n = id ? g.node(id) : nullptr;
            int w = n ? attr(*n, "worldMatrix") : -1;
            return w >= 0 ? std::get<glm::dmat4>(g.value({id, w, -1})) : glm::dmat4(1.0);
        };
        putMatrix(in, world(mRoot));
        in.push_back(std::get<double>(g.value({mNode, mWeightAttr, -1})));
        for (const ColliderNode& c : mColliders)
            putMatrix(in, world(c.node));
        for (const Entry& e : mEntries)
        {
            putMatrix(in, world(e.joint));
            putMatrix(in, world(e.parent));
            in.push_back(e.override && e.pinAttr >= 0 ? std::get<double>(g.value({e.override, e.pinAttr, -1})) : 0.0);
        }
    }

    void reset(const std::vector<double>& in, std::vector<double>& state) override
    {
        for (size_t i = 0; i < mEntries.size(); ++i)
        {
            const Entry& e = mEntries[i];
            const Rigid t = target(in, i);
            BoneState s;
            overlap::reset(s, t.p, t.q);
            s.write(&state[e.offset]);
            fillHistory(e, state, t.q);
        }
        if (mCarrier)
        {
            double* c = &state[mCarrierBase];
            const glm::dvec3 p(getMatrix(&in[0])[3]);
            std::fill(c, c + 7, 0.0);
            c[0] = p.x, c[1] = p.y, c[2] = p.z;
            c[6] = 1.0;
        }
    }

    /**
     * The carrier's acceleration, world space, from its positions at the ends of the last steps. The first steps after a
     * reset, and the step a loop wraps on (where a travelling carrier jumps back), add none.
     */
    glm::dvec3 carrierAcceleration(const std::vector<double>& in, std::vector<double>& state, const sim::StepInfo& at) const
    {
        double* c = &state[mCarrierBase];
        const glm::dvec3 p(getMatrix(&in[0])[3]), last(c[0], c[1], c[2]), lastVelocity(c[3], c[4], c[5]);
        const bool wrap = at.period > 0.0 && at.index == 0;
        const glm::dvec3 v = wrap ? lastVelocity : (p - last) / at.dt;
        const glm::dvec3 a = c[6] >= 2.0 && !wrap ? (v - lastVelocity) / at.dt : glm::dvec3(0.0);
        c[0] = p.x, c[1] = p.y, c[2] = p.z;
        c[3] = v.x, c[4] = v.y, c[5] = v.z;
        c[6] = std::min(c[6] + 1.0, 2.0);
        return a;
    }

    void step(const std::vector<double>& in, std::vector<double>& state, const sim::StepInfo& at) override
    {
        const double weight = in[16];
        const glm::dvec3 wind = windAt(at, -1);
        std::vector<Collider> worldColliders = colliders(in);
        const glm::dvec3 carried = mCarrier ? carrierAcceleration(in, state, at) * mCarrierInertia : glm::dvec3(0.0);
        for (size_t i = 0; i < mEntries.size(); ++i)
        {
            const Entry& e = mEntries[i];
            const Rigid frame = frameOf(in, i);
            const Rigid t = target(in, i);
            BoneState s;
            s.read(&state[e.offset]);
            const double alpha = e.settings.style.weight * weight * (1.0 - std::clamp(pin(in, i), 0.0, 1.0));
            if (alpha < kSmall || !e.settings.relevant())
            {
                // Suppressed bones still anchor their children, but hold no stale state.
                overlap::reset(s, t.p, t.q);
                s.write(&state[e.offset]);
                fillHistory(e, state, t.q);
                continue;
            }
            const glm::dquat delayed = delay(e, state, t.q);
            const glm::dquat toFrame = glm::conjugate(frame.q);
            Context ctx;
            ctx.gravity = toFrame * mGravity;
            ctx.force = toFrame * (mWindStrength != 0.0 ? wind + windAt(at, int(i)) : glm::dvec3(0.0));
            ctx.carried = toFrame * carried;
            ctx.stretchAxis = e.stretchAxis;
            integrate(e.settings, s, t.p, delayed, at.dt, ctx);
            if (e.collide)
                for (const Collider& wc : worldColliders)
                {
                    Collider c = wc;
                    c.a = toFrame * (c.a - frame.p);
                    c.b = toFrame * (c.b - frame.p);
                    const glm::dvec3 push = pushOut(t.p + s.offset, c, e.radius);
                    if (push == glm::dvec3(0.0))
                        continue;
                    s.offset += push;
                    const glm::dvec3 n = glm::normalize(push);
                    if (const double vn = glm::dot(s.velocity, n); vn < 0.0)
                        s.velocity -= n * vn;
                }
            s.write(&state[e.offset]);
        }
    }

    void pose(const std::vector<double>& in, const std::vector<double>& state, std::vector<glm::dmat4>& world) override
    {
        const size_t n = mEntries.size();
        world.assign(n, glm::dmat4(1.0));
        std::vector<glm::dmat4> solved(n), authored(n);
        std::vector<Rigid> frames(n), targets(n);
        std::vector<Pose> poses(n);
        std::vector<Collider> worldColliders = colliders(in);
        const double weight = in[16];
        for (size_t i = 0; i < n; ++i)
        {
            const Entry& e = mEntries[i];
            authored[i] = getMatrix(&in[entryBase(i)]);
            frames[i] = frameOf(in, i);
            targets[i] = target(in, i);
            BoneState s;
            s.read(&state[e.offset]);
            const double alpha = e.settings.style.weight * weight * (1.0 - std::clamp(pin(in, i), 0.0, 1.0));
            poses[i].position = targets[i].p;
            poses[i].rotation = targets[i].q;
            if (alpha >= kSmall && e.settings.relevant())
                poses[i] = compose(e.settings, s, targets[i].p, s.prevTargetRotation, alpha, e.stretchAxis);
        }
        for (size_t i = 0; i < n; ++i)
        {
            const Entry& e = mEntries[i];
            const Rigid& frame = frames[i];
            const Pose& p = poses[i];
            // Aiming joints keep their bone lengths: their place comes from the turn of the joint above.
            const glm::dvec3 at = e.aim ? targets[i].p : p.position;
            glm::dmat4 full = withScale(matrixOf({frame.p + frame.q * at, glm::normalize(frame.q * p.rotation)}), scaleOf(authored[i]));
            // The ancestor's deviation is carried rigidly, so a chain hangs together without each joint amplifying the last.
            if (e.ancestor >= 0)
                full = solved[size_t(e.ancestor)] * glm::inverse(authored[size_t(e.ancestor)]) * full;
            if (e.aim && e.child >= 0)
                full = aimAtChild(full, authored[i], size_t(e.child), frames, targets, poses, authored, worldColliders, e.collide);
            else if (e.collide && !e.aim)
                for (const Collider& c : worldColliders)
                    full[3] += glm::dvec4(pushOut(glm::dvec3(full[3]), c, e.radius), 0.0);
            solved[i] = full;
            world[i] = withScale(full, p.stretch);
        }
    }

    /**
     * Turns a joint so its bone points where its child would be if it moved by its own positional deviation, kept out of
     * the colliders: lag and collisions become rotation, and the chain never stretches.
     */
    glm::dmat4 aimAtChild(glm::dmat4 full, const glm::dmat4& authoredSelf, size_t child, const std::vector<Rigid>& frames,
                          const std::vector<Rigid>& targets, const std::vector<Pose>& poses, const std::vector<glm::dmat4>& authored,
                          const std::vector<Collider>& worldColliders, bool collide) const
    {
        const glm::dvec3 local(glm::inverse(authoredSelf) * authored[child][3]);
        const glm::dvec3 origin(full[3]);
        const glm::dvec3 from = glm::dvec3(full * glm::dvec4(local, 1.0)) - origin;
        glm::dvec3 to = from + frames[child].q * (poses[child].position - targets[child].p);
        if (collide && mEntries[child].collide)
            for (const Collider& c : worldColliders)
                to += pushOut(origin + to, c, mEntries[child].radius);
        const double lf = glm::length(from), lt = glm::length(to);
        if (lf < 1e-9 || lt < 1e-9)
            return full;
        const glm::dvec3 axis = glm::cross(from / lf, to / lt);
        const double s = glm::length(axis), c = glm::dot(from / lf, to / lt);
        if (s < 1e-12)
            return full;
        const glm::dmat3 turn = glm::mat3_cast(glm::angleAxis(std::atan2(s, c), axis / s));
        for (int k = 0; k < 3; ++k)
            full[k] = glm::dvec4(turn * glm::dvec3(full[k]), 0.0);
        return full;
    }

private:
    struct Entry
    {
        NodeId joint = kNoNode;
        NodeId parent = kNoNode;
        NodeId override = kNoNode;
        int pinAttr = -1;
        int ancestor = -1;
        BoneSettings settings;
        glm::dvec3 stretchAxis{1.0, 0.0, 0.0};
        int delaySteps = 0;
        bool collide = true;
        double radius = 1.0;
        size_t offset = 0;
        bool aim = false;
        NodeId next = kNoNode; // the next joint of its chain
        int child = -1;        // aiming: that joint's entry
    };

    struct ColliderNode
    {
        NodeId node = kNoNode;
        Shape shape = Shape::Sphere;
        double radius = 1.0;
        double length = 0.0;
    };

    // Inputs: root world (16), weight (1), each collider's world (16), then per entry its world, its parent's world
    // (16 each) and its pin.
    size_t entryBase(size_t i) const { return 17 + mColliders.size() * 16 + i * 33; }
    double pin(const std::vector<double>& in, size_t i) const { return in[entryBase(i) + 32]; }

    Rigid frameOf(const std::vector<double>& in, size_t i) const
    {
        switch (mSpace)
        {
        case Space::CharacterRoot: return rigidOf(getMatrix(&in[0]));
        case Space::Parent: return rigidOf(getMatrix(&in[entryBase(i) + 16]));
        default: return {};
        }
    }

    /** The joint's animated pose in its simulation space. */
    Rigid target(const std::vector<double>& in, size_t i) const
    {
        const Rigid f = frameOf(in, i), a = rigidOf(getMatrix(&in[entryBase(i)]));
        const glm::dquat inv = glm::conjugate(f.q);
        return {inv * (a.p - f.p), glm::normalize(inv * a.q)};
    }

    std::vector<Collider> colliders(const std::vector<double>& in) const
    {
        std::vector<Collider> out;
        for (size_t c = 0; c < mColliders.size(); ++c)
        {
            const glm::dmat4 m = getMatrix(&in[17 + c * 16]);
            const glm::dvec3 s = scaleOf(m);
            Collider col;
            col.shape = mColliders[c].shape;
            col.radius = mColliders[c].radius * std::max({s.x, s.y, s.z});
            const double half = mColliders[c].length * 0.5;
            col.a = glm::dvec3(m * glm::dvec4(col.shape == Shape::Capsule ? -half : 0.0, 0.0, 0.0, 1.0));
            col.b = glm::dvec3(m * glm::dvec4(col.shape == Shape::Capsule ? half : 0.0, 0.0, 0.0, 1.0));
            out.push_back(col);
        }
        return out;
    }

    /**
     * Wind as an acceleration in world space. entry -1 is the steady part with its gusts; an entry index adds that
     * joint's own turbulence, decorrelated by seed. Looping, the noise repeats with the period.
     */
    glm::dvec3 windAt(const sim::StepInfo& at, int entry) const
    {
        if (mWindStrength == 0.0)
            return glm::dvec3(0.0);
        const double freq = std::max(mWindFrequency, 1e-6);
        double x = at.seconds * freq;
        int64_t period = 0;
        if (at.period > 0.0)
        {
            period = std::max<int64_t>(1, std::llround(at.period * freq));
            x = at.seconds / at.period * double(period);
        }
        const double len = glm::length(mWindDirection);
        const glm::dvec3 dir = len > 1e-12 ? mWindDirection / len : glm::dvec3(1.0, 0.0, 0.0);
        if (entry < 0)
            return dir * (mWindStrength * (1.0 + mTurbulence * sim::noise(mSeed, x, period)));
        return sim::noise3(mSeed * 1000003ull + uint64_t(entry) + 1, x, period) * (mTurbulence * mWindStrength);
    }

    void fillHistory(const Entry& e, std::vector<double>& state, const glm::dquat& q) const
    {
        if (e.delaySteps <= 0)
            return;
        double* h = &state[e.offset + BoneState::kDoubles];
        h[0] = 0.0;
        for (int k = 0; k < e.delaySteps; ++k)
        {
            double* slot = h + 1 + k * 4;
            slot[0] = q.w, slot[1] = q.x, slot[2] = q.y, slot[3] = q.z;
        }
    }

    /** The target rotation delaySteps ago, pushing the current one into the joint's history. */
    glm::dquat delay(const Entry& e, std::vector<double>& state, const glm::dquat& now) const
    {
        if (e.delaySteps <= 0)
            return now;
        double* h = &state[e.offset + BoneState::kDoubles];
        const int head = int(h[0]);
        double* slot = h + 1 + head * 4;
        const glm::dquat old(slot[0], slot[1], slot[2], slot[3]);
        slot[0] = now.w, slot[1] = now.x, slot[2] = now.y, slot[3] = now.z;
        h[0] = double((head + 1) % e.delaySteps);
        return old;
    }

    NodeId mNode = kNoNode;
    NodeId mRoot = kNoNode;
    std::vector<Entry> mEntries;
    std::vector<ColliderNode> mColliders;
    Space mSpace = Space::CharacterRoot;
    glm::dvec3 mGravity{0.0, -980.0, 0.0};
    glm::dvec3 mWindDirection{1.0, 0.0, 0.0};
    double mWindStrength = 0.0, mTurbulence = 0.0, mWindFrequency = 0.5;
    uint64_t mSeed = 1;
    int mWeightAttr = -1;
    bool mEnabled = true;
    NodeId mCarrier = kNoNode;
    double mCarrierInertia = 1.0;
    size_t mCarrierBase = 0;
};

bool registerTypes(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    if (!transform)
        return false;
    const uint32_t hidden = AttrHidden;

    NodeType o{.name = kOverlapType,
               .base = transform,
               .dag = true,
               .doc = "Overlap: procedural secondary motion (follow-through, lag, wobble) for a character's joint chains, "
                      "simulated deterministically and recorded to an additive animation layer.",
               .icon = "wave-sine",
               .create = transform->create};
    o.ownAttrs = {
        boolean("enabled", true, "Simulate. Off, every joint follows its animation."),
        string("root", "The character's root joint, the frame of the character-root space. Empty: the top joint above the first chain."),
        whole("startFrame", 0, "Frame the simulation starts from, at rest on the animation."),
        limited(whole("substeps", 2, "Simulation steps per frame. More is more accurate; the result never depends on playback."), 1.0, 64.0),
        choice("space", 1, {"world", "characterRoot", "parent"},
               "Frame the deviation lives in: world, the character root (travel doesn't drag the chains) or each joint's parent."),
        vector3("gravity", {0.0, -980.0, 0.0}, "Gravity, cm/s^2, scaled by each chain's gravity scale."),
        vector3("windDirection", {1.0, 0.0, 0.0}, "Wind direction, world space."),
        limited(number("windStrength", 0.0, "Wind acceleration, cm/s^2, scaled by each chain's wind scale."), 0.0),
        limited(number("windTurbulence", 0.5, "Gusts and per-joint swirl, as a share of the wind strength."), 0.0),
        limited(number("windFrequency", 0.5, "How fast the gusts change, per second."), 0.0),
        whole("seed", 1, "Seed of the wind noise: the same seed gives the same wind on every machine."),
        limited(whole("stepped", 1, "Hold each simulated pose this many frames: 2 animates on twos, 3 on threes."), 1.0, 12.0),
        limited(number("weight", 1.0, "Blend of the whole character's overlap. Keyable.", AttrKeyable), 0.0, 1.0),
        limited(number("stiffnessScale", 1.0, "Multiplies every chain's stiffness."), 0.0),
        limited(number("dampingScale", 1.0, "Multiplies every chain's damping."), 0.0),
        limited(number("maxOffsetScale", 1.0, "Multiplies every chain's max offset."), 0.0),
        limited(number("maxAngleScale", 1.0, "Multiplies every chain's max angle."), 0.0),
        limited(number("inertiaScale", 1.0, "Multiplies every chain's inertia."), 0.0),
        number("gravityMultiplier", 1.0, "Multiplies every chain's gravity scale."),
        limited(number("exaggerationScale", 1.0, "Multiplies every chain's exaggeration."), 0.0),
        limited(number("squashScale", 1.0, "Multiplies every chain's squash and stretch."), 0.0),
        boolean("preview", true, "Draw the simulated chains in the viewport, simulating as the time changes."),
        string(sim::kRecordingsAttr, "Layers recorded from this simulation, muted while it samples its inputs.", hidden),
        string("carrier", "Carried motion: a transform (saddle, seat) whose space the chains simulate in, overriding space."),
        limited(number("carrierInertia", 1.0, "Share of the carrier's acceleration the chains feel: 1 like the world, 0 carried rigidly."), 0.0),
    };

    NodeType chain{.name = kChainType,
                   .base = transform,
                   .dag = true,
                   .doc = "One chain of an Overlap: joints from root to end, with the settings spread along them.",
                   .icon = "line",
                   .create = transform->create};
    chain.ownAttrs = {
        string("root", "First joint of the chain."),
        string("end", "Last joint, below root. Empty: follow first children to the end of the branch."),
        boolean("enabled", true, "Simulate this chain."),
        limited(number("bias", 0.0, "Where the motion concentrates: 0 even, +1 fades out toward the root, -1 toward the tip."), -1.0, 1.0),
        limited(number("falloff", 1.0, "Shape of the bias: 1 linear, above 1 holds the strong end longer."), 0.01),
        limited(number("tipStiffness", 1.0, "Stiffness multiplier at the tip; damping follows so the bounce stays the same."), 0.01),
        limited(number("tipExaggeration", 1.0, "Exaggeration multiplier at the tip, for a whip that grows to the end."), 0.0),
        boolean("distribute", true, "The settings are the whole chain's motion, not every joint's: shares are normalised."),
        limited(number("translationWeight", 1.0, "How much of the positional deviation is applied."), 0.0, 1.0),
        limited(number("stiffness", 250.0, "Spring pulling each joint back onto its animated position. Higher settles sooner."), 0.0),
        limited(number("damping", 12.0, "Resistance to motion: low overshoots and wobbles, high settles without bouncing."), 0.0),
        limited(number("maxOffset", 15.0, "Soft limit on the positional deviation, cm."), 0.0),
        limited(number("rotationWeight", 1.0, "How much of the rotational deviation is applied."), 0.0, 1.0),
        limited(number("rotationStiffness", 250.0, "Spring pulling each joint back onto its animated orientation."), 0.0),
        limited(number("rotationDamping", 12.0, "Resistance to rotation."), 0.0),
        limited(number("maxAngle", 30.0, "Soft limit on the rotational deviation, degrees."), 0.0, 180.0),
        limited(number("inertia", 1.0, "Resistance to being dragged by the animation: heavy appendages high, light ones low."), 0.0),
        number("velocityInfluence", 0.0, "Trail behind the motion in proportion to its speed."),
        number("accelerationInfluence", 0.0, "Snap on acceleration and deceleration."),
        number("gravityScale", 0.0, "Share of the gravity: the joint sags about gravity * scale / stiffness."),
        number("windScale", 1.0, "Share of the wind."),
        limited(number("exaggeration", 1.0, "Scales the deviation on output: 1 simulated, 1.5 cartoony, 2 strongly exaggerated."), 0.0),
        limited(number("blend", 1.0, "Blend between the animation and the simulated pose."), 0.0, 1.0),
        number("squash", 0.0, "Squash and stretch from the deviation: 0 off, negative inverts."),
        limited(number("squashReference", 10.0, "Deviation that reaches the full squash, cm."), 0.0),
        limited(number("squashSmoothing", 0.08, "Seconds the stretch takes to follow the deviation."), 0.0),
        limited(number("squashRestAdapt", 0.5, "Seconds a deviation holds before it counts as the new neutral. 0 measures from the animation."), 0.0),
        choice("stretchAxis", 0, {"auto", "x", "y", "z"}, "Axis the joint stretches along; auto points at its first child."),
        boolean("preserveVolume", true, "Thin the other axes as the joint stretches."),
        limited(number("squashMin", 0.6, "Smallest scale along the axis."), 0.01, 1.0),
        limited(number("squashMax", 1.8, "Largest scale along the axis."), 1.0),
        limited(number("delay", 0.0, "Frames each joint's rotation lags its animation, for overlapping action."), 0.0),
        boolean("collide", true, "Keep the joints out of overlap colliders."),
        limited(number("radius", 1.0, "Joint radius for collisions, cm."), 0.0),
        boolean("aim", false, "Turn each joint toward where its child lags instead of moving the child: the chain keeps its "
                              "bone lengths, as a tail or tentacle does."),
    };

    NodeType joint{.name = kJointType,
                   .base = transform,
                   .dag = true,
                   .doc = "Per-joint Overlap settings: a keyable pin to the animation, a delay and scales on its chain's settings.",
                   .icon = "pin",
                   .create = transform->create};
    joint.ownAttrs = {
        string("joint", "The joint these settings apply to."),
        boolean("enabled", true, "Simulate this joint. Off, it follows its animation."),
        limited(number("pin", 0.0, "Pin the joint to its animation: 1 holds it exactly, 0 lets it overlap freely. Keyable.", AttrKeyable), 0.0, 1.0),
        limited(number("delay", -1.0, "Frames this joint's rotation lags its animation; negative uses the chain's."), -1.0),
        limited(number("stiffnessScale", 1.0, "Multiplies the chain's stiffness here."), 0.0),
        limited(number("exaggerationScale", 1.0, "Multiplies the chain's exaggeration here."), 0.0),
    };

    NodeType collider{.name = kColliderType,
                      .base = transform,
                      .dag = true,
                      .doc = "A sphere or capsule the Overlap chains are kept out of. Parent it to a joint to follow the body.",
                      .icon = "circle-dashed",
                      .create = transform->create};
    collider.ownAttrs = {
        choice("shape", 0, {"sphere", "capsule"}, "Sphere, or a capsule along the local X axis."),
        limited(number("radius", 5.0, "Radius, cm."), 0.0),
        limited(number("length", 20.0, "Capsule length between the end centres, cm."), 0.0),
        boolean("enabled", true, "Collide with this."),
    };

    lila::scene::relocate::registerWorldAttrs(o.name, {.directions = {"gravity", "windDirection"}});
    for (const NodeType* t : {&chain, &joint, &collider})
        lila::scene::relocate::registerNeutral(t->name);
    for (NodeType* t : {&o, &chain, &joint, &collider})
        if (!types::registerType(ctx, std::move(*t)))
            return false;
    return true;
}

Graph& graph() { return Graph::current(); }

NodeId topJoint(Graph& g, NodeId id)
{
    NodeId top = id;
    for (NodeId cur = g.parent(id); cur; cur = g.parent(cur))
        if (Node* n = g.node(cur); n && isJoint(*n))
            top = cur;
    return top;
}

std::string setString(Graph& g, NodeId id, const char* name, const std::string& v)
{
    g.setValue({id, attr(*g.node(id), name), -1}, v);
    return v;
}

NodeId overlapFor(Graph& g, NodeId root)
{
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().name == kOverlapType && named(g, text(*n, "root")) == g.node(root))
            return id;
    return kNoNode;
}

std::vector<NodeId> jointArgs(Graph& g, const Args& a, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& name : a.objects())
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
        if (!isJoint(*n))
            throw CommandError(std::string(cmd) + ": " + name + " is not a joint");
        out.push_back(n->id());
    }
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select or name joints");
    return out;
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;

    ctx.registerCommand({
        .name = "createOverlap",
        .doc = "Add Overlap to joints: one chain from each joint down its first children (or to end), on the character's "
               "Overlap node, made when it has none. Returns the Overlap node.",
        .positionalDoc = "chain root joints (default the selection)",
        .flags = {
            {"name", "n", T::String, nullptr, "Name of a new Overlap node. Default: <root joint>_overlap."},
            nodeFlag({"end", "e", T::String, nullptr, "Last joint of the chain (one root joint only)."}, "joint"),
            {"space", "sp", T::String, nullptr, "Simulation space of a new Overlap node.", {"world", "characterRoot", "parent"}},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> joints = jointArgs(g, a, "createOverlap");
            if (a.has("end") && joints.size() != 1)
                throw CommandError("createOverlap: end needs exactly one root joint");
            const NodeId top = topJoint(g, joints.front());
            const std::string rootName = g.node(top)->name();
            NodeId o = overlapFor(g, top);
            if (!o)
            {
                o = g.createNode(*types::find(kOverlapType), a.get<std::string>("name", rootName + "_overlap"))->id();
                setString(g, o, "root", g.path(top));
                if (a.has("space"))
                {
                    const std::string s = a.get<std::string>("space");
                    g.setValue({o, attr(*g.node(o), "space"), -1}, int64_t(s == "world" ? 0 : s == "parent" ? 2 : 1));
                }
            }
            for (NodeId j : joints)
            {
                const std::string name = g.node(j)->name();
                NodeId c = g.createNode(*types::find(kChainType), name + "_chain", o)->id();
                setString(g, c, "root", g.path(j));
                if (a.has("end"))
                {
                    Node* end = g.find(a.get<std::string>("end"));
                    if (!end || chainJoints(g, j, end->id()).empty())
                        throw CommandError("createOverlap: " + a.get<std::string>("end") + " is not below " + g.path(j));
                    setString(g, c, "end", end->path());
                }
            }
            g.select({o});
            return g.node(o)->path();
        },
    });

    ctx.registerCommand({
        .name = "createOverlapCollider",
        .doc = "Make an Overlap collider (sphere or capsule), parented to a node so it follows it. Returns its name.",
        .positionalDoc = "parent (default the first selected node; none for world)",
        .flags = {
            {"shape", "sh", T::String, "sphere", "sphere | capsule", {"sphere", "capsule"}},
            {"radius", "r", T::Float, 5.0, "Radius, cm.", {}, {}, 0.0},
            {"length", "l", T::Float, 20.0, "Capsule length, cm.", {}, {}, 0.0},
            {"name", "n", T::String, "overlapCollider1", "Name."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId parent = kNoNode;
            std::vector<std::string> objs = a.objects();
            if (!objs.empty())
            {
                Node* p = g.find(objs.front());
                if (!p)
                    throw CommandError("createOverlapCollider: no node named '" + objs.front() + "'");
                parent = p->id();
            }
            Node* n = g.createNode(*types::find(kColliderType), a.get<std::string>("name", "overlapCollider1"), parent);
            const NodeId id = n->id();
            g.setValue({id, attr(*n, "shape"), -1}, int64_t(a.get<std::string>("shape", "sphere") == "capsule" ? 1 : 0));
            g.setValue({id, attr(*n, "radius"), -1}, a.get<double>("radius", 5.0));
            g.setValue({id, attr(*n, "length"), -1}, a.get<double>("length", 20.0));
            g.select({id});
            return g.node(id)->path();
        },
    });

    ctx.registerCommand({
        .name = "addOverlapJoint",
        .doc = "Per-joint Overlap settings (a keyable pin, delay, stiffness and exaggeration scales) on the Overlap node "
               "whose chains hold each joint. Returns the settings nodes.",
        .positionalDoc = "joints (default the selection)",
        .flags = {
            {"pin", "p", T::Float, nullptr, "Pin weight: 1 holds the joint on its animation.", {}, {}, 0.0, 1.0},
            {"delay", "d", T::Float, nullptr, "Frames its rotation lags.", {}, {}, -1.0},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            for (NodeId j : jointArgs(g, a, "addOverlapJoint"))
            {
                const std::string name = g.node(j)->name();
                NodeId o = overlapFor(g, topJoint(g, j));
                if (!o)
                    throw CommandError("addOverlapJoint: " + g.path(j) + " has no Overlap; run createOverlap first");
                NodeId settings = kNoNode;
                for (NodeId s : descendantsOfType(g, o, kJointType))
                    if (named(g, text(*g.node(s), "joint")) == g.node(j))
                        settings = s;
                if (!settings)
                {
                    settings = g.createNode(*types::find(kJointType), name + "_overlapJoint", o)->id();
                    setString(g, settings, "joint", g.path(j));
                }
                if (a.has("pin"))
                    g.setValue({settings, attr(*g.node(settings), "pin"), -1}, a.get<double>("pin"));
                if (a.has("delay"))
                    g.setValue({settings, attr(*g.node(settings), "delay"), -1}, a.get<double>("delay"));
                out.push_back(g.node(settings)->path());
            }
            return out;
        },
    });
}
/** World-space length of the joints from first to last. */
double chainLength(Graph& g, const std::vector<NodeId>& joints)
{
    double len = 0.0;
    for (size_t i = 1; i < joints.size(); ++i)
        len += glm::length(glm::dvec3(g.node(joints[i])->as<glm::dmat4>(attr(*g.node(joints[i]), "worldMatrix"))[3]) -
                           glm::dvec3(g.node(joints[i - 1])->as<glm::dmat4>(attr(*g.node(joints[i - 1]), "worldMatrix"))[3]));
    return len;
}

/**
 * splineDynamics' overlap mode: an Overlap node of its own, in world space (or the carrier's), with one aiming chain over
 * the spline chain's joints so lag turns the joints and the chain keeps its length.
 */
NodeId attachToSplineChain(Graph& g, NodeId chainNode, const std::vector<NodeId>& joints, const spline::DynamicsOptions& o)
{
    // Every enabled overlap collider in the scene counts; naming others is a mistake worth saying.
    for (NodeId col : o.colliders)
        if (!g.node(col) || !g.node(col)->type().isA(kColliderType))
            throw std::invalid_argument("overlap dynamics collide with overlap colliders (createOverlapCollider); " +
                                        (g.node(col) ? g.node(col)->path() : std::string("a collider")) + " is not one");
    const std::string base = o.name.empty() ? g.node(chainNode)->name() + "_overlap" : o.name;
    const NodeId ov = g.createNode(*types::find(kOverlapType), base)->id();
    auto set = [&](NodeId id, const char* name, PlugValue v) { g.setValue({id, attr(*g.node(id), name), -1}, std::move(v)); };
    set(ov, "space", int64_t(Space::World));
    if (o.carrier)
    {
        set(ov, "carrier", g.path(o.carrier));
        set(ov, "carrierInertia", o.carrierInertia);
    }
    const double length = std::max(chainLength(g, joints), 1.0);
    const NodeId c = g.createNode(*types::find(kChainType), g.node(chainNode)->name() + "_dynamics", ov)->id();
    set(c, "root", g.path(joints.front()));
    set(c, "end", g.path(joints.back()));
    set(c, "aim", true);
    set(c, "maxOffset", length * (o.full ? 1.0 : 0.5));
    if (o.full)
    {
        set(c, "stiffness", o.stiffness.value_or(4.0));
        set(c, "rotationStiffness", o.stiffness.value_or(4.0));
        set(c, "damping", o.damping.value_or(3.0));
        set(c, "rotationDamping", o.damping.value_or(3.0));
        set(c, "maxAngle", 150.0);
        set(c, "gravityScale", o.gravity.value_or(1.0));
    }
    else
    {
        if (o.stiffness)
        {
            set(c, "stiffness", *o.stiffness);
            set(c, "rotationStiffness", *o.stiffness);
        }
        if (o.damping)
        {
            set(c, "damping", *o.damping);
            set(c, "rotationDamping", *o.damping);
        }
        set(c, "gravityScale", o.gravity.value_or(0.0));
    }
    set(c, "radius", o.radius.value_or(std::max(1.0, length / double(joints.size()) * 0.25)));
    return ov;
}
} // namespace

bool registerScene(ext::Context& ctx)
{
    if (!registerTypes(ctx))
        return false;
    ctx.add(spline::dynamicsProviders(), "splineDynamics", "overlap",
            spline::DynamicsProvider{.name = "overlap",
                                     .doc = "Stylised overlap: cheap, loops seamlessly, collides with overlap colliders.",
                                     .attach = attachToSplineChain});
    if (!sim::registerSolverType(ctx, {.nodeType = kOverlapType, .create = [](Graph&, NodeId node) -> std::unique_ptr<sim::Solver> {
                                           return std::make_unique<OverlapSolver>(node);
                                       }}))
        return false;
    registerCommands(ctx);
    return true;
}
} // namespace overlap

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    if (!overlap::registerScene(ctx))
        return 1;
    overlap::registerUi(ctx);
    return 0;
}
