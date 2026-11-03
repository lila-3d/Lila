#include "ui/RigHandles.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "curve/CurveControls.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Ik.h"
#include "scene/Rig.h"
#include "scene/SplineChain.h"
#include "ui/Actions.h"
#include "ui/NodeHandles.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "scene/SceneIndex.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lila::ui::rigHandles
{
namespace
{
using scene::Joint;
using scene::Node;
using scene::NodeId;
using D3 = glm::dvec3;

constexpr double kPi = std::numbers::pi;
constexpr float kDot = 4.5f;
constexpr double kPickPx = 8.0;

const glm::vec4 kHinge{1.0f, 0.62f, 0.2f, 1.0f};
const glm::vec4 kCone{0.3f, 0.75f, 1.0f, 1.0f};
const glm::vec4 kCurrent{0.95f, 0.95f, 0.95f, 1.0f};
const glm::vec4 kHot{1.0f, 1.0f, 1.0f, 1.0f};
const glm::vec4 kChain{0.25f, 0.85f, 0.8f, 1.0f};
const glm::vec4 kHose{1.0f, 0.45f, 0.6f, 1.0f};

glm::vec4 faded(glm::vec4 c, float a)
{
    c.a = a;
    return c;
}

D3 across(const D3& v, const D3& axis) { return v - axis * glm::dot(v, axis); }

D3 anyAcross(const D3& axis)
{
    const D3 t = std::abs(axis.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0);
    return glm::normalize(across(t, axis));
}

D3 unit(const D3& v, const D3& fallback) { return glm::length(v) > 1e-12 ? glm::normalize(v) : fallback; }

D3 rotated(const D3& v, const D3& axis, double degrees) { return glm::angleAxis(glm::radians(degrees), axis) * v; }

double displayScale() { return PrefStore::get().value<double>("general", "/rig/jointDisplayScale", 1.0); }

bool showAll() { return PrefStore::get().value<bool>("general", kShowAllLimits, false); }

// IK limits ------------------------------------------------------------------------------------------------------

/** A joint's limit laid out in world space. */
struct Limit
{
    int64_t kind = Joint::kNoLimit;
    D3 origin{0.0};
    /** Hinge: its axis; cone: unused. World, unit. */
    D3 axis{0, 0, 1};
    /** Where the bone points at rotate zero (across the axis for a hinge). World, unit. */
    D3 zero{1, 0, 0};
    /** Cone: where the bone points now. */
    D3 bone{1, 0, 0};
    double radius = 1.0;
    double minDeg = 0.0, maxDeg = 0.0, coneDeg = 0.0, currentDeg = 0.0, preferredDeg = 0.0;
};

std::optional<Limit> limitOf(scene::Graph& g, NodeId id)
{
    const Node* n = g.node(id);
    if (!n || !scene::rig::isJoint(*n))
        return std::nullopt;
    Limit l;
    l.kind = std::clamp<int64_t>(n->as<int64_t>(Joint::kLimitType), 0, 2);
    if (l.kind == Joint::kNoLimit)
        return std::nullopt;
    glm::dmat4 before, after;
    scene::Transform::splitLocal(*n, before, after);
    const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dmat4 frameW = n->as<glm::dmat4>(scene::Transform::kParentMatrix) * before;
    glm::dmat3 frame(frameW);
    for (int i = 0; i < 3; ++i)
        frame[i] = unit(frame[i], D3(i == 0, i == 1, i == 2));
    l.origin = D3(world[3]);

    // The bone in rotate space: toward the first child joint, else down the joint's x axis.
    D3 bone = D3(after * glm::dvec4(1, 0, 0, 0));
    double length = 0.0;
    for (NodeId c : g.children(id))
        if (const Node* cn = g.node(c); cn && scene::rig::isJoint(*cn))
        {
            const D3 local(cn->as<glm::dmat4>(scene::Transform::kMatrix)[3]);
            const D3 b = D3(after * glm::dvec4(local, 1.0)) - D3(after[3]);
            if (glm::length(b) > 1e-9)
            {
                bone = b;
                length = glm::length(D3(cn->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]) - l.origin);
                break;
            }
        }
    const D3 zeroWorld = unit(frame * bone, D3(1, 0, 0));
    const glm::dmat3 rot(static_cast<const Joint*>(n)->rotation());
    const double jointR = n->as<double>(Joint::kRadius) * displayScale();
    l.radius = std::max(length * 0.5, jointR * 3.0);
    l.minDeg = n->as<double>(Joint::kLimitMin);
    l.maxDeg = n->as<double>(Joint::kLimitMax);
    l.coneDeg = n->as<double>(Joint::kConeAngle);
    if (l.kind == Joint::kHinge)
    {
        const int ax = int(std::clamp<int64_t>(n->as<int64_t>(Joint::kLimitAxis), 0, 2));
        D3 a(0.0);
        a[ax] = 1.0;
        l.axis = unit(frame * a, D3(0, 0, 1));
        l.zero = unit(across(zeroWorld, l.axis), anyAcross(l.axis));
        glm::dquat q = glm::normalize(glm::quat_cast(rot));
        if (q.w < 0.0)
            q = -q;
        l.currentDeg = glm::degrees(2.0 * std::atan2(glm::dot(D3(q.x, q.y, q.z), a), q.w));
        if (l.currentDeg > 180.0)
            l.currentDeg -= 360.0;
        l.preferredDeg = n->as<D3>(Joint::kPreferredAngle)[ax];
    }
    else
    {
        l.zero = zeroWorld;
        l.bone = unit(frame * (rot * bone), zeroWorld);
        l.currentDeg = glm::degrees(std::acos(std::clamp(glm::dot(l.bone, l.zero), -1.0, 1.0)));
    }
    return l;
}

/** The side of a cone its handle sits on: across the bone and the view, so it stays in sight. */
D3 coneSide(const Limit& l, const ViewInfo& view)
{
    return unit(glm::cross(l.zero, view.viewDir(l.origin)), anyAcross(l.zero));
}

D3 hingePoint(const Limit& l, double deg, double scale = 1.0) { return l.origin + rotated(l.zero, l.axis, deg) * l.radius * scale; }

D3 conePoint(const Limit& l, const D3& side, double deg)
{
    const double c = glm::radians(deg);
    return l.origin + (l.zero * std::cos(c) + side * std::sin(c)) * l.radius;
}

std::optional<D3> handleWorld(const Limit& l, LimitHandle h, const ViewInfo& view)
{
    if (l.kind == Joint::kHinge && h == kLimitMin)
        return hingePoint(l, l.minDeg);
    if (l.kind == Joint::kHinge && h == kLimitMax)
        return hingePoint(l, l.maxDeg);
    if (l.kind == Joint::kCone && h == kLimitCone)
        return conePoint(l, coneSide(l, view), l.coneDeg);
    return std::nullopt;
}

std::vector<QPointF> projected(const ViewInfo& view, const std::vector<D3>& pts)
{
    std::vector<QPointF> out;
    for (const D3& p : pts)
        if (auto s = view.project(p))
            out.push_back(*s);
        else
            return {};
    return out;
}

void drawLimit(OverlayPainter& p, const ViewInfo& view, const Limit& l, int hovered, int active, bool handles)
{
    const auto o = view.project(l.origin);
    if (!o)
        return;
    auto handleColor = [&](int h, const glm::vec4& c) { return h == hovered || h == active ? kHot : c; };
    if (l.kind == Joint::kHinge)
    {
        const double lo = std::min(l.minDeg, l.maxDeg), hi = std::max(l.minDeg, l.maxDeg);
        const int segs = std::max(4, int((hi - lo) / 6.0));
        std::vector<D3> arc;
        for (int k = 0; k <= segs; ++k)
            arc.push_back(hingePoint(l, lo + (hi - lo) * k / segs));
        const std::vector<QPointF> px = projected(view, arc);
        if (px.empty())
            return;
        for (size_t k = 0; k + 1 < px.size(); ++k)
            p.triangle(*o, px[k], px[k + 1], faded(kHinge, 0.14f));
        p.polyline(px, 2.0f, kHinge);
        p.line(*o, px.front(), 1.5f, faded(kHinge, 0.8f));
        p.line(*o, px.back(), 1.5f, faded(kHinge, 0.8f));
        if (l.preferredDeg != 0.0)
            if (auto pref = view.project(hingePoint(l, l.preferredDeg, 0.8)))
                p.dashedLine(*o, *pref, 1.5f, 4.0f, faded(kCurrent, 0.7f));
        if (auto cur = view.project(hingePoint(l, l.currentDeg, 1.15)))
            p.line(*o, *cur, 2.0f, kCurrent);
        if (handles)
        {
            const QPointF& minAt = l.minDeg <= l.maxDeg ? px.front() : px.back();
            const QPointF& maxAt = l.minDeg <= l.maxDeg ? px.back() : px.front();
            p.disc(minAt, kDot, handleColor(kLimitMin, kHinge));
            p.disc(maxAt, kDot, handleColor(kLimitMax, kHinge));
        }
        return;
    }
    const D3 side = coneSide(l, view), up = glm::cross(l.zero, side);
    const double c = glm::radians(l.coneDeg);
    std::vector<D3> rim;
    for (int k = 0; k < 36; ++k)
    {
        const double phi = 2.0 * kPi * k / 36;
        rim.push_back(l.origin + (l.zero * std::cos(c) + (side * std::cos(phi) + up * std::sin(phi)) * std::sin(c)) * l.radius);
    }
    const std::vector<QPointF> px = projected(view, rim);
    if (px.empty())
        return;
    for (size_t k = 0; k < px.size(); ++k)
        p.triangle(*o, px[k], px[(k + 1) % px.size()], faded(kCone, 0.08f));
    p.polyline(px, 2.0f, kCone, true);
    for (size_t k = 0; k < px.size(); k += 9)
        p.line(*o, px[k], 1.25f, faded(kCone, 0.7f));
    if (auto axisEnd = view.project(l.origin + l.zero * l.radius))
        p.dashedLine(*o, *axisEnd, 1.5f, 4.0f, faded(kCone, 0.8f));
    if (auto cur = view.project(l.origin + l.bone * l.radius * 1.15))
        p.line(*o, *cur, 2.0f, kCurrent);
    if (handles)
        p.disc(px.front(), kDot, handleColor(kLimitCone, kCone));
}

/** Plug edits on one joint with undo off while dragging, the IK handles solving it refreshed with them. */
class LimitEdit
{
public:
    explicit LimitEdit(NodeId joint) : mJoint(joint)
    {
        scene::Graph& g = scene::Graph::current();
        for (int a : {int(Joint::kLimitMin), int(Joint::kLimitMax), int(Joint::kConeAngle)})
            mStart.push_back({a, g.storedValue({joint, a})});
    }

    void set(int attr, double v)
    {
        scene::Graph& g = scene::Graph::current();
        if (!g.node(mJoint))
            return;
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        g.setValue({mJoint, attr}, v);
        refresh(g);
        u.setEnabled(was);
        viewportTools::requestRedraw();
    }

    void restore()
    {
        scene::Graph& g = scene::Graph::current();
        if (!g.node(mJoint))
            return;
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        for (const auto& [a, v] : mStart)
            g.setValue({mJoint, a}, v);
        refresh(g);
        u.setEnabled(was);
        viewportTools::requestRedraw();
    }

    /** Puts the start values back and sets the changed ones with one ikLimit call. */
    void commit()
    {
        scene::Graph& g = scene::Graph::current();
        const Node* n = g.node(mJoint);
        if (!n)
            return;
        nlohmann::json call = {{"args", {n->path()}}};
        static constexpr std::pair<int, const char*> kFlags[] = {{Joint::kLimitMin, "min"}, {Joint::kLimitMax, "max"}, {Joint::kConeAngle, "cone"}};
        for (const auto& [a, v] : mStart)
            if (const scene::PlugValue& now = g.storedValue({mJoint, a}); now != v)
                for (const auto& [attr, flag] : kFlags)
                    if (attr == a)
                        call[flag] = std::get<double>(now);
        restore();
        if (call.size() == 1)
            return;
        try
        {
            ops::execute("ikLimit", call, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
        }
    }

private:
    void refresh(scene::Graph& g)
    {
        for (NodeId h : scene::ik::handlesOf(g, mJoint))
            scene::ik::refresh(g, h);
    }

    NodeId mJoint;
    std::vector<std::pair<int, scene::PlugValue>> mStart;
};

class LimitDrag final : public nodeHandles::Drag
{
public:
    LimitDrag(LimitHandle h, const Limit& l, NodeId joint, const ViewInfo& view)
        : mHandle(h), mLimit(l), mSide(coneSide(l, view)), mJoint(joint), mEdit(joint)
    {
    }

    void move(QPointF px, const ViewInfo& view, Qt::KeyboardModifiers mods) override
    {
        D3 o, d;
        view.ray(px, o, d);
        const D3 normal = mHandle == kLimitCone ? unit(glm::cross(mLimit.zero, mSide), D3(0, 0, 1)) : mLimit.axis;
        const double denom = glm::dot(d, normal);
        if (std::abs(denom) < 1e-6)
            return;
        const D3 v = o + d * (glm::dot(mLimit.origin - o, normal) / denom) - mLimit.origin;
        if (glm::length(v) < 1e-9)
            return;
        const double step = mods & Qt::ControlModifier ? 5.0 : 0.5;
        auto snap = [&](double deg) { return std::round(deg / step) * step; };
        if (mHandle == kLimitCone)
        {
            const double deg = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(v), mLimit.zero), -1.0, 1.0)));
            mEdit.set(Joint::kConeAngle, std::clamp(snap(deg), 0.0, 180.0));
            return;
        }
        const double deg = std::clamp(snap(glm::degrees(std::atan2(glm::dot(glm::cross(mLimit.zero, v), mLimit.axis), glm::dot(mLimit.zero, v)))), -180.0, 180.0);
        if (mHandle == kLimitMin)
            mEdit.set(Joint::kLimitMin, std::min(deg, mLimit.maxDeg));
        else
            mEdit.set(Joint::kLimitMax, std::max(deg, mLimit.minDeg));
    }

    void release(bool moved) override
    {
        if (moved)
            mEdit.commit();
    }

    void cancel() override { mEdit.restore(); }

    QString readout() const override
    {
        const Node* n = scene::Graph::current().node(mJoint);
        if (!n)
            return {};
        auto deg = [](double v) { return QString::number(v, 'f', 1) + QChar(0x00B0); };
        switch (mHandle)
        {
        case kLimitMin: return "Min " + deg(n->as<double>(Joint::kLimitMin));
        case kLimitMax: return "Max " + deg(n->as<double>(Joint::kLimitMax));
        case kLimitCone: return "Cone " + deg(n->as<double>(Joint::kConeAngle));
        }
        return {};
    }

