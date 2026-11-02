#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Ik.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/RotationCurve.h"
#include "scene/Skin.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

/**
 * Editing a bound skeleton without deforming its mesh or losing its animation.
 *
 * Entering puts the skeleton in bind pose, disables its skins (the mesh shows its bind shape, which is what the skin
 * computes in bind pose anyway) and parks every connection into the joints' channels, so nothing animates or solves
 * while joints are edited. The entry state (layout, pose, parked connections) lives in a skeletonEdit node, so the
 * mode saves with the scene and undoes like any edit. Leaving rebinds every influence to its new bind pose, re-expresses
 * the animation for the new layout and reconnects everything.
 */
namespace lila::scene
{
using namespace rigutil;

namespace
{
constexpr const char* kStateType = "skeletonEdit";
constexpr double kEps = 1e-9;

class SkeletonEditNode final : public Node
{
public:
    enum Attr
    {
        kState,
    };
};

/** The joint attributes editing may change that are not animation channels. */
struct Layout
{
    glm::dvec3 jo{0.0}, ra{0.0}, rp{0.0}, rpt{0.0}, sp{0.0}, spt{0.0};
    RotateOrder order = RotateOrder::XYZ;
};

json vec(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
glm::dvec3 vec(const json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }

Layout layoutOf(Graph& g, NodeId id)
{
    auto v = [&](int a) { return std::get<glm::dvec3>(g.value({id, a, -1})); };
    return {.jo = v(Joint::kJointOrient), .ra = v(Transform::kRotateAxis), .rp = v(Transform::kRotatePivot),
            .rpt = v(Transform::kRotatePivotTranslate), .sp = v(Transform::kScalePivot), .spt = v(Transform::kScalePivotTranslate),
            .order = RotateOrder(std::get<int64_t>(g.value({id, Transform::kRotateOrder, -1})))};
}

json toJson(const Layout& l)
{
    return {{"jo", vec(l.jo)}, {"ra", vec(l.ra)}, {"rp", vec(l.rp)}, {"rpt", vec(l.rpt)},
            {"sp", vec(l.sp)}, {"spt", vec(l.spt)}, {"order", int64_t(l.order)}};
}

Layout layoutFrom(const json& j)
{
    return {.jo = vec(j["jo"]), .ra = vec(j["ra"]), .rp = vec(j["rp"]), .rpt = vec(j["rpt"]), .sp = vec(j["sp"]), .spt = vec(j["spt"]),
            .order = RotateOrder(j["order"].get<int64_t>())};
}

/** A joint's local matrix from a layout and channel values, as Joint computes it (segment scale compensation aside). */
glm::dmat4 compose(const Layout& l, const glm::dvec3& t, const glm::dvec3& r, const glm::dvec3& s)
{
    auto tr = [](const glm::dvec3& v) { return glm::translate(glm::dmat4(1.0), v); };
    return tr(t) * tr(l.rpt) * tr(l.rp) * eulerToMatrix(l.jo, RotateOrder::XYZ) * eulerToMatrix(r, l.order) *
           eulerToMatrix(l.ra, RotateOrder::XYZ) * tr(-l.rp) * tr(l.spt) * tr(l.sp) * glm::scale(glm::dmat4(1.0), s) * tr(-l.sp);
}

NodeId stateNode(Graph& g)
{
    for (NodeId id : g.nodes())
        if (g.node(id)->type().name == kStateType)
            return id;
    return kNoNode;
}

int depthOf(Graph& g, NodeId id)
{
    int d = 0;
    for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
        ++d;
    return d;
}

NodeId topJoint(Graph& g, NodeId j)
{
    while (g.parent(j) != kNoNode && rig::isJoint(*g.node(g.parent(j))))
        j = g.parent(j);
    return j;
}

/** Skins and whole skeletons the targets name: meshes, skinClusters or joints; everything skinned when empty. */
void gather(Graph& g, const std::vector<std::string>& targets, std::vector<NodeId>& skins, std::vector<NodeId>& joints)
{
    std::set<NodeId> skinSet, tops;
    std::vector<NodeId> allSkins;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().isA("skinCluster"))
            allSkins.push_back(id);
    if (targets.empty())
        skinSet.insert(allSkins.begin(), allSkins.end());
    for (const std::string& name : targets)
    {
        Node& n = nodeNamed(name, "editSkeleton");
        if (rig::isJoint(n))
            tops.insert(topJoint(g, n.id()));
        else if (n.type().isA("skinCluster"))
            skinSet.insert(n.id());
        else if (NodeId s = skin::find(g, n.id()); s != kNoNode)
            skinSet.insert(s);
        else
            throw CommandError("editSkeleton: " + name + " is neither a joint nor a skinned mesh");
    }
    // A named skeleton brings every skin it drives, so none of them deforms while it is edited.
    for (NodeId s : allSkins)
        for (const skin::Influence& inf : skin::influences(g, s))
            if (inf.joint != kNoNode && tops.contains(topJoint(g, inf.joint)))
                skinSet.insert(s);
    for (NodeId s : skinSet)
        for (const skin::Influence& inf : skin::influences(g, s))
            if (inf.joint != kNoNode)
                tops.insert(topJoint(g, inf.joint));
    skins.assign(skinSet.begin(), skinSet.end());
    joints.clear();
    for (NodeId top : tops)
    {
        joints.push_back(top);
        for (NodeId d : g.descendants(top))
            if (rig::isJoint(*g.node(d)))
                joints.push_back(d);
    }
    std::stable_sort(joints.begin(), joints.end(), [&](NodeId a, NodeId b) { return depthOf(g, a) < depthOf(g, b); });
}

bool parked(int attr)
{
    switch (attr)
    {
    case Transform::kTranslate:
    case Transform::kRotate:
    case Transform::kScale:
    case Transform::kRotateAxis:
    case Joint::kJointOrient:
    case Joint::kSolvedRotate:
    case Joint::kIkBlend:
    case Joint::kIkStretch: return true;
    default: return false;
    }
}

/** Puts the skins' influences in bind pose, parents first. */
void goToBind(Graph& g, const std::vector<NodeId>& skins)
{
    std::vector<std::pair<NodeId, glm::dmat4>> binds;
    for (NodeId s : skins)
        for (const skin::Influence& inf : skin::influences(g, s))
            if (inf.joint != kNoNode)
                binds.push_back({inf.joint, glm::inverse(g.node(inf.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix))});
    std::stable_sort(binds.begin(), binds.end(), [&](auto& a, auto& b) { return depthOf(g, a.first) < depthOf(g, b.first); });
    for (auto& [joint, bind] : binds)
    {
        g.evaluate();
        glm::dmat4 parent = g.node(joint)->as<glm::dmat4>(Transform::kParentMatrix);
        guarded([&] { Transform::setLocalMatrix(g, joint, glm::inverse(parent) * bind); return 0; });
    }
}

void enter(Graph& g, const std::vector<std::string>& targets)
{
    LILA_ZONE();

    if (stateNode(g) != kNoNode)
        throw CommandError("editSkeleton: already editing; turn it off first");
    std::vector<NodeId> skins, joints;
    gather(g, targets, skins, joints);
    if (joints.empty())
        throw CommandError("editSkeleton: nothing skinned to edit; select a skinned mesh or a joint");

    g.evaluate();
    json st;
    st["time"] = g.time();
    json js = json::array();
    std::set<NodeId> inSet(joints.begin(), joints.end());
    for (NodeId id : joints)
    {
        auto v = [&](int a) { return vec(std::get<glm::dvec3>(g.value({id, a, -1}))); };
        js.push_back({{"name", g.node(id)->path()}, {"layout", toJson(layoutOf(g, id))},
                      {"pose", {{"t", v(Transform::kTranslate)}, {"r", v(Transform::kRotate)}, {"s", v(Transform::kScale)}}}});
    }

    json conns = json::array();
    std::vector<PlugRef> drop;
    for (const auto& [dst, src] : g.connections())
        if (inSet.contains(dst.node) && parked(dst.attr))
        {
            conns.push_back({g.plugName(src), g.plugName(dst)});
            drop.push_back(dst);
        }
    for (const PlugRef& d : drop)
        g.disconnect(d);
    for (NodeId id : joints)
        if (g.node(id)->as<double>(Joint::kIkBlend) != 0.0)
            g.setValue({id, Joint::kIkBlend}, 0.0);

    json sk = json::array();
    for (NodeId s : skins)
    {
        bool enabled = g.node(s)->as<bool>(SkinClusterNode::kEnabled);
        sk.push_back({{"name", g.node(s)->path()}, {"enabled", enabled}});
        if (enabled && !g.source({s, SkinClusterNode::kEnabled, -1}))
            g.setValue({s, SkinClusterNode::kEnabled}, false);
    }

    goToBind(g, skins);
    g.evaluate();
    for (size_t i = 0; i < joints.size(); ++i)
    {
        js[i]["bindT"] = vec(g.node(joints[i])->as<glm::dvec3>(Transform::kTranslate));
        js[i]["bindPos"] = vec(positionOf(*g.node(joints[i])));
    }
    st["joints"] = std::move(js);
    st["connections"] = std::move(conns);
    st["skins"] = std::move(sk);

    Node* n = g.createNode(*types::find(kStateType), "skeletonEdit", kNoNode, false);
    g.setValue({n->id(), SkeletonEditNode::kState}, st.dump());
}

/** Where a joint's channels come from while it is parked: curve nodes by channel, or something else. */
struct Channels
{
    std::array<NodeId, 3> t{kNoNode, kNoNode, kNoNode}, r{kNoNode, kNoNode, kNoNode}, s{kNoNode, kNoNode, kNoNode};
    NodeId rq = kNoNode;
    bool foreignT = false, foreignR = false, foreignS = false;
};

double evalScalar(Graph& g, NodeId curve, double t)
{
    anim::Curve c = anim::curveOf(*g.node(curve));
    return c.loop ? c.looped().evaluate(t) : c.evaluate(t);
}

glm::dvec3 evalGroup(Graph& g, const std::array<NodeId, 3>& curves, const glm::dvec3& rest, double t)
{
    glm::dvec3 v = rest;
    for (int a = 0; a < 3; ++a)
        if (curves[size_t(a)] != kNoNode)
            v[a] = evalScalar(g, curves[size_t(a)], t);
    return v;
}

void addTimes(Graph& g, const std::array<NodeId, 3>& curves, std::set<double>& out)
{
    for (NodeId c : curves)
        if (c != kNoNode)
            for (const anim::Key& k : anim::curveOf(*g.node(c)).keys)
                out.insert(k.time);
}

bool anyCurve(const std::array<NodeId, 3>& c) { return c[0] != kNoNode || c[1] != kNoNode || c[2] != kNoNode; }

/** Writes sampled values into a channel group: existing curves get keys at the times, static axes a value or a new curve. */
void writeGroup(Graph& g, NodeId joint, int attr, const std::array<NodeId, 3>& curves, const std::map<double, glm::dvec3>& values,
                std::vector<std::pair<PlugRef, std::map<double, double>>>& newCurves)
{
    for (int a = 0; a < 3; ++a)
    {
        NodeId curve = curves[size_t(a)];
        if (curve != kNoNode)
        {
            anim::Curve c = anim::curveOf(*g.node(curve));
            bool changed = false;
            for (const auto& [t, v] : values)
            {
                int i = c.find(t);
                if (i >= 0 && std::abs(c.keys[size_t(i)].value - v[a]) <= kEps)
                    continue;
                if (i < 0 && std::abs(c.evaluate(t) - v[a]) <= kEps)
                    continue;
                changed = true;
                if (i >= 0)
                    c.keys[size_t(i)].value = v[a];
                else
                    c.insert({.time = t, .value = v[a]});
            }
            if (!changed)
                continue;
            // The stored angles describe the old values; let the tangents follow the new ones.
            for (anim::Key& k : c.keys)
            {
                if (k.in == anim::Tangent::Fixed)
                    k.in = anim::Tangent::Auto;
                if (k.out == anim::Tangent::Fixed)
                    k.out = anim::Tangent::Auto;
            }
            anim::setCurve(g, curve, c);
            continue;
        }
        double lo = 1e300, hi = -1e300;
        for (const auto& [t, v] : values)
            lo = std::min(lo, v[a]), hi = std::max(hi, v[a]);
        if (values.empty())
            continue;
        if (hi - lo <= kEps)
        {
            glm::dvec3 cur = g.node(joint)->as<glm::dvec3>(attr);
            if (std::abs(cur[a] - lo) > kEps)
                g.setValue({joint, attr, a}, lo);
            continue;
        }
        std::map<double, double> keys;
        for (const auto& [t, v] : values)
            keys[t] = v[a];
        newCurves.push_back({{joint, attr, a}, std::move(keys)});
    }
}

void exit(Graph& g, bool preserveWorld)
{
    LILA_ZONE();

    NodeId stateId = stateNode(g);
    if (stateId == kNoNode)
        throw CommandError("editSkeleton: not editing a skeleton");
    json st = json::parse(g.node(stateId)->as<std::string>(SkeletonEditNode::kState));

    struct Rec
    {
        NodeId id = kNoNode;
        int parent = -1;
        Layout oldLayout, newLayout;
        glm::dvec3 poseT, poseR, poseS;
        glm::dvec3 delta{0.0};
        bool moved = false;
        Channels ch;
        std::set<double> ownT, ownR, times;
    };
    std::vector<Rec> recs;
    std::unordered_map<NodeId, int> index;
    g.evaluate();
    for (const json& j : st["joints"])
    {
        Node* n = g.find(j["name"].get<std::string>());
        if (!n || !rig::isJoint(*n))
            continue;
        Rec r;
        r.id = n->id();
        r.oldLayout = layoutFrom(j["layout"]);
        r.newLayout = layoutOf(g, r.id);
        r.poseT = vec(j["pose"]["t"]);
        r.poseR = vec(j["pose"]["r"]);
        r.poseS = vec(j["pose"]["s"]);
        // A joint whose bind position moved takes the move into its translate; the others keep their world placement.
        r.moved = glm::length(positionOf(*n) - vec(j["bindPos"])) > 1e-6;
        r.delta = n->as<glm::dvec3>(Transform::kTranslate) - vec(j["bindT"]);
        index[r.id] = int(recs.size());
        recs.push_back(r);
    }
    for (Rec& r : recs)
        if (auto it = index.find(g.parent(r.id)); it != index.end())
            r.parent = it->second;

    std::vector<std::pair<PlugRef, PlugRef>> conns;
    for (const json& c : st["connections"])
    {
        try
        {
            conns.push_back({g.plug(c[0].get<std::string>()), g.plug(c[1].get<std::string>())});
        }
        catch (const std::invalid_argument&)
        {
            continue; // a node was deleted or renamed while editing
        }
    }
    for (const auto& [src, dst] : conns)
    {
        auto it = index.find(dst.node);
        if (it == index.end())
            continue;
        Channels& ch = recs[size_t(it->second)].ch;
        const Node& s = *g.node(src.node);
        bool scalar = anim::isCurve(s), rotation = anim::isRotationCurve(s);
        auto put = [&](std::array<NodeId, 3>& group, bool& foreign) {
            if (scalar && dst.component >= 0)
                group[size_t(dst.component)] = src.node;
            else
                foreign = true;
        };
        if (dst.attr == Transform::kTranslate)
            put(ch.t, ch.foreignT);
        else if (dst.attr == Transform::kScale)
            put(ch.s, ch.foreignS);
        else if (dst.attr == Transform::kRotate)
        {
            if (rotation && dst.component < 0)
                ch.rq = src.node;
            else
                put(ch.r, ch.foreignR);
        }
    }

    // Rebind every influence to its new bind pose, so the mesh stays where it is.
    std::vector<NodeId> skins;
    for (const json& s : st["skins"])
        if (Node* n = g.find(s["name"].get<std::string>()))
            skins.push_back(n->id());
    for (NodeId s : skins)
        for (const skin::Influence& inf : skin::influences(g, s))
        {
            if (inf.joint == kNoNode)
                continue;
            glm::dmat4 pre = glm::inverse(worldOf(*g.node(inf.joint)));
            glm::dmat4 old = g.node(inf.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix);
            bool same = true;
            for (int c = 0; c < 4; ++c)
                same = same && glm::length(pre[c] - old[c]) <= 1e-12;
            if (!same)
                g.setValue({inf.link, SkinInfluenceNode::kBindPreMatrix}, pre);
        }

    for (Rec& r : recs)
    {
        addTimes(g, r.ch.t, r.ownT);
        addTimes(g, r.ch.r, r.ownR);
        if (r.ch.rq != kNoNode)
            for (const anim::RotKey& k : anim::rotationCurveOf(*g.node(r.ch.rq)).keys)
                r.ownR.insert(k.time);
        std::set<double> s;
        addTimes(g, r.ch.s, s);
        r.times = r.ownT;
        r.times.insert(r.ownR.begin(), r.ownR.end());
        r.times.insert(s.begin(), s.end());
        if (r.parent >= 0)
            r.times.insert(recs[size_t(r.parent)].times.begin(), recs[size_t(r.parent)].times.end());
    }
    std::set<double> allTimes{st["time"].get<double>()};
    for (Rec& r : recs)
        allTimes.insert(r.times.begin(), r.times.end());

    // Channel values at each time: the old ones as they posed the skeleton, and the new ones that keep that pose.
    struct Sample
    {
        glm::dvec3 t, r, s;
    };
    std::vector<std::map<double, Sample>> newVals(recs.size());
    const double entryTime = st["time"].get<double>();
    std::vector<Sample> entryVals(recs.size());
    std::vector<glm::dvec3> lastR(recs.size());
    for (size_t i = 0; i < recs.size(); ++i)
        lastR[i] = recs[i].poseR;
    for (double time : allTimes)
    {
        std::vector<glm::dmat4> oldW(recs.size()), newW(recs.size());
        for (size_t i = 0; i < recs.size(); ++i)
        {
            Rec& r = recs[i];
            glm::dvec3 t = evalGroup(g, r.ch.t, r.poseT, time);
            glm::dvec3 rot = r.ch.rq != kNoNode ? [&] {
                anim::RotationCurve rc = anim::rotationCurveOf(*g.node(r.ch.rq));
                return rc.loop ? rc.looped().evaluate(time) : rc.evaluate(time);
            }()
                                                : evalGroup(g, r.ch.r, r.poseR, time);
            glm::dvec3 s = evalGroup(g, r.ch.s, r.poseS, time);
            glm::dmat4 pOld = r.parent >= 0 ? oldW[size_t(r.parent)] : glm::dmat4(1.0);
            glm::dmat4 pNew = r.parent >= 0 ? newW[size_t(r.parent)] : glm::dmat4(1.0);
            oldW[i] = pOld * compose(r.oldLayout, t, rot, s);

            Sample out{t, rot, s};
            if (r.moved)
                out.t = t + r.delta;
            // Keeping channels keeps raw keys only: unkeyed channels still keep their world placement.
            bool staticT = !anyCurve(r.ch.t) && !r.ch.foreignT;
            bool staticR = r.ch.rq == kNoNode && !anyCurve(r.ch.r) && !r.ch.foreignR;
            if (preserveWorld || staticR)
            {
                // World rotation kept: parent * jointOrient * R * rotateAxis is unchanged.
                glm::dmat3 want = glm::transpose(glm::dmat3(eulerToMatrix(r.newLayout.jo, RotateOrder::XYZ))) *
                                  glm::transpose(orthonormal(glm::dmat3(pNew))) * orthonormal(glm::dmat3(oldW[i])) *
                                  glm::transpose(glm::dmat3(eulerToMatrix(r.newLayout.ra, RotateOrder::XYZ)));
                out.r = eulerNear(want, r.newLayout.order, lastR[i]);
                lastR[i] = out.r;
            }
            if (!r.moved && (preserveWorld || staticT))
            {
                glm::dvec3 local(glm::inverse(pNew) * oldW[i][3]);
                out.t = local - glm::dvec3(compose(r.newLayout, glm::dvec3(0.0), out.r, s)[3]);
            }
            if (time == entryTime)
                entryVals[i] = out;
            newW[i] = pNew * compose(r.newLayout, out.t, out.r, out.s);
            if (r.times.contains(time) || r.times.empty())
                newVals[i][time] = out;
        }
    }

    // Write the new values: curves get keys, static channels values, and channels that now vary get new curves.
    std::vector<std::pair<PlugRef, std::map<double, double>>> newCurves;
    for (size_t i = 0; i < recs.size(); ++i)
    {
        Rec& r = recs[i];
        auto pick = [&](const std::set<double>& times, auto member) {
            std::map<double, glm::dvec3> out;
            // Without world preservation an unkeyed channel stays unkeyed: it keeps the entry pose's placement only.
            if (!preserveWorld && !anyCurve(member == &Sample::t ? r.ch.t : r.ch.r))
            {
                out[entryTime] = entryVals[i].*member;
                return out;
            }
            for (const auto& [t, smp] : newVals[i])
                if (times.empty() || times.contains(t) || r.times.empty())
                    out[t] = smp.*member;
            return out;
        };
        if (!r.ch.foreignT)
        {
            if (r.moved && anyCurve(r.ch.t))
            {
                // A moved joint's own keys shift by the move, exactly.
                for (int a = 0; a < 3; ++a)
                    if (NodeId c = r.ch.t[size_t(a)]; c != kNoNode && std::abs(r.delta[a]) > kEps)
                    {
                        anim::Curve cv = anim::curveOf(*g.node(c));
                        for (anim::Key& k : cv.keys)
                            k.value += r.delta[a];
                        anim::setCurve(g, c, cv);
                    }
                    else if (c == kNoNode)
                        g.setValue({r.id, Transform::kTranslate, a}, r.poseT[a] + r.delta[a]);
            }
            else
                writeGroup(g, r.id, Transform::kTranslate, r.ch.t, pick(r.times, &Sample::t), newCurves);
        }
        if (r.ch.foreignR)
            continue;
        if (r.ch.rq != kNoNode)
        {
            if (preserveWorld)
            {
                // Exact for rotation curves: R' = A * R * B at every key, and slerp commutes with both.
                glm::dquat a = glm::quat_cast(glm::transpose(glm::dmat3(eulerToMatrix(r.newLayout.jo, RotateOrder::XYZ))) *
                                              glm::dmat3(eulerToMatrix(r.oldLayout.jo, RotateOrder::XYZ)));
                glm::dmat3 bm = glm::dmat3(eulerToMatrix(r.oldLayout.ra, RotateOrder::XYZ)) *
                                glm::transpose(glm::dmat3(eulerToMatrix(r.newLayout.ra, RotateOrder::XYZ)));
                glm::dquat b = glm::quat_cast(bm);
                anim::RotationCurve rc = anim::rotationCurveOf(*g.node(r.ch.rq));
                if (r.newLayout.order != rc.order)
                {
                    rc.reorder(r.newLayout.order);
                    g.setValue({r.ch.rq, anim::RotationCurveNode::kRotateOrder}, int64_t(r.newLayout.order));
                }
                for (size_t k = 0; k < rc.keys.size(); ++k)
                {
                    anim::RotKey& key = rc.keys[k];
                    glm::dquat q = glm::normalize(a * key.q * b);
                    rc.setEuler(k, eulerNear(glm::mat3_cast(q), rc.order, key.euler));
                    rc.keys[k].q = q;
                    // Velocities are in the key's own frame, which turns by B.
                    rc.keys[k].inVelocity = glm::transpose(bm) * key.inVelocity;
                    rc.keys[k].outVelocity = glm::transpose(bm) * key.outVelocity;
                }
                anim::setRotationCurve(g, r.ch.rq, rc);
            }
        }
        else
            writeGroup(g, r.id, Transform::kRotate, r.ch.r, pick(r.ownR, &Sample::r), newCurves);
        for (int a = 0; a < 3; ++a)
            if (r.ch.s[size_t(a)] == kNoNode && !r.ch.foreignS)
            {
                double v = r.poseS[a];
                if (std::abs(g.node(r.id)->as<glm::dvec3>(Transform::kScale)[a] - v) > kEps)
                    g.setValue({r.id, Transform::kScale, a}, v);
            }
    }

    for (const json& s : st["skins"])
        if (Node* n = g.find(s["name"].get<std::string>()); n && !g.source({n->id(), SkinClusterNode::kEnabled, -1}))
            if (n->as<bool>(SkinClusterNode::kEnabled) != s["enabled"].get<bool>())
                g.setValue({n->id(), SkinClusterNode::kEnabled}, s["enabled"].get<bool>());
    for (const auto& [src, dst] : conns)
        if (!g.source(dst))
            guarded([&] { g.connect(src, dst, true); return 0; });
    for (auto& [plug, keys] : newCurves)
        for (const auto& [t, v] : keys)
            guarded([&] { return anim::setKey(g, plug, t, v); });

    g.evaluate();
    std::set<NodeId> handles;
    for (const Rec& r : recs)
        for (NodeId h : ik::handlesOf(g, r.id))
            handles.insert(h);
    for (NodeId h : handles)
        ik::refresh(g, h);
    g.deleteNode(stateId);
}

void registerCommandsImpl()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "editSkeleton",
        .doc = "Edit a bound skeleton without deforming its mesh. on=True puts the skeleton (of the given meshes or "
               "joints, else every skinned one) in bind pose and holds its animation; move, orient and re-pivot joints "
               "freely, then on=False rebinds the skins to the new joints and re-expresses the animation for them. "
               "preserve=\"worldPose\" keeps every joint's world placement at each key (a moved joint keeps its move); "
               "\"channels\" keeps the raw key values. Without on, toggles. Returns whether editing is on.",
        .positionalDoc = "meshes or joints",
        .flags = {
            {"on", "", T::Bool, nullptr, "Turn editing on or off; toggles when omitted."},
            {"preserve", "pr", T::String, "worldPose", "What leaving keeps: worldPose or channels.", {"worldPose", "channels"}},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            bool active = stateNode(g) != kNoNode;
            bool on = a.has("on") ? a.get<bool>("on") : !active;
            std::string preserve = a.get<std::string>("preserve", "worldPose");
            if (preserve != "worldPose" && preserve != "channels")
                throw CommandError("editSkeleton: preserve is worldPose or channels");
            if (on && !active)
            {
                std::vector<std::string> targets;
                for (const json& p : a.positional)
                    targets.push_back(p.get<std::string>());
                if (targets.empty())
                    for (NodeId id : g.selection())
                        targets.push_back(g.node(id)->path());
                enter(g, targets);
            }
            else if (!on && active)
                exit(g, preserve == "worldPose");
            return on;
        },
    });

    ops::registerCommand({
        .name = "moveSkinnedJoints",
        .doc = "Change bound joints' layout without deforming the mesh or losing animation, in one step: the same as "
               "editSkeleton on, setting the values in bind pose, then editSkeleton off. Moving a joint leaves its "
               "children where they are. Values are bind-pose values.",
        .positionalDoc = "joints",
        .flags = {
            {"translate", "t", T::Vec3, nullptr, "Translate in bind pose."},
            {"position", "p", T::Vec3, nullptr, "World position in bind pose."},
            {"jointOrient", "jo", T::Vec3, nullptr, "jointOrient, degrees."},
            {"rotateAxis", "ra", T::Vec3, nullptr, "rotateAxis, degrees."},
            {"rotatePivot", "rp", T::Vec3, nullptr, "rotatePivot."},
            {"scalePivot", "sp", T::Vec3, nullptr, "scalePivot."},
            {"preserve", "pr", T::String, "worldPose", "What to keep: worldPose or channels.", {"worldPose", "channels"}},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            std::vector<NodeId> joints;
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "moveSkinnedJoints");
                if (!rig::isJoint(n))
                    throw CommandError("moveSkinnedJoints: " + name + " is not a joint");
                joints.push_back(n.id());
            }
            if (joints.empty())
                throw CommandError("moveSkinnedJoints: give joints");
            bool wasEditing = stateNode(g) != kNoNode;
            if (!wasEditing)
            {
                std::vector<std::string> names;
                for (NodeId j : joints)
                    names.push_back(g.node(j)->path());
                enter(g, names);
            }
            for (NodeId j : joints)
            {
                g.evaluate();
                std::vector<std::pair<NodeId, glm::dmat4>> kids;
                for (NodeId c : g.children(j))
                    kids.push_back({c, worldOf(*g.node(c))});
                if (a.has("translate"))
                    g.setValue({j, Transform::kTranslate}, vec3Of(a.flags["translate"]));
                if (a.has("position"))
                {
                    glm::dmat4 parentInv = glm::inverse(g.node(j)->as<glm::dmat4>(Transform::kParentMatrix));
                    glm::dvec3 local(parentInv * glm::dvec4(vec3Of(a.flags["position"]), 1.0));
                    glm::dvec3 offset = glm::dvec3(Transform::localMatrix(*g.node(j))[3]) - g.node(j)->as<glm::dvec3>(Transform::kTranslate);
                    g.setValue({j, Transform::kTranslate}, local - offset);
                }
                for (auto [flag, attr] : {std::pair{"jointOrient", int(Joint::kJointOrient)}, {"rotateAxis", int(Transform::kRotateAxis)},
                                          {"rotatePivot", int(Transform::kRotatePivot)}, {"scalePivot", int(Transform::kScalePivot)}})
                    if (a.has(flag))
                        g.setValue({j, attr}, vec3Of(a.flags[flag]));
                g.evaluate();
                glm::dmat4 inv = glm::inverse(worldOf(*g.node(j)));
                for (auto& [c, world] : kids)
                    if (g.node(c)->type().isA("transform"))
                        guarded([&] { Transform::setLocalMatrix(g, c, inv * world); return 0; });
            }
            if (!wasEditing)
                exit(g, a.get<std::string>("preserve", "worldPose") == "worldPose");
            return nullptr;
        },
    });
}
} // namespace

void registerEditSkeletonTypes()
{
    if (types::find(kStateType))
        return;
    NodeType t;
    t.name = kStateType;
    t.icon = "bone";
    t.doc = "Present while editSkeleton is on: what leaving the mode needs to restore.";
    t.ownAttrs = {{.name = "state", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
                   .doc = "Entry layout, pose and parked connections, JSON."}};
    t.create = [] { return std::make_unique<SkeletonEditNode>(); };
    types::registerType(std::move(t));
}

bool rig::editingSkeleton(Graph& g)
{
    return stateNode(g) != kNoNode;
}

void registerEditSkeletonCommands()
{
    if (ops::find("editSkeleton"))
        return;
    registerCommandsImpl();
}
} // namespace lila::scene
