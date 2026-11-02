// Rubber hose limbs and fingers: the hoseLimb node that shapes a spline chain's curve, the hoseHand driver, and their
// commands.

#include "scene/SplineChain.h"

#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/RigMath.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace lila::scene::spline
{
using D3 = glm::dvec3;
using rigmath::rotationOf;

namespace
{
constexpr uint32_t kHoseKeyable = AttrInput | AttrStorable | AttrKeyable;
constexpr uint32_t kHoseHidden = AttrInput | AttrStorable | AttrHidden;
/** Curve points for a hose turning through radians: at least 48 spans, and one per degree and a half of turn. */
int samplesFor(double turn) { return std::clamp(int(std::abs(turn) / rigmath::radians(1.5)) + 1, 49, 721); }
/** Fingers sample past their tip so joints walking the arc by chord never leave it. */
constexpr double kFingerOvershoot = 1.25;

const std::vector<std::string> kHoseModes = {"limb", "finger"};

AttrDef hoseAttr(const char* name, PlugType type, PlugValue def, uint32_t flags, const char* doc, std::optional<double> lo = {},
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

D3 posOf(const glm::dmat4& m) { return D3(m[3]); }

D3 acrossAxis(const D3& v, const D3& axis) { return v - axis * glm::dot(v, axis); }

/** Any unit vector across axis. */
D3 anyAcross(const D3& axis)
{
    const D3 t = std::abs(axis.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0);
    return glm::normalize(acrossAxis(t, axis));
}

/** The angle a circular arc of unit length turns through to span a chord of length k (0 < k < 1), radians. */
double arcAngle(double k)
{
    k = std::clamp(k, 1e-4, 1.0);
    double lo = 1e-9, hi = 2.0 * rigmath::kPi;
    for (int i = 0; i < 80; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (std::sin(mid * 0.5) / (mid * 0.5) > k ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

class HoseLimbNode : public Node
{
public:
    enum Attr
    {
        kMode,
        kChain,
        kHose,
        kAutoArc,
        kRootMatrix,
        kMidMatrix,
        kEndMatrix,
        kBendMatrix,
        kHasMid,
        kRestLength,
        kPole,
        kCurl,
        kSpread,
        kCurlMax,
        kSpreadMax,
        kRestCurl,
        kHandCurl,
        kHandSpread,
        kFist,
        kSpreadWeight,
        kFistCurl,
        kBaseParentMatrix,
        kRestBase,
        kRestDir,
        kCurlAxis,
        kFingerLength,
        kOutCurve,
        kUpVector,
    };

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        curve::Spline s;
        s.kind = curve::Kind::Poly;
        D3 up(0, 1, 0);
        if (as<int64_t>(kMode) == 1)
            s.points = finger(up);
        else
            s.points = limb();
        setOutput(kOutCurve, curve::makeCurveData({std::move(s)}));
        setOutput(kUpVector, up);
    }

private:
    std::vector<D3> limb() const
    {
        const glm::dmat4 rootW = as<glm::dmat4>(kRootMatrix);
        const D3 r = posOf(rootW), e = posOf(as<glm::dmat4>(kEndMatrix));
        const bool hasMid = as<bool>(kHasMid);
        const D3 m = hasMid ? posOf(as<glm::dmat4>(kMidMatrix)) : 0.5 * (r + e);
        const D3 bend = posOf(as<glm::dmat4>(kBendMatrix)) - m;
        const double upper = glm::length(m - r), lower = glm::length(e - m);
        const double length = hasMid ? upper + lower : as<double>(kRestLength);
        const double hose = std::clamp(as<double>(kHose), 0.0, 1.0), autoArc = std::clamp(as<double>(kAutoArc), 0.0, 1.0);

        const D3 chord = e - r;
        const double d = glm::length(chord);
        const D3 c = d > 1e-9 ? chord / d : glm::normalize(rotationOf(rootW)[0]);
        D3 b = acrossAxis(m - r, c);
        if (glm::length(b) < 1e-6 * std::max(length, 1.0))
            b = acrossAxis(rotationOf(rootW) * as<D3>(kPole), c);
        b = glm::length(b) > 1e-12 ? glm::normalize(b) : anyAcross(c);

        // The circular arc of the limb's length from root to end, bowing toward b.
        const bool bows = length > 1e-9 && d < length * (1.0 - 1e-9);
        const double theta = bows ? arcAngle(d / length) : 0.0;
        const double radius = bows ? length / theta : 0.0;
        const D3 centre = 0.5 * (r + e) - b * radius * std::cos(theta * 0.5);
        const D3 control = 2.0 * m - 0.5 * (r + e);

        const int samples = samplesFor(theta);
        std::vector<D3> pts(size_t(samples), D3(0.0));
        for (int k = 0; k < samples; ++k)
        {
            const double u = double(k) / double(samples - 1);
            D3 jointed = glm::mix(r, e, u);
            if (hasMid && upper + lower > 1e-12)
            {
                const double at = u * (upper + lower);
                jointed = at <= upper ? glm::mix(r, m, upper > 1e-12 ? at / upper : 0.0) : glm::mix(m, e, lower > 1e-12 ? (at - upper) / lower : 1.0);
            }
            const D3 smooth = (1.0 - u) * (1.0 - u) * r + 2.0 * u * (1.0 - u) * control + u * u * e;
            D3 arc = glm::mix(r, e, u);
            if (bows)
            {
                const double psi = -0.5 * theta + theta * u;
                arc = centre + radius * (std::sin(psi) * c + std::cos(psi) * b);
            }
            const D3 shaped = glm::mix(smooth, arc, autoArc);
            pts[size_t(k)] = glm::mix(jointed, shaped, hose) + bend * std::sin(rigmath::kPi * u);
        }
        return pts;
    }

    std::vector<D3> finger(D3& upOut) const
    {
        const glm::dmat4 parent = as<glm::dmat4>(kBaseParentMatrix);
        const glm::dmat3 rp = rotationOf(parent);
        const D3 base = D3(parent * glm::dvec4(as<D3>(kRestBase), 1.0));
        D3 d = glm::normalize(rp * as<D3>(kRestDir));
        D3 a = acrossAxis(rp * as<D3>(kCurlAxis), d);
        a = glm::length(a) > 1e-12 ? glm::normalize(a) : anyAcross(d);

        const double fist = std::clamp(as<double>(kFist), 0.0, 1.0);
        const double curl = glm::mix(as<double>(kCurl) + as<double>(kHandCurl), as<double>(kFistCurl), fist);
        const double spread = (as<double>(kSpread) + as<double>(kHandSpread) * as<double>(kSpreadWeight)) * (1.0 - fist);
        if (const double turn = spread * as<double>(kSpreadMax); turn != 0.0)
        {
            const glm::dmat3 q(glm::rotate(glm::dmat4(1.0), rigmath::radians(turn), glm::normalize(glm::cross(a, d))));
            d = q * d;
            a = q * a;
        }
        upOut = glm::transpose(rp) * a;

        const double length = std::max(as<double>(kFingerLength), 1e-6);
        const double kappa = rigmath::radians(as<double>(kRestCurl) + curl * as<double>(kCurlMax)) / length;
        const D3 side = glm::cross(a, d);
        const int samples = samplesFor(kappa * length * kFingerOvershoot);
        std::vector<D3> pts(size_t(samples), D3(0.0));
        for (int k = 0; k < samples; ++k)
        {
            const double s = kFingerOvershoot * length * double(k) / double(samples - 1);
            pts[size_t(k)] = std::abs(kappa) < 1e-9 ? base + d * s : base + d * (std::sin(kappa * s) / kappa) + side * ((1.0 - std::cos(kappa * s)) / kappa);
        }
        return pts;
    }
};

class HoseHandNode : public Node
{
public:
    enum Attr
    {
        kCurl,
        kSpread,
        kFist,
    };
};

glm::dmat4 hoseWorld(Graph& g, NodeId id) { return std::get<glm::dmat4>(g.value({id, Transform::kWorldMatrix, -1})); }
glm::dmat4 parentWorld(Graph& g, NodeId id) { return g.parent(id) ? hoseWorld(g, g.parent(id)) : glm::dmat4(1.0); }

std::vector<NodeId> jointsDown(Graph& g, NodeId start, NodeId end)
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

/** The finger from base to its tip, following the child joint farthest from each joint. */
std::vector<NodeId> fingerJoints(Graph& g, NodeId base)
{
    std::vector<NodeId> out{base};
    for (NodeId cur = base;;)
    {
        NodeId next = kNoNode;
        double far = -1.0;
        for (NodeId c : g.children(cur))
            if (rig::isJoint(*g.node(c)))
                if (const double d = glm::length(std::get<D3>(g.value({c, Transform::kTranslate, -1}))); d > far)
                    far = d, next = c;
        if (!next)
            return out;
        out.push_back(next);
        cur = next;
    }
}

const NodeType& hoseType(const char* name)
{
    const NodeType* t = types::find(name);
    if (!t)
        throw std::logic_error(std::string("rubber hose: type ") + name + " is not registered");
    return *t;
}

/** A curve node showing what the hose node makes; the spline chain rides it. */
NodeId displayCurve(Graph& g, NodeId hose, const std::string& name)
{
    const std::string made = ops::execute("curve", {{"type", "poly"}, {"points", {{0, 0, 0}, {1, 0, 0}}}, {"name", name}}).get<std::string>();
    const NodeId c = g.find(made)->id();
    g.connect({hose, HoseLimbNode::kOutCurve, -1}, {c, curve::CurveShape::kCreate, -1}, true);
    g.evaluate();
    return c;
}

/** Sets the joint's translate and jointOrient so its world is `world` (rotate zero) under its parent. */
void placeJoint(Graph& g, NodeId j, const glm::dmat4& world)
{
    const glm::dmat4 local = glm::inverse(parentWorld(g, j)) * world;
    g.setValue({j, Transform::kTranslate, -1}, D3(local[3]));
    g.setValue({j, Joint::kJointOrient, -1}, matrixToEuler(rotationOf(local), RotateOrder::XYZ));
    g.evaluate();
}
} // namespace

void registerHoseTypes()
{
    NodeType hose{.name = kHoseType,
                  .doc = "Shapes a rubber hose spline chain's curve: a limb blending its jointed elbow or knee into an arc that keeps "
                         "the limb's length, or a finger curling along a circular arc.",
                  .icon = "wave-sine",
                  .create = [] { return std::make_unique<HoseLimbNode>(); }};
    hose.ownAttrs = {
        {.name = "mode", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kHoseHidden, .enumNames = kHoseModes, .doc = "A limb or a finger."},
        hoseAttr("chain", PlugType::String, std::string(), AttrInput | AttrStorable, "The spline chain riding the curve."),
        hoseAttr("hose", PlugType::Float, 1.0, kHoseKeyable, "0 bends at the elbow or knee, 1 is a smooth hose.", 0.0, 1.0),
        hoseAttr("autoArc", PlugType::Float, 1.0, kHoseKeyable,
                 "1: as the end nears the root the hose bows into an arc keeping its length. 0: a smooth curve through the elbow.", 0.0, 1.0),
        hoseAttr("rootMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrInput | AttrHidden, "The shoulder or hip."),
        hoseAttr("midMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrInput | AttrHidden, "The elbow or knee."),
        hoseAttr("endMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrInput | AttrHidden, "The wrist or ankle."),
        hoseAttr("bendMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrInput | AttrHidden, "The bend control; moving it pushes the middle of the hose."),
        hoseAttr("hasMid", PlugType::Bool, false, kHoseHidden, "Whether an elbow or knee joint shapes the limb."),
        hoseAttr("restLength", PlugType::Float, 0.0, kHoseHidden, "The limb's length without an elbow or knee, cm."),
        hoseAttr("pole", PlugType::Vec3, D3(0, 0, 1), kHoseHidden, "Where a straight limb bows, in the root's space."),
        hoseAttr("curl", PlugType::Float, 0.0, kHoseKeyable, "Finger: curl, 1 turns it through curlMax."),
        hoseAttr("spread", PlugType::Float, 0.0, kHoseKeyable, "Finger: sideways turn, 1 is spreadMax."),
        hoseAttr("curlMax", PlugType::Float, 270.0, AttrInput | AttrStorable, "Finger: degrees of curl at curl 1."),
        hoseAttr("spreadMax", PlugType::Float, 20.0, AttrInput | AttrStorable, "Finger: degrees of spread at spread 1."),
        hoseAttr("restCurl", PlugType::Float, 0.0, kHoseHidden, "Finger: its curl as modelled, degrees."),
        hoseAttr("handCurl", PlugType::Float, 0.0, AttrInput | AttrHidden, "The hand's curl."),
        hoseAttr("handSpread", PlugType::Float, 0.0, AttrInput | AttrHidden, "The hand's spread."),
        hoseAttr("fist", PlugType::Float, 0.0, AttrInput | AttrHidden, "The hand's fist."),
        hoseAttr("spreadWeight", PlugType::Float, 0.0, AttrInput | AttrStorable, "Finger: its share of the hand's spread, negative the other way."),
        hoseAttr("fistCurl", PlugType::Float, 1.0, AttrInput | AttrStorable, "Finger: its curl in a fist."),
        hoseAttr("baseParentMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrInput | AttrHidden, "Finger: the base joint's parent."),
        hoseAttr("restBase", PlugType::Vec3, D3(0.0), kHoseHidden, "Finger: the base in its parent's space."),
        hoseAttr("restDir", PlugType::Vec3, D3(1, 0, 0), kHoseHidden, "Finger: its first bone's direction in the base's parent's space."),
        hoseAttr("curlAxis", PlugType::Vec3, D3(0, 0, 1), kHoseHidden, "Finger: the axis it curls about, base's parent's space."),
        hoseAttr("fingerLength", PlugType::Float, 1.0, kHoseHidden, "Finger: its length, cm."),
        hoseAttr("outCurve", PlugType::Data, DataPtr(), AttrOutput | AttrHidden, "The hose's curve, world space."),
        hoseAttr("upVector", PlugType::Vec3, D3(0, 1, 0), AttrOutput | AttrHidden, "Finger: up for its chain, base's parent's space."),
    };

    NodeType hand{.name = kHandType,
                  .doc = "Drives every hose finger on a hand: curl, spread and fist.",
                  .icon = "hand-stop",
                  .create = [] { return std::make_unique<HoseHandNode>(); }};
    hand.ownAttrs = {
        hoseAttr("curl", PlugType::Float, 0.0, kHoseKeyable, "Added to every finger's curl."),
        hoseAttr("spread", PlugType::Float, 0.0, kHoseKeyable, "Fans the fingers apart by each one's spread weight."),
        hoseAttr("fist", PlugType::Float, 0.0, kHoseKeyable, "1 closes every finger to its fist curl.", 0.0, 1.0),
    };
    for (NodeType* t : {&hose, &hand})
        types::registerType(std::move(*t));
}

NodeId hoseChain(Graph& g, NodeId hose)
{
    const Node* h = g.node(hose);
    if (!h || !h->type().isA(kHoseType))
        return kNoNode;
    try
    {
        const Node* c = g.find(h->as<std::string>(HoseLimbNode::kChain));
        return c && c->type().isA(kChainType) ? c->id() : kNoNode;
    }
    catch (const std::exception&)
    {
        return kNoNode;
    }
}

NodeId createHoseLimb(Graph& g, NodeId start, NodeId end, const HoseOptions& opts)
{
    LILA_ZONE();

    if (!g.node(start) || !g.node(end) || !rig::isJoint(*g.node(start)) || !rig::isJoint(*g.node(end)))
        throw std::invalid_argument("a hose limb runs between joints");
    const std::vector<NodeId> limb = jointsDown(g, start, end);
    if (limb.size() < 2)
        throw std::invalid_argument(g.node(end)->path() + " is not below " + g.node(start)->path());
    if (opts.joints < 3 || opts.joints > 64)
        throw std::invalid_argument("a hose limb has 3 to 64 joints");

    g.evaluate();
    const D3 r = posOf(hoseWorld(g, start)), e = posOf(hoseWorld(g, end));
    const D3 chord = e - r;
    if (glm::length(chord) < 1e-6)
        throw std::invalid_argument(g.node(start)->path() + " and " + g.node(end)->path() + " are in the same place");
    const D3 c = glm::normalize(chord);
    // The elbow or knee: the joint between farthest off the line from root to end.
    NodeId mid = kNoNode;
    double off = -1.0;
    for (size_t i = 1; i + 1 < limb.size(); ++i)
        if (const double d = glm::length(acrossAxis(posOf(hoseWorld(g, limb[i])) - r, c)); d > off)
            off = d, mid = limb[i];
    const D3 m = mid ? posOf(hoseWorld(g, mid)) : 0.5 * (r + e);
    D3 bendDir = acrossAxis(m - r, c);
    if (glm::length(bendDir) < 1e-6)
    {
        const glm::dmat3 sr = rotationOf(hoseWorld(g, start));
        bendDir = D3(0.0);
        double least = 2.0;
        for (int k = 0; k < 3; ++k)
            if (const double d = std::abs(glm::dot(sr[k], c)); d < least)
                least = d, bendDir = acrossAxis(sr[k], c);
    }
    bendDir = glm::normalize(bendDir);

    const std::string base = opts.name.empty() ? g.node(start)->name() + "_hose" : opts.name;
    Node* h = g.createNode(hoseType(kHoseType), base);
    const NodeId hid = h->id();
    auto set = [&](int a, PlugValue v) { g.setValue({hid, a, -1}, std::move(v)); };
    set(HoseLimbNode::kMode, int64_t(0));
    set(HoseLimbNode::kHose, opts.hose);
    set(HoseLimbNode::kAutoArc, opts.autoArc);
    set(HoseLimbNode::kHasMid, mid != kNoNode);
    set(HoseLimbNode::kRestLength, glm::length(chord));
    set(HoseLimbNode::kPole, glm::transpose(rotationOf(hoseWorld(g, start))) * bendDir);

    // The bend control sits on the elbow (or halfway along a limb without one) and moves with it.
    const NodeId bendParent = mid ? mid : start;
    Node* bend = g.createNode(hoseType("locator"), base + "Bend", bendParent);
    g.setValue({bend->id(), Transform::kTranslate, -1}, D3(glm::inverse(hoseWorld(g, bendParent)) * glm::dvec4(m, 1.0)));
    g.connect({start, Transform::kWorldMatrix, -1}, {hid, HoseLimbNode::kRootMatrix, -1}, true);
    g.connect({end, Transform::kWorldMatrix, -1}, {hid, HoseLimbNode::kEndMatrix, -1}, true);
    if (mid)
        g.connect({mid, Transform::kWorldMatrix, -1}, {hid, HoseLimbNode::kMidMatrix, -1}, true);
    g.connect({bend->id(), Transform::kWorldMatrix, -1}, {hid, HoseLimbNode::kBendMatrix, -1}, true);
    const NodeId curveId = displayCurve(g, hid, base + "Curve");

    // Hose joints evenly along the hose as it is now, x down the limb and y toward the bend.
    const curve::Splines& splines = curve::splinesOf(g.value({curveId, curve::CurveShape::kWorldSpace, -1}));
    if (splines.empty())
        throw std::invalid_argument("the hose has no curve");
    const curve::Polyline pl = curve::polylineFrom(splines.front().points, false);
    std::vector<D3> at;
    for (int i = 0; i < opts.joints; ++i)
        at.push_back(pl.pointAtLength(pl.length * double(i) / double(opts.joints - 1)));
    const NodeId under = g.parent(start);
    const double radius = std::get<double>(g.value({start, Joint::kRadius, -1})) * 0.6;
    std::vector<NodeId> hoseJoints;
    D3 x = c;
    for (int i = 0; i < opts.joints; ++i)
    {
        if (i + 1 < opts.joints && glm::length(at[size_t(i + 1)] - at[size_t(i)]) > 1e-9)
            x = glm::normalize(at[size_t(i + 1)] - at[size_t(i)]);
        D3 y = acrossAxis(bendDir, x);
        y = glm::length(y) > 1e-9 ? glm::normalize(y) : anyAcross(x);
        glm::dmat4 w(glm::dmat3(x, y, glm::cross(x, y)));
        w[3] = glm::dvec4(at[size_t(i)], 1.0);
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "_%02d", i + 1);
        Node* j = g.createNode(hoseType("joint"), base + suffix, hoseJoints.empty() ? under : hoseJoints.back());
        g.setValue({j->id(), Joint::kRadius, -1}, radius);
        placeJoint(g, j->id(), w);
        hoseJoints.push_back(j->id());
    }
    for (NodeId j : limb)
        g.setValue({j, Joint::kDeform, -1}, false);

    ChainOptions co{.name = base + "Spline", .lengthMode = LengthMode::Squash, .upMode = UpMode::UpObjects, .upStart = start, .upEnd = end,
                    .minStretch = 0.1, .maxStretch = 10.0};
    const NodeId chain = createSplineChain(g, hoseJoints.front(), hoseJoints.back(), curveId, co);
    set(HoseLimbNode::kChain, g.node(chain)->path());
    g.evaluate();
    if (opts.reskin)
        reskinHoseLimb(g, hid);
    return hid;
}

namespace
{
/** The closest point to p on the polyline through pts: the segment and how far along it. */
std::pair<size_t, double> closestOnPolyline(const std::vector<D3>& pts, const D3& p)
{
    std::pair<size_t, double> best{0, 0.0};
    double nearest = 1e300;
    for (size_t k = 0; k + 1 < pts.size(); ++k)
    {
        const D3 ab = pts[k + 1] - pts[k];
        const double len2 = glm::dot(ab, ab);
        const double f = len2 > 1e-18 ? std::clamp(glm::dot(p - pts[k], ab) / len2, 0.0, 1.0) : 0.0;
        if (const double d = glm::length(p - (pts[k] + ab * f)); d < nearest)
            nearest = d, best = {k, f};
    }
    return best;
}
} // namespace

int reskinHoseLimb(Graph& g, NodeId hose)
{
    LILA_ZONE();

    const Node* h = g.node(hose);
    if (!h || h->type().name != kHoseType || h->as<int64_t>(HoseLimbNode::kMode) != 0)
        throw std::invalid_argument("not a hose limb");
    const auto root = g.source({hose, HoseLimbNode::kRootMatrix, -1}), tip = g.source({hose, HoseLimbNode::kEndMatrix, -1});
    const Node* chainNode = g.find(h->as<std::string>(HoseLimbNode::kChain));
    if (!root || !tip || !chainNode)
        throw std::invalid_argument(h->path() + " has lost its limb or its chain");
    const std::vector<NodeId> limb = jointsDown(g, root->node, tip->node);
    const std::vector<NodeId> hoseJoints = splineChainJoints(g, chainNode->id());
    if (limb.empty() || hoseJoints.size() < 2)
        throw std::invalid_argument(h->path() + " has lost its limb or its chain");
    g.evaluate();

    std::vector<D3> limbAt;
    for (NodeId j : limb)
        limbAt.push_back(posOf(hoseWorld(g, j)));
    // The limb joint each hose joint lies along, whose skinning it takes on.
    std::vector<NodeId> alongOf;
    for (NodeId j : hoseJoints)
    {
        const size_t k = closestOnPolyline(limbAt, posOf(hoseWorld(g, j))).first;
        alongOf.push_back(limb[std::min(k, limb.size() - 1)]);
    }

    int changed = 0;
    std::vector<NodeId> skins;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().isA("skinCluster"))
            skins.push_back(id);
    for (NodeId sc : skins)
    {
        std::vector<skin::Influence> infs = skin::influences(g, sc);
        std::map<NodeId, const skin::Influence*> byJoint;
        for (const skin::Influence& inf : infs)
            if (inf.joint != kNoNode)
                byJoint[inf.joint] = &inf;
        std::vector<uint32_t> old;
        for (NodeId j : limb)
            if (auto it = byJoint.find(j); it != byJoint.end())
                old.push_back(it->second->index);
        const MeshPtr input = skin::inputMesh(g, sc);
        if (old.empty() || !input)
            continue;
        auto skinning = [&](const skin::Influence& inf) {
            return hoseWorld(g, inf.joint) * g.node(inf.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix);
        };
        std::map<NodeId, glm::dmat4> limbSkinning;
        for (NodeId j : limb)
            limbSkinning[j] = byJoint.contains(j) ? skinning(*byJoint[j]) : glm::dmat4(1.0);

        std::vector<uint32_t> ids;
        std::vector<D3> bindAt;
        for (size_t k = 0; k < hoseJoints.size(); ++k)
        {
            const bool had = byJoint.contains(hoseJoints[k]);
            ids.push_back(skin::addInfluence(g, sc, hoseJoints[k]));
            glm::dmat4 bindPre(1.0);
            for (const skin::Influence& inf : skin::influences(g, sc))
                if (inf.index == ids.back())
                {
                    if (!had)
                        g.setValue({inf.link, SkinInfluenceNode::kBindPreMatrix, -1},
                                   glm::inverse(hoseWorld(g, hoseJoints[k])) * limbSkinning[alongOf[k]]);
                    bindPre = g.node(inf.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix);
                }
            bindAt.push_back(posOf(glm::inverse(bindPre)));
        }

        WeightTable t = skin::table(g, sc);
        const glm::dmat4 geom = g.node(sc)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix);
        const auto pts = input->positions();
        bool moved = false;
        for (size_t p = 0; p < t.points.size() && p < pts.size(); ++p)
        {
            float share = 0.0f;
            for (uint32_t o : old)
                share += t.get(p, o);
            if (share <= 0.0f)
                continue;
            for (uint32_t o : old)
                t.set(p, o, 0.0f);
            const auto [k, f] = closestOnPolyline(bindAt, D3(geom * glm::dvec4(D3(pts[p]), 1.0)));
            t.set(p, ids[k], t.get(p, ids[k]) + share * float(1.0 - f));
            t.set(p, ids[k + 1], t.get(p, ids[k + 1]) + share * float(f));
            moved = true;
        }
        if (!moved)
            continue;
        skin::setWeights(g, sc, t);
        ++changed;
    }
    g.evaluate();
    return changed;
}

std::vector<NodeId> createHoseFingers(Graph& g, const std::vector<NodeId>& bases, const FingerOptions& opts)
{
    LILA_ZONE();

    if (bases.empty())
        throw std::invalid_argument("give the base joint of each finger");
    std::vector<std::vector<NodeId>> fingers;
    for (NodeId b : bases)
    {
        if (!g.node(b) || !rig::isJoint(*g.node(b)))
            throw std::invalid_argument("a finger starts at a joint");
        fingers.push_back(fingerJoints(g, b));
        if (fingers.back().size() < 2)
            throw std::invalid_argument(g.node(b)->path() + " has no joint below it");
    }
    const glm::dvec3 axisLocal = rig::axisVector(opts.curlAxis);

    g.evaluate();
    const NodeId handJoint = g.parent(bases.front());
    const std::string handName = opts.name.empty() ? (handJoint ? g.node(handJoint)->name() : g.node(bases.front())->name()) + "_hoseHand" : opts.name;
    Node* hand = g.createNode(hoseType(kHandType), handName);
    const NodeId handId = hand->id();
    std::vector<NodeId> out{handId};

    for (size_t f = 0; f < fingers.size(); ++f)
    {
        const std::vector<NodeId>& js = fingers[f];
        const NodeId base = js.front();
        const NodeId parent = g.parent(base);
        const glm::dmat4 pw = parentWorld(g, base);
        const glm::dmat3 rp = rotationOf(pw);
        std::vector<D3> p;
        for (NodeId j : js)
            p.push_back(posOf(hoseWorld(g, j)));
        double length = 0.0;
        for (size_t i = 1; i < p.size(); ++i)
            length += glm::length(p[i] - p[i - 1]);
        const D3 first = glm::normalize(p[1] - p[0]), last = glm::normalize(p.back() - p[p.size() - 2]);
        D3 a = acrossAxis(rotationOf(hoseWorld(g, base)) * axisLocal, first);
        if (glm::length(a) < 1e-6)
            throw std::invalid_argument(g.node(base)->path() + "'s " + opts.curlAxis + " axis runs along the finger; pick another curlAxis");
        a = glm::normalize(a);

        const std::string name = g.node(base)->name() + "_hose";
        Node* h = g.createNode(hoseType(kHoseType), name);
        const NodeId hid = h->id();
        auto set = [&](int at, PlugValue v) { g.setValue({hid, at, -1}, std::move(v)); };
        set(HoseLimbNode::kMode, int64_t(1));
        set(HoseLimbNode::kCurlMax, opts.curlMax);
        set(HoseLimbNode::kSpreadMax, opts.spreadMax);
        set(HoseLimbNode::kRestCurl, rigmath::signedAngle(first, last, a));
        set(HoseLimbNode::kRestBase, D3(glm::inverse(pw) * glm::dvec4(p[0], 1.0)));
        set(HoseLimbNode::kRestDir, glm::transpose(rp) * first);
        set(HoseLimbNode::kCurlAxis, glm::transpose(rp) * a);
        set(HoseLimbNode::kFingerLength, length);
        set(HoseLimbNode::kSpreadWeight, fingers.size() > 1 ? -1.0 + 2.0 * double(f) / double(fingers.size() - 1) : 0.0);
        if (parent)
            g.connect({parent, Transform::kWorldMatrix, -1}, {hid, HoseLimbNode::kBaseParentMatrix, -1}, true);
        g.connect({handId, HoseHandNode::kCurl, -1}, {hid, HoseLimbNode::kHandCurl, -1}, true);
        g.connect({handId, HoseHandNode::kSpread, -1}, {hid, HoseLimbNode::kHandSpread, -1}, true);
        g.connect({handId, HoseHandNode::kFist, -1}, {hid, HoseLimbNode::kFist, -1}, true);
        const NodeId curveId = displayCurve(g, hid, name + "Curve");

        ChainOptions co{.name = name + "Spline", .lengthMode = LengthMode::KeepLength, .upMode = UpMode::UpVector, .upVector = glm::transpose(rp) * a};
        const NodeId chain = createSplineChain(g, base, js.back(), curveId, co);
        g.connect({hid, HoseLimbNode::kUpVector, -1}, {chain, g.node(chain)->type().attrIndex("upVector"), -1}, true);
        set(HoseLimbNode::kChain, g.node(chain)->path());
        out.push_back(hid);
    }
    g.evaluate();
    return out;
}

// Commands --------------------------------------------------------------------------------------------------------

namespace
{
using rigutil::graph;
using rigutil::nodeNamed;

template <class F> auto hoseGuard(const char* cmd, F&& f)
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

std::vector<NodeId> jointArgs(Graph& g, const Args& a, const char* cmd)
{
    std::vector<std::string> objs = a.objects();
    if (objs.empty())
        for (NodeId id : g.selection())
            objs.push_back(g.node(id)->path());
    std::vector<NodeId> out;
    for (const std::string& o : objs)
    {
        Node& n = nodeNamed(o, cmd);
        if (!rig::isJoint(n))
            throw CommandError(std::string(cmd) + ": " + n.name() + " is not a joint");
        out.push_back(n.id());
    }
    return out;
}

json hoseLimbCmd(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> js = jointArgs(g, a, "hoseLimb");
    if (js.size() < 2)
        throw CommandError("hoseLimb: give the shoulder or hip, then the wrist or ankle");
    HoseOptions o{.name = a.get<std::string>("name", ""), .joints = a.get<int>("joints", 12), .hose = a.get<double>("hose", 1.0),
                  .autoArc = a.get<double>("autoArc", 1.0), .reskin = a.get<bool>("reskin", true)};
    const NodeId h = hoseGuard("hoseLimb", [&] { return createHoseLimb(g, js.front(), js.back(), o); });
    g.select({h});
    return g.node(h)->path();
}

json hoseReskinCmd(const Args& a)
{
    Graph& g = graph();
    std::vector<std::string> objs = a.objects();
    if (objs.empty())
        for (NodeId id : g.selection())
            objs.push_back(g.node(id)->path());
    if (objs.empty())
        throw CommandError("hoseReskin: give hose limbs");
    int changed = 0;
    for (const std::string& o : objs)
    {
        const NodeId id = nodeNamed(o, "hoseReskin").id();
        changed += hoseGuard("hoseReskin", [&] { return reskinHoseLimb(g, id); });
    }
    return changed;
}

json hoseFingersCmd(const Args& a)
{
    Graph& g = graph();
    const std::vector<NodeId> js = jointArgs(g, a, "hoseFingers");
    if (js.empty())
        throw CommandError("hoseFingers: give the base joint of each finger");
    FingerOptions o{.name = a.get<std::string>("name", ""), .curlAxis = a.get<std::string>("curlAxis", "z"),
                    .curlMax = a.get<double>("curlMax", 270.0), .spreadMax = a.get<double>("spreadMax", 20.0)};
    const std::vector<NodeId> made = hoseGuard("hoseFingers", [&] { return createHoseFingers(g, js, o); });
    g.select({made.front()});
    json out = json::array();
    for (NodeId id : made)
        out.push_back(g.node(id)->path());
    return out;
}

void registerHoseOptionBoxes()
{
    using ops::options::Override;
    Override l;
    l.order = {"joints", "hose", "autoArc", "reskin", "name"};
    l.hidden = {"name"};
    l.labels = {{"hose", "Hose Amount"}, {"autoArc", "Auto Arc"}, {"reskin", "Move Skin Weights"}};
    l.ranges = {{"joints", {3.0, 32.0, 1.0, ""}}, {"hose", {0.0, 1.0, 0.05, ""}}, {"autoArc", {0.0, 1.0, 0.05, ""}}};
    ops::options::setOverride("hoseLimb", std::move(l));
    Override f;
    f.order = {"curlAxis", "curlMax", "spreadMax", "name"};
    f.hidden = {"name"};
    f.labels = {{"curlAxis", "Curl Axis"}, {"curlMax", "Full Curl"}, {"spreadMax", "Full Spread"}};
    f.ranges = {{"curlMax", {0.0, 360.0, 5.0, " deg"}}, {"spreadMax", {0.0, 90.0, 1.0, " deg"}}};
    ops::options::setOverride("hoseFingers", std::move(f));
}
} // namespace

void registerHoseCommands()
{
    if (ops::find("hoseLimb"))
        return;
    using T = ArgType;
    ops::registerCommand({
        .name = "hoseLimb",
        .doc = "Make a rubber hose limb from the shoulder or hip to the wrist or ankle. A limb with an elbow or knee between keeps "
               "its joints as drivers (no longer deforming) and gets a chain of hose joints that follow it, from jointed (hose 0) to a "
               "smooth hose that bows into an arc as the end nears the root (hose 1). Without joints between, the hose spans the two "
               "joints. A bend control on the elbow pushes the middle of the hose. The limb joints' skin weights move onto the hose "
               "joints (reskin). Returns the hoseLimb.",
        .positionalDoc = "start joint, end joint (default the selection)",
        .flags = {{"joints", "j", T::Int, 12, "Hose joints along the limb.", {}, {}, 3.0, 64.0},
                  {"hose", "h", T::Float, 1.0, "0 bends at the elbow or knee, 1 is a smooth hose.", {}, {}, 0.0, 1.0},
                  {"autoArc", "aa", T::Float, 1.0, "1 bows into an arc keeping the limb's length as the end nears the root.", {}, {}, 0.0, 1.0},
                  {"reskin", "rs", T::Bool, true, "Move the limb joints' skin weights onto the hose joints, as hoseReskin does."},
                  {"name", "n", T::String, nullptr, "Name for the hoseLimb; default <start>_hose."}},
        .run = hoseLimbCmd,
        .examples = {"hoseLimb(\"upperarm_l\", \"hand_l\", joints=12)"},
    });
    ops::registerCommand({
        .name = "hoseReskin",
        .doc = "Move the skin weights of hose limbs' limb joints onto their hose joints: each point's weight goes to the two hose "
               "joints either side of where it sits along the hose, so the skin follows the hose. Run it with the limb in its bind "
               "pose, where the skin then deforms exactly as before. Returns how many skinClusters changed.",
        .positionalDoc = "hoseLimb nodes (default the selection)",
        .run = hoseReskinCmd,
        .examples = {"hoseReskin(\"upperarm_l_hose\")"},
    });
    ops::registerCommand({
        .name = "hoseFingers",
        .doc = "Make rubber hose fingers: each base joint's finger, down to its tip, curls along an arc. Each finger's hoseLimb has a "
               "curl and spread; one hoseHand adds curl, spread and fist to all of them. Returns the hoseHand, then the fingers.",
        .positionalDoc = "finger base joints (default the selection)",
        .flags = {{"curlAxis", "ca", T::String, "z", "The base joint's axis the finger curls about: x, y, z, -x, -y or -z."},
                  {"curlMax", "cm", T::Float, 270.0, "Degrees the whole finger curls at curl 1."},
                  {"spreadMax", "sm", T::Float, 20.0, "Degrees a finger turns sideways at spread 1."},
                  {"name", "n", T::String, nullptr, "Name for the hoseHand; default <hand>_hoseHand."}},
        .run = hoseFingersCmd,
        .examples = {"hoseFingers(\"index_01_l\", \"middle_01_l\", \"ring_01_l\", \"pinky_01_l\")"},
    });
    registerHoseOptionBoxes();
}
} // namespace lila::scene::spline