private:
    LimitHandle mHandle;
    Limit mLimit;
    D3 mSide;
    NodeId mJoint;
    LimitEdit mEdit;
};

int pickLimit(QPointF px, const nodeHandles::Context& c)
{
    const std::optional<Limit> l = limitOf(scene::Graph::current(), c.node.id());
    if (!l)
        return -1;
    int best = -1;
    double bestD = kPickPx;
    for (LimitHandle h : {kLimitMin, kLimitMax, kLimitCone})
        if (auto w = handleWorld(*l, h, c.view))
            if (auto s = c.view.project(*w))
                if (const double d = std::hypot(s->x() - px.x(), s->y() - px.y()); d < bestD)
                {
                    bestD = d;
                    best = h;
                }
    return best;
}

/** Whether nodeHandles draws the selected joints' limits (object mode, no tool), so the drawer leaves them to it. */
bool handlesShown() { return !viewportTools::active() && scene::components::mode() == scene::ComponentMode::Object; }

void drawAllLimits(render::FrameState&, OverlayPainter& p, const ViewInfo& view)
{
    if (!showAll())
        return;
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const bool handled = handlesShown();
    for (NodeId id : g.nodes())
    {
        const Node* n = g.node(id);
        if (!n || !scene::rig::isJoint(*n) || (handled && g.isSelected(id)) || !scene::effectivelyVisible(g, id) ||
            n->as<int64_t>(Joint::kDrawStyle) == Joint::kHidden || viewportHooks::hiddenInView(id))
            continue;
        if (const std::optional<Limit> l = limitOf(g, id))
            drawLimit(p, view, *l, -1, -1, false);
    }
}

// Spline chains and hoses ----------------------------------------------------------------------------------------

enum class Kind
{
    Chain,
    Hose,
    Finger,
    Hand,
};

/** A rig node drawn through its glyph: where it anchors in the world, and which way the glyph sits off it on screen. */
struct Item
{
    NodeId node = scene::kNoNode;
    Kind kind = Kind::Chain;
    D3 anchor{0.0};
    QPointF offset{14, -14};
};

int attr(const Node& n, const char* name) { return n.type().attrIndex(name); }

const curve::Splines* curveOf(const Node& n, const char* plug)
{
    const int a = attr(n, plug);
    return a < 0 ? nullptr : &curve::splinesOf(n.value(a));
}

curve::Polyline polylineOf(const curve::Spline& s)
{
    curve::TessOptions o;
    o.chordError = 0.01;
    return curve::tessellate(s, o);
}

glm::dmat4 matrixOf(const Node& n, const char* plug)
{
    const int a = attr(n, plug);
    return a < 0 ? glm::dmat4(1.0) : n.as<glm::dmat4>(a);
}

NodeId sourceNode(scene::Graph& g, const Node& n, const char* plug)
{
    const int a = attr(n, plug);
    if (a < 0)
        return scene::kNoNode;
    const std::optional<scene::PlugRef> s = g.source({n.id(), a, -1});
    return s ? s->node : scene::kNoNode;
}

bool shown(scene::Graph& g, NodeId id)
{
    const Node* n = id ? g.node(id) : nullptr;
    return n && scene::effectivelyVisible(g, id) && !viewportHooks::hiddenInView(id);
}

/** The hose limb's bow: the side its middle bends to, the same way the hose node picks it. */
D3 hoseBow(const Node& n)
{
    const glm::dmat4 rootW = matrixOf(n, "rootMatrix");
    const D3 r(rootW[3]), e(matrixOf(n, "endMatrix")[3]);
    const bool hasMid = n.as<bool>(attr(n, "hasMid"));
    const D3 m = hasMid ? D3(matrixOf(n, "midMatrix")[3]) : 0.5 * (r + e);
    const D3 c = unit(e - r, D3(1, 0, 0));
    D3 b = across(m - r, c);
    if (glm::length(b) < 1e-6 * std::max(glm::length(e - r), 1.0))
        b = across(glm::dmat3(rootW) * n.as<D3>(attr(n, "pole")), c);
    return unit(b, anyAcross(c));
}

/** The hose's anchor: off the middle of its chord toward the bow, a third of the limb out. */
D3 hoseAnchor(const Node& n)
{
    const D3 r(matrixOf(n, "rootMatrix")[3]), e(matrixOf(n, "endMatrix")[3]);
    return 0.5 * (r + e) + hoseBow(n) * std::max(glm::length(e - r), 1.0) * 0.3;
}

std::vector<Item> items(scene::Graph& g)
{
    std::vector<Item> out;
    const auto rigNode = [](const Node& n) {
        const std::string& type = n.type().name;
        return type == scene::spline::kChainType || type == scene::spline::kHoseType || type == scene::spline::kHandType;
    };
    for (const Node* n : scene::sceneIndex::matching(g, "rigHandleItems", rigNode))
    {
        const NodeId id = n->id();
        const std::string& type = n->type().name;
        if (type == scene::spline::kChainType)
        {
            const std::vector<NodeId> joints = scene::spline::splineChainJoints(g, id);
            const curve::Splines* s = curveOf(*n, "inCurve");
            if (joints.empty() || !shown(g, joints.front()) || !s || s->empty())
                continue;
            const curve::Polyline pl = polylineOf(s->front());
            if (pl.points.empty())
                continue;
            out.push_back({id, Kind::Chain, pl.pointAtLength(pl.length * 0.5)});
        }
        else if (type == scene::spline::kHoseType)
        {
            if (n->as<int64_t>(attr(*n, "mode")) == 1)
            {
                const curve::Splines* s = curveOf(*n, "outCurve");
                if (!s || s->empty() || s->front().points.empty() || !shown(g, sourceNode(g, *n, "baseParentMatrix")))
                    continue;
                // The curve runs past the fingertip by a quarter, so the tip is four fifths along.
                const std::vector<D3>& pts = s->front().points;
                out.push_back({id, Kind::Finger, pts[size_t(double(pts.size() - 1) * 0.8)], {10, -10}});
            }
            else if (shown(g, sourceNode(g, *n, "rootMatrix")))
                out.push_back({id, Kind::Hose, hoseAnchor(*n)});
        }
        else if (type == scene::spline::kHandType)
        {
            D3 sum(0.0);
            int count = 0;
            NodeId handJoint = scene::kNoNode;
            for (const scene::PlugRef& d : g.destinations({id, attr(*n, "curl"), -1}))
                if (const Node* f = g.node(d.node))
                {
                    sum += D3(matrixOf(*f, "baseParentMatrix")[3]);
                    ++count;
                    handJoint = handJoint ? handJoint : sourceNode(g, *f, "baseParentMatrix");
                }
            if (count && shown(g, handJoint))
                out.push_back({id, Kind::Hand, sum / double(count), {-18, -18}});
        }
    }
    return out;
}

glm::vec4 itemColor(Kind k) { return k == Kind::Chain ? kChain : kHose; }

void drawGlyph(OverlayPainter& p, const Item& it, QPointF anchor, bool selected)
{
    const QPointF at = anchor + it.offset;
    const glm::vec4 c = selected ? kHot : itemColor(it.kind);
    p.line(anchor, at, 1.0f, faded(c, 0.6f));
    const float r = it.kind == Kind::Finger ? 4.0f : 5.5f;
    switch (it.kind)
    {
    case Kind::Chain:
        p.fill({at + QPointF(0, -r), at + QPointF(r, 0), at + QPointF(0, r), at + QPointF(-r, 0)}, c);
        break;
    case Kind::Hand: p.rect(QRectF(at - QPointF(r, r), at + QPointF(r, r)), c); break;
    default: p.disc(at, r, c); break;
    }
}

bool anySelected(scene::Graph& g, const std::vector<NodeId>& ids)
{
    return std::ranges::any_of(ids, [&](NodeId id) { return id && g.isSelected(id); });
}

/** A chain's curve and how its joints sit on it, while the chain, its curve, its controls or its joints are selected. */
void drawChain(scene::Graph& g, OverlayPainter& p, const ViewInfo& view, const Node& chain)
{
    const curve::Splines* s = curveOf(chain, "inCurve");
    if (!s || s->empty())
        return;
    const std::vector<NodeId> joints = scene::spline::splineChainJoints(g, chain.id());
    std::vector<NodeId> related = joints;
    related.push_back(chain.id());
    if (const NodeId c = sourceNode(g, chain, "inCurve"))
    {
        related.push_back(c);
        if (const Node* cn = g.node(c); cn && curve::hasControls(*cn))
            for (NodeId ctl : curve::curveControlNodes(g, *cn))
                related.push_back(ctl);
    }
    if (!anySelected(g, related))
        return;
    for (const curve::Spline& sp : *s)
        if (const std::vector<QPointF> px = projected(view, polylineOf(sp).points); px.size() > 1)
            p.dashedPolyline(px, 2.0f, 6.0f, faded(kChain, 0.9f));
    for (NodeId j : joints)
    {
        const D3 at(g.node(j)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        D3 on;
        curve::closestParam(s->front(), at, &on);
        auto a = view.project(at), b = view.project(on);
        if (!a || !b)
            continue;
        p.disc(*b, 2.5f, kChain);
        if (std::hypot(a->x() - b->x(), a->y() - b->y()) > 2.0)
            p.dashedLine(*a, *b, 1.0f, 3.0f, faded(kChain, 0.8f));
    }
}

/** A hose limb's bend control relation and bow direction, while the hose, its bend control or its ends are selected. */
void drawHose(scene::Graph& g, OverlayPainter& p, const ViewInfo& view, const Node& hose, const Item& it)
{
    const NodeId bend = sourceNode(g, hose, "bendMatrix");
    if (!anySelected(g, {hose.id(), bend, sourceNode(g, hose, "rootMatrix"), sourceNode(g, hose, "endMatrix"), sourceNode(g, hose, "midMatrix")}))
        return;
    const D3 r(matrixOf(hose, "rootMatrix")[3]), e(matrixOf(hose, "endMatrix")[3]);
    const bool hasMid = hose.as<bool>(attr(hose, "hasMid"));
    const D3 m = hasMid ? D3(matrixOf(hose, "midMatrix")[3]) : 0.5 * (r + e);
    if (bend)
        if (auto a = view.project(m), b = view.project(D3(matrixOf(hose, "bendMatrix")[3])); a && b)
            p.dashedLine(*a, *b, 1.5f, 4.0f, kHose);
    auto from = view.project(0.5 * (r + e)), to = view.project(it.anchor);
    if (!from || !to)
        return;
    p.dashedLine(*from, *to, 1.5f, 5.0f, faded(kHose, 0.85f));
    const QPointF d = *to - *from;
    if (const double len = std::hypot(d.x(), d.y()); len > 8.0)
    {
        const QPointF u = d / len, n(-u.y(), u.x());
        p.triangle(*to, *to - u * 9.0 + n * 4.5, *to - u * 9.0 - n * 4.5, kHose);
    }
}

/** Lines from a hand to its fingers, while the hand or a finger is selected. */
void drawHand(scene::Graph& g, OverlayPainter& p, const ViewInfo& view, const Item& hand, const std::vector<Item>& all)
{
    std::vector<const Item*> fingers;
    std::vector<NodeId> related{hand.node};
    const Node* n = g.node(hand.node);
    for (const scene::PlugRef& d : g.destinations({hand.node, attr(*n, "curl"), -1}))
        for (const Item& it : all)
            if (it.node == d.node)
            {
                fingers.push_back(&it);
                related.push_back(it.node);
            }
    if (!anySelected(g, related))
        return;
    auto from = view.project(hand.anchor);
    if (!from)
        return;
    for (const Item* f : fingers)
        if (auto to = view.project(f->anchor))
            p.dashedLine(*from + hand.offset, *to + f->offset, 1.0f, 4.0f, faded(kHose, 0.7f));
}

void drawRigNodes(render::FrameState&, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const std::vector<Item> all = items(g);
    for (const Item& it : all)
    {
        const Node* n = g.node(it.node);
        if (it.kind == Kind::Chain)
            drawChain(g, p, view, *n);
        else if (it.kind == Kind::Hose)
            drawHose(g, p, view, *n, it);
        else if (it.kind == Kind::Hand)
            drawHand(g, p, view, it, all);
    }
    for (const Item& it : all)
        if (auto a = view.project(it.anchor))
            drawGlyph(p, it, *a, g.isSelected(it.node));
}

std::vector<viewportHooks::PickHit> pickRigNodes(QPointF a, QPointF, bool click, const ViewInfo& view)
{
    std::vector<viewportHooks::PickHit> hits;
    if (!click)
        return hits;
    scene::Graph& g = scene::Graph::current();
    for (const Glyph& gl : glyphs(view))
        if (const double d = std::hypot(gl.at.x() - a.x(), gl.at.y() - a.y()); d < kPickPx)
            hits.push_back({g.node(gl.node)->path(), d});
    return hits;
}
} // namespace

std::optional<QPointF> limitHandleAt(NodeId joint, LimitHandle h, const ViewInfo& view)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const std::optional<Limit> l = limitOf(g, joint);
    if (!l)
        return std::nullopt;
    const std::optional<D3> w = handleWorld(*l, h, view);
    return w ? view.project(*w) : std::nullopt;
}

std::vector<Glyph> glyphs(const ViewInfo& view)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::vector<Glyph> out;
    for (const Item& it : items(g))
        if (auto a = view.project(it.anchor))
            out.push_back({it.node, *a + it.offset});
    return out;
}

void registerRigHandles()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    nodeHandles::add({
        .id = "lila.ikLimits",
        .nodeType = "joint",
        .draw =
            [](OverlayPainter& p, const nodeHandles::Context& c, int hovered, int active) {
                if (const std::optional<Limit> l = limitOf(scene::Graph::current(), c.node.id()))
                    drawLimit(p, c.view, *l, hovered, active, true);
            },
        .pick = pickLimit,
        .begin = [](int handle, QPointF, const nodeHandles::Context& c) -> std::unique_ptr<nodeHandles::Drag> {
            const std::optional<Limit> l = limitOf(scene::Graph::current(), c.node.id());
            if (!l)
                return nullptr;
            return std::make_unique<LimitDrag>(LimitHandle(handle), *l, c.node.id(), c.view);
        },
    });
    viewportHooks::addDrawer(drawAllLimits);
    viewportHooks::addDrawer(drawRigNodes);
    viewportHooks::addPicker(pickRigNodes);
    actions::add({.id = "rig.showIkLimits", .label = "Show All IK Limits", .category = "Rig",
                  .run = [] {
                      PrefStore::get().set("general", kShowAllLimits, !showAll());
                      viewportTools::requestRedraw();
                  },
                  .doc = "Draws the IK limits of every joint that has them; the selected joints always show theirs, with handles to drag.",
                  .checked = showAll});
}
} // namespace lila::ui::rigHandles
