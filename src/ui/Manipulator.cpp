#include "ui/Manipulator.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/CenterInMesh.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "geo/Mesh.h"
#include "ui/GizmoMath.h"
#include "ui/Isolate.h"
#include "ui/SurfaceSnap.h"
#include "ui/Theme.h"
#include "ui/ViewportDisplay.h"

#include <QKeyEvent>

#include <glm/gtc/matrix_transform.hpp>

#include <iterator>
#include <map>
#include <numbers>
#include <unordered_set>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::NodeId;
using scene::Transform;
using H = Manipulator::Handle;

constexpr double kPi = std::numbers::pi;
constexpr int kSavedAttrs[] = {Transform::kTranslate,           Transform::kRotate,     Transform::kScale,
                               Transform::kRotatePivot,         Transform::kRotatePivotTranslate, Transform::kScalePivot,
                               Transform::kScalePivotTranslate, Transform::kPivotOrient};
constexpr int kSavedCount = int(std::size(kSavedAttrs));

const glm::vec4 kGray(0.70f, 0.70f, 0.70f, 1.0f);
const glm::vec4 kHot(1.0f, 1.0f, 1.0f, 1.0f);
const glm::vec4 kTrail(0.95f, 0.95f, 0.95f, 0.8f);
const glm::vec4 kLocked(0.42f, 0.42f, 0.42f, 0.55f);

double pref(const char* key, double fallback) { return PrefStore::get().value<double>("general", key, fallback); }

scene::Space spaceFor(Tool t)
{
    std::string key = t == Tool::Rotate ? "/manipulator/rotateSpace" : "/manipulator/moveSpace";
    std::string s = PrefStore::get().value<std::string>("general", key, "world");
    // Normal has no meaning for whole objects, so they move on their own axes.
    return s == "object" || s == "normal" ? scene::Space::Object : s == "parent" ? scene::Space::Parent : scene::Space::World;
}

bool normalSpace() { return PrefStore::get().value<std::string>("general", "/manipulator/moveSpace", "world") == "normal"; }

/** "world", "object" or "component" (the selection's average normal). Objects always scale on their own axes. */
std::string scaleSpace() { return PrefStore::get().value<std::string>("general", "/manipulator/scaleSpace", "world"); }

/** A pivot placed with Edit Pivot on a component selection, shared by every viewport; it lasts until that selection changes. */
struct ComponentPivot
{
    scene::ComponentMode mode = scene::ComponentMode::Object;
    std::map<NodeId, scene::components::Ids> selection;
    glm::dvec3 position{0.0};
    std::optional<glm::dmat3> frame;
};
std::optional<ComponentPivot> gComponentPivot;

/** A frame whose Z is the normal; X and Y are any perpendicular pair. */
glm::dmat3 normalFrame(const glm::dvec3& n)
{
    glm::dvec3 x = glm::normalize(glm::cross(std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0), n));
    return glm::dmat3(x, glm::cross(n, x), n);
}

int axisOf(H h)
{
    switch (h)
    {
    case H::X: case H::PlaneYZ: return 0;
    case H::Y: case H::PlaneXZ: return 1;
    case H::Z: case H::PlaneXY: return 2;
    default: return -1;
    }
}

bool isPlane(H h) { return h == H::PlaneYZ || h == H::PlaneXZ || h == H::PlaneXY; }

/** Pivot editing moves the pivot with the move tool and orients it with the rotate tool. */
Tool pivotTool(bool pivotEdit, Tool t)
{
    if (!pivotEdit)
        return t;
    return t == Tool::Rotate ? Tool::Rotate : Tool::Move;
}

double dist(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

double segmentDistance(QPointF p, QPointF a, QPointF b)
{
    QPointF d = b - a;
    double len2 = QPointF::dotProduct(d, d);
    double t = len2 > 0 ? std::clamp(QPointF::dotProduct(p - a, d) / len2, 0.0, 1.0) : 0.0;
    return dist(p, a + d * t);
}

bool insideConvex(QPointF p, const std::vector<QPointF>& poly)
{
    int sign = 0;
    for (size_t i = 0; i < poly.size(); ++i)
    {
        QPointF a = poly[i], b = poly[(i + 1) % poly.size()];
        double c = (b.x() - a.x()) * (p.y() - a.y()) - (b.y() - a.y()) * (p.x() - a.x());
        int s = c > 0 ? 1 : c < 0 ? -1 : 0;
        if (s != 0 && sign != 0 && s != sign)
            return false;
        if (s != 0)
            sign = s;
    }
    return true;
}

/** Math-convention angle (counterclockwise on screen) of a pixel around a center. */
double screenAngle(QPointF center, QPointF p) { return std::atan2(-(p.y() - center.y()), p.x() - center.x()); }

double wrapPi(double a)
{
    while (a > kPi)
        a -= 2 * kPi;
    while (a < -kPi)
        a += 2 * kPi;
    return a;
}

using gizmo::rayPlane;

/** The frame axes a handle moves along: one for an axis, two for a plane, all three for the center. */
glm::dvec3 moveMask(H h)
{
    int ai = axisOf(h);
    glm::dvec3 mask(1.0);
    if (h >= H::X && h <= H::Z)
        mask = glm::dvec3(0.0), mask[ai] = 1.0;
    else if (isPlane(h))
        mask[ai] = 0.0;
    return mask;
}

/** Hold X/C/V/N: Maya's grid and point snaps and surface sliding, which win over the toolbar's increment snapping. */
bool heldSnap() { return tools::snaps() & (SnapGrid | SnapEdge | SnapVertex | SnapSurface); }

using gizmo::lineParam;

double tidy(double v) { return std::round(v * 1e4) / 1e4 + 0.0; }

int channelAttr(Tool t)
{
    return t == Tool::Rotate ? Transform::kRotate : t == Tool::Scale ? Transform::kScale : Transform::kTranslate;
}

/** Bits of the channels that moving, turning or scaling object id along world axis `dir` changes, the way the commands apply it. */
uint8_t channelsAlong(Graph& g, NodeId id, Tool t, const glm::dvec3& dir, int frameAxis)
{
    scene::Node* n = g.node(id);
    glm::dvec3 change(0.0);
    if (t == Tool::Scale)
        change[frameAxis] = 1.0;
    else if (t == Tool::Rotate)
    {
        // A small turn shows which Euler channels follow; closestEuler keeps it from wrapping.
        glm::dvec3 now = n->as<glm::dvec3>(Transform::kRotate);
        change = scene::xformops::rotatedEuler(g, id, glm::angleAxis(glm::radians(1.0), dir)) - now;
    }
    else
        change = glm::dvec3(glm::inverse(n->as<glm::dmat4>(Transform::kParentMatrix)) * glm::dvec4(dir, 0.0));
    double largest = std::max({std::abs(change.x), std::abs(change.y), std::abs(change.z)});
    uint8_t bits = 0;
    for (int c = 0; c < 3; ++c)
        if (std::abs(change[c]) > largest * 1e-6)
            bits |= uint8_t(1u << c);
    return bits;
}

json vecJson(const glm::dvec3& v) { return json::array({tidy(v.x), tidy(v.y), tidy(v.z)}); }

std::string nameOf(NodeId id)
{
    scene::Node* n = Graph::current().node(id);
    return n ? n->path() : std::string();
}
} // namespace

// ------------------------------------------------------------------- state

bool Manipulator::supports(std::string_view mode) const
{
    return mode == "select" || mode == "move" || mode == "rotate" || mode == "scale";
}

Tool Manipulator::effectiveTool() const
{
    if (mDrag)
        return pivotTool(mDrag->pivotEdit, mDrag->tool);
    return pivotTool(tools::pivotEdit(), tools::current());
}

void Manipulator::refresh()
{
    if (mDrag)
        return;
    Graph& g = Graph::current();
    mTargets.clear();
    mLead = scene::kNoNode;
    mComponents = false;
    mNormal = false;
    if (tools::current() == Tool::Select && !tools::pivotEdit())
        return;

    // Component mode: the manipulator sits at the selected components' centre.
    scene::ComponentMode mode = scene::components::mode();
    if (mode != scene::ComponentMode::Object)
    {
        for (auto& [node, ids] : scene::components::selected(mode))
            if (!ids.empty() && g.node(node))
                mTargets.push_back(node);
        if (mTargets.empty())
            return;
        mComponents = true;
        mLead = mTargets.back();
        mPivot = scene::selectionCenter();
        const Tool t = pivotTool(tools::pivotEdit(), tools::current());
        if (t == Tool::Scale)
        {
            const std::string s = scaleSpace();
            mFrame = s == "component" ? normalFrame(scene::selectionNormal())
                                      : scene::xformops::frame(g, mLead, s == "object" ? scene::Space::Object : scene::Space::World);
        }
        else
            mFrame = scene::xformops::frame(g, mLead, spaceFor(t));
        if (t == Tool::Move && normalSpace() && !tools::pivotEdit())
        {
            mNormal = true;
            mFrame = scene::selectionNormalFrame();
        }
        if (gComponentPivot && (gComponentPivot->mode != mode || gComponentPivot->selection != scene::components::selected(mode)))
            gComponentPivot.reset();
        if (gComponentPivot)
        {
            mPivot = gComponentPivot->position;
            if (gComponentPivot->frame && !mNormal)
                mFrame = *gComponentPivot->frame;
        }
        return;
    }
    mTargets = scene::xformops::topLevel(g, g.selection());
    // Maya puts the manipulator on the last selected object.
    for (auto it = g.selection().rbegin(); it != g.selection().rend(); ++it)
        if (scene::Node* n = g.node(*it); n && n->type().isA("transform"))
        {
            mLead = *it;
            break;
        }
    if (mLead == scene::kNoNode)
        return;
    mPivot = scene::xformops::worldPivot(g, mLead);
    Tool t = pivotTool(tools::pivotEdit(), tools::current());
    scene::Space space = t == Tool::Scale ? scene::Space::Object : spaceFor(t);
    // Object space is the pivot's own orientation; scale stays on the object's axes, which is what scaleBy uses.
    bool pivotAxes = (tools::pivotEdit() && t == Tool::Rotate) || (space == scene::Space::Object && t != Tool::Scale);
    mFrame = pivotAxes ? scene::xformops::pivotFrame(g, mLead) : scene::xformops::frame(g, mLead, space);
    refreshLocks(t);
}

void Manipulator::refreshLocks(Tool t)
{
    mLockedAxes = glm::bvec3(false);
    mAllLocked = false;
    if (mComponents || tools::pivotEdit() || mTargets.empty())
        return;
    // Each object keeps its own locked channels, so an axis is locked only where no target can move along it.
    Graph& g = Graph::current();
    const int attr = channelAttr(t);
    glm::bvec3 locked(true);
    bool all = true;
    for (NodeId id : mTargets)
    {
        uint8_t mask = g.node(id)->lockMask(attr) & 7u;
        all = all && mask == 7u;
        for (int i = 0; i < 3; ++i)
            locked[i] = locked[i] && mask != 0 && (channelsAlong(g, id, t, mFrame[i], i) & ~mask) == 0;
    }
    mLockedAxes = locked;
    mAllLocked = all;
}

bool Manipulator::handleLocked(H h) const
{
    int ai = axisOf(h);
    if (h >= H::X && h <= H::Z)
        return mLockedAxes[ai];
    if (isPlane(h))
        return mLockedAxes[(ai + 1) % 3] && mLockedAxes[(ai + 2) % 3];
    if (h == H::Center)
        return glm::all(mLockedAxes);
    if (h == H::ViewRing || h == H::Free)
        return mAllLocked;
    return false;
}

void Manipulator::maskLocked()
{
    Drag& d = *mDrag;
    for (int i = 0; i < 3; ++i)
        if (mLockedAxes[i])
        {
            d.moveDelta -= d.frame[i] * glm::dot(d.moveDelta, d.frame[i]);
            d.scale[i] = 1.0;
        }
}

bool Manipulator::visible()
{
    refresh();
    return mLead != scene::kNoNode && !mTargets.empty();
}

double Manipulator::handleLength() const
{
    return pref("/manipulator/size", 100.0) * mView.unitsPerPixel(mPivot);
}

// ------------------------------------------------------------------- picking

std::vector<QPointF> Manipulator::ringPoints(int i, bool frontOnly) const
{
    std::vector<QPointF> pts;
    glm::dvec3 a = axis(i);
    glm::dvec3 u = axis((i + 1) % 3), v = axis((i + 2) % 3);
    double r = handleLength() * 0.85;
    glm::dvec3 toEye = glm::normalize(mView.eye - mPivot);
    (void)a;
    const int n = 72;
    for (int k = 0; k <= n; ++k)
    {
        double t = 2 * kPi * k / n;
        glm::dvec3 dir = u * std::cos(t) + v * std::sin(t);
        if (frontOnly && glm::dot(dir, toEye) < -0.02)
        {
            pts.push_back(QPointF(qQNaN(), qQNaN()));
            continue;
        }
        auto p = mView.project(mPivot + dir * r);
        pts.push_back(p ? *p : QPointF(qQNaN(), qQNaN()));
    }
    return pts;
}

H Manipulator::pick(QPointF px) const
{
    auto c = mView.project(mPivot);
    if (!c)
        return H::None;
    const double grab = 8.0;
    double L = handleLength();
    Tool t = effectiveTool();
    glm::dvec3 viewDir = mView.viewDir(mPivot);

    if (t == Tool::Move && mNormal)
    {
        if (std::abs(px.x() - c->x()) <= 8 && std::abs(px.y() - c->y()) <= 8)
            return H::Center;
        for (int i = 0; i < 3; ++i)
            if (auto tip = mView.project(mPivot + axis(i) * L); tip && segmentDistance(px, *c, *tip) < grab)
                return H(int(H::X) + i);
        return H::None;
    }
    if (t == Tool::Move || t == Tool::Scale)
    {
        if (std::abs(px.x() - c->x()) <= 8 && std::abs(px.y() - c->y()) <= 8)
            return H::Center;
        if (t == Tool::Move)
            for (int i = 0; i < 3; ++i)
            {
                glm::dvec3 a = axis((i + 1) % 3), b = axis((i + 2) % 3);
                if (std::abs(glm::dot(axis(i), viewDir)) < 0.12)
                    continue;
                std::vector<QPointF> quad;
                for (auto [s, u] : {std::pair{0.25, 0.25}, {0.45, 0.25}, {0.45, 0.45}, {0.25, 0.45}})
                    if (auto p = mView.project(mPivot + (a * s + b * u) * L))
                        quad.push_back(*p);
                if (quad.size() == 4 && insideConvex(px, quad))
                    return H(int(H::PlaneYZ) + i);
            }
        H best = H::None;
        double bestD = grab;
        for (int i = 0; i < 3; ++i)
        {
            auto tip = mView.project(mPivot + axis(i) * L);
            if (!tip || dist(*tip, *c) < 12)
                continue;
            double d = segmentDistance(px, *c, *tip);
            if (d < bestD)
            {
                bestD = d;
                best = H(int(H::X) + i);
            }
        }
        return best;
    }

    // Rotate: rings first, then the view ring, then the trackball inside.
    H best = H::None;
    double bestD = grab;
    for (int i = 0; i < 3; ++i)
    {
        auto pts = ringPoints(i, true);
        for (size_t k = 0; k + 1 < pts.size(); ++k)
        {
            if (std::isnan(pts[k].x()) || std::isnan(pts[k + 1].x()))
                continue;
            double d = segmentDistance(px, pts[k], pts[k + 1]);
            if (d < bestD)
            {
                bestD = d;
                best = H(int(H::X) + i);
            }
        }
    }
    if (best != H::None)
        return best;
    double ringPx = 0.85 * pref("/manipulator/size", 100.0);
    double r = dist(px, *c);
    if (std::abs(r - ringPx * 1.18) < grab)
        return H::ViewRing;
    if (r < ringPx)
        return H::Free;
    return H::None;
}

bool Manipulator::hover(QPointF px)
{
    if (dragging())
        return false;
    ExtrudeHandles::Handle e = mExtrude.pick(px, mView);
    H h = e == ExtrudeHandles::Handle::None && visible() ? pick(px) : H::None;
    if (h == mHover && e == mExtrudeHover)
        return false;
    mHover = h;
    mExtrudeHover = e;
    return true;
}

// ------------------------------------------------------------------- dragging

void Manipulator::syncHandleResets()
{
    if (handlesCurrent())
        return;
    mHandleResets = tools::handleResets();
    mLastHandle = H::None;
    mLastExtrude = ExtrudeHandles::Handle::None;
}

bool Manipulator::handlesCurrent() const
{
    return mHandleResets == tools::handleResets();
}

bool Manipulator::press(QPointF px, Qt::MouseButton button, Qt::KeyboardModifiers mods)
{
    syncHandleResets();
    if (button == Qt::LeftButton)
        if (ExtrudeHandles::Handle e = mExtrude.pick(px, mView); e != ExtrudeHandles::Handle::None)
        {
            mExtrude.begin(e, px, mView);
            mExtrudeHover = e;
            if (mExtrude.dragging())
            {
                mLastExtrude = e;
                mLastHandle = H::None;
            }
            return mExtrude.dragging();
        }
    const bool ctrlMiddleMove = button == Qt::MiddleButton && (mods & Qt::ControlModifier) && !(mods & Qt::ShiftModifier) &&
                                effectiveTool() == Tool::Move && !tools::pivotEdit() && !mNormal;
    // A middle drag off the handles keeps working the extrude handle used last, as it does a move axis.
    if (button == Qt::MiddleButton && !(mods & Qt::ShiftModifier) && !ctrlMiddleMove && mLastExtrude != ExtrudeHandles::Handle::None)
    {
        if (!mExtrude.active())
            mLastExtrude = ExtrudeHandles::Handle::None;
        else if (!visible() || pick(px) == H::None)
        {
            mExtrude.begin(mLastExtrude, px, mView);
            return mExtrude.dragging();
        }
    }
    if (!visible())
        return false;
    H h = pick(px);
    bool axisFromDrag = button == Qt::MiddleButton && (mods & Qt::ShiftModifier);
    const bool surfaceSlide = ctrlMiddleMove && h == H::None;
    if (surfaceSlide)
        h = H::Center;
    // Ctrl on an axis isolates it away, like Maya: move or scale in the plane of the other two.
    const bool ctrlPlane = (mods & Qt::ControlModifier) && h >= H::X && h <= H::Z && effectiveTool() != Tool::Rotate;
    if (ctrlPlane)
        h = H(int(H::PlaneYZ) + axisOf(h));
    if (axisFromDrag)
        h = effectiveTool() == Tool::Rotate ? H::Free : H::Center;
    else if (button == Qt::MiddleButton)
    {
        Tool t = effectiveTool();
        if (h == H::None)
            h = mLastHandle != H::None ? mLastHandle : (t == Tool::Rotate ? H::Free : H::Center);
        // A handle from another tool makes no sense here.
        if (t != Tool::Rotate && (h == H::ViewRing || h == H::Free))
            h = H::Center;
        if (t == Tool::Rotate && (isPlane(h) || h == H::Center))
            h = H::Free;
    }
    else if (button != Qt::LeftButton || h == H::None)
        return false;
    const bool alongNormal = mNormal && effectiveTool() == Tool::Move && !tools::pivotEdit();
    // Taken so it neither drags nor falls through to a selection click.
    if (handleLocked(h))
    {
        mHover = h;
        return true;
    }

    Graph& g = Graph::current();
    Drag d;
    d.handle = h;
    d.pressPx = d.virtualPx = d.lastPx = px;
    d.startPivot = mPivot;
    d.frame = mFrame;
    d.pivotEdit = tools::pivotEdit();
    d.components = mComponents && !d.pivotEdit;
    d.componentPivot = mComponents && d.pivotEdit;
    if (d.componentPivot && !gComponentPivot)
    {
        const scene::ComponentMode mode = scene::components::mode();
        gComponentPivot = ComponentPivot{mode, scene::components::selected(mode), mPivot, std::nullopt};
    }
    d.tool = tools::current();
    d.axisFromDrag = d.axisPending = axisFromDrag;
    d.ctrlPlane = ctrlPlane && !axisFromDrag && button == Qt::LeftButton;
    d.alongNormal = alongNormal;
    d.surfaceSlide = surfaceSlide;
    if (d.components)
    {
        // Live edits write the base mesh directly; resolving it freezes a procedural source first (its own undo step).
        for (NodeId id : mTargets)
        {
            Saved s;
            s.id = id;
            s.plug = scene::modifiers::editPlug(g, id);
            s.values[0] = g.storedValue(s.plug);
            d.saved.push_back(s);
        }
    }
    else if (!d.componentPivot)
        for (NodeId id : mTargets)
        {
            Saved s;
            s.id = id;
            for (int k = 0; k < kSavedCount; ++k)
                s.values[k] = g.storedValue({id, kSavedAttrs[k]});
            d.saved.push_back(s);
            d.startPivots.push_back(scene::xformops::worldPivot(g, id));
            d.startPivotFrames.push_back(scene::xformops::pivotFrame(g, id));
            d.startFrames.push_back(scene::xformops::frame(g, id, scene::Space::Object));
        }
    if (pivotTool(d.pivotEdit, d.tool) == Tool::Move && !d.pivotEdit && !d.componentPivot && !d.alongNormal)
    {
        for (const Saved& s : d.saved)
        {
            d.surfaceExclude.push_back(s.id);
            if (!d.components)
                for (NodeId c : g.descendants(s.id))
                    d.surfaceExclude.push_back(c);
        }
        for (NodeId id : surfaceSnap::surfaces())
            if (std::find(d.surfaceExclude.begin(), d.surfaceExclude.end(), id) == d.surfaceExclude.end())
                d.conformTo.push_back(id);
        if (!d.conformTo.empty())
            d.conform = [ids = d.conformTo](const glm::dvec3& p) {
                auto hit = surfaceSnap::closestAmong(p, ids);
                return hit ? hit->position : p;
            };
    }

    glm::dvec3 o, dir;
    mView.ray(px, o, dir);
    Tool t = pivotTool(d.pivotEdit, d.tool);
    if (t == Tool::Move)
    {
        int ai = axisOf(h);
        if (h >= H::X && h <= H::Z)
            d.startHit.x = lineParam(mPivot, axis(ai), o, dir);
        else
        {
            d.edgeOnPlane = isPlane(h) && std::abs(glm::dot(axis(ai), mView.viewDir(mPivot))) < 0.05;
            glm::dvec3 n = isPlane(h) && !d.edgeOnPlane ? axis(ai) : mView.viewDir(mPivot);
            if (!rayPlane(o, dir, mPivot, n, d.startHit))
                d.startHit = mPivot;
        }
    }
    else if (t == Tool::Rotate)
    {
        auto c = mView.project(mPivot);
        d.startAngle = c ? screenAngle(*c, px) : 0.0;
        if (h >= H::X && h <= H::Z)
        {
            // Remember where on the ring we grabbed, for edge-on drags that pull along its tangent.
            auto pts = ringPoints(axisOf(h), true);
            double best = 1e30;
            int bestK = 0;
            for (size_t k = 0; k < pts.size(); ++k)
                if (!std::isnan(pts[k].x()) && dist(pts[k], px) < best)
                {
                    best = dist(pts[k], px);
                    bestK = int(k);
                }
            double tparam = 2 * kPi * bestK / 72.0;
            int i = axisOf(h);
            d.startHit = mPivot + (axis((i + 1) % 3) * std::cos(tparam) + axis((i + 2) % 3) * std::sin(tparam)) * handleLength() * 0.85;
        }
    }

    mDrag = std::move(d);
    if (!axisFromDrag && !surfaceSlide)
    {
        mLastHandle = h;
        mLastExtrude = ExtrudeHandles::Handle::None;
    }
    mHover = h;
    return true;
}

void Manipulator::pickAxisFromDrag(QPointF px)
{
    Drag& d = *mDrag;
    QPointF drag = px - d.pressPx;
    if (std::hypot(drag.x(), drag.y()) < 6.0)
        return;
    auto c = mView.project(d.startPivot);
    if (!c)
        return;
    int ai = -1;
    glm::dvec2 dragV(drag.x(), drag.y());
    if (pivotTool(d.pivotEdit, d.tool) == Tool::Rotate)
    {
        glm::dvec3 camRight(mView.view[0][0], mView.view[1][0], mView.view[2][0]);
        glm::dvec3 camUp(mView.view[0][1], mView.view[1][1], mView.view[2][1]);
        glm::dvec3 ideal = gizmo::trackballAxis(dragV, camRight, camUp);
        for (int i = 0; i < 3; ++i)
            if (!mLockedAxes[i] && (ai < 0 || std::abs(glm::dot(d.frame[i], ideal)) > std::abs(glm::dot(d.frame[ai], ideal))))
                ai = i;
        if (ai < 0)
            return;
        d.linearRotate = true;
        d.linearDir = drag / std::hypot(drag.x(), drag.y());
        d.linearSign = glm::dot(d.frame[ai], ideal) < 0.0 ? -1.0 : 1.0;
    }
    else
    {
        glm::dvec2 axes[3];
        for (int i = 0; i < 3; ++i)
        {
            auto tip = mView.project(d.startPivot + d.frame[i] * handleLength());
            axes[i] = tip && !mLockedAxes[i] ? glm::dvec2(tip->x() - c->x(), tip->y() - c->y()) : glm::dvec2(0.0);
        }
        ai = gizmo::axisAlongDrag(axes, dragV, 12.0);
        if (ai < 0)
            return;
        glm::dvec3 o, dir;
        mView.ray(d.pressPx, o, dir);
        d.startHit.x = lineParam(d.startPivot, d.frame[ai], o, dir);
    }
    d.handle = H(int(H::X) + ai);
    d.axisPending = false;
    mLastHandle = mHover = d.handle;
}

glm::dvec3 Manipulator::snapGrid(const glm::dvec3& p, const glm::dvec3& mask) const
{
    double step = pref("/grid/spacing", 10.0);
    glm::dvec3 out = p;
    for (int i = 0; i < 3; ++i)
        if (mask[i] != 0.0)
            out[i] = std::round(p[i] / step) * step;
    return out;
}

void Manipulator::update(QPointF px, Qt::KeyboardModifiers mods)
{
    Drag& d = *mDrag;
    // Shift slows the drag down for precision, except on Shift+MMB where it picks the axis.
    double factor = (mods & Qt::ShiftModifier) && !d.axisFromDrag ? 0.1 : 1.0;
    d.precise = factor < 1.0;
    d.virtualPx += (px - d.lastPx) * factor;
    d.lastPx = px;
    QPointF cur = d.virtualPx;
    if (d.axisPending)
    {
        pickAxisFromDrag(cur);
        if (d.axisPending)
            return;
    }
    bool gridSnap = tools::snaps() & SnapGrid;
    IncrementSnap inc = heldSnap() ? IncrementSnap{} : tools::incrementSnap(pivotTool(d.pivotEdit, d.tool));

    glm::dvec3 o, dir;
    mView.ray(cur, o, dir);
    Tool t = pivotTool(d.pivotEdit, d.tool);
    H h = d.handle;
    int ai = axisOf(h);
    bool worldFrame = d.frame == glm::dmat3(1.0);

    if (t == Tool::Move)
    {
        glm::dvec3 delta(0.0);
        if (h >= H::X && h <= H::Z)
            delta = d.frame[ai] * (lineParam(d.startPivot, d.frame[ai], o, dir) - d.startHit.x);
        else
        {
            glm::dvec3 hit;
            if (d.edgeOnPlane)
            {
                // A plane seen edge-on can't be hit reliably: drag in the view plane and drop the isolated axis.
                if (rayPlane(o, dir, d.startPivot, mView.viewDir(d.startPivot), hit))
                    delta = gizmo::constrainToPlane(hit - d.startHit, d.frame[ai]);
            }
            else if (rayPlane(o, dir, d.startPivot, isPlane(h) ? d.frame[ai] : mView.viewDir(d.startPivot), hit))
                delta = hit - d.startHit;
        }
        if (gridSnap)
        {
            if (worldFrame)
                delta = snapGrid(d.startPivot + delta, moveMask(h)) - d.startPivot;
            else
            {
                glm::dvec3 local = glm::transpose(d.frame) * delta;
                delta = d.frame * snapGrid(local, glm::dvec3(1.0));
            }
        }
        else if (inc.enabled)
            delta = inc.absolute ? gizmo::snapAbsolute(d.startPivot, delta, d.frame, moveMask(h), inc.step)
                                 : gizmo::snapRelative(delta, d.frame, moveMask(h), inc.step);
        std::optional<glm::dvec3> snap = snapTarget(cur, tools::snaps());
        d.alignRotations.clear();
        if (!snap && (d.surfaceSlide || (tools::snaps() & SnapSurface)))
            snap = surfaceSlide(cur);
        if (snap)
        {
            // Point snaps keep the handle's constraint: an axis drag slides to the snap point's projection.
            glm::dvec3 full = *snap - d.startPivot;
            if (h >= H::X && h <= H::Z)
                delta = d.frame[ai] * glm::dot(full, d.frame[ai]);
            else if (isPlane(h))
                delta = full - d.frame[ai] * glm::dot(full, d.frame[ai]);
            else
                delta = full;
        }
        if (d.conform && !d.components)
            delta = d.conform(d.startPivot + delta) - d.startPivot;
        d.moveDelta = delta;
    }
    else if (t == Tool::Rotate)
    {
        auto c = mView.project(d.startPivot);
        if (!c)
            return;
        glm::dvec3 toEye = glm::normalize(mView.eye - d.startPivot);
        double theta = 0.0;
        glm::dvec3 a(0, 1, 0);
        if (h == H::Free)
        {
            glm::dvec3 camRight(mView.view[0][0], mView.view[1][0], mView.view[2][0]);
            glm::dvec3 camUp(mView.view[0][1], mView.view[1][1], mView.view[2][1]);
            QPointF delta = cur - d.pressPx;
            double k = glm::radians(0.5);
            glm::dquat q = glm::angleAxis(delta.x() * k, camUp) * glm::angleAxis(delta.y() * k, camRight);
            d.angle = glm::degrees(glm::angle(q));
            d.rotateAxis = glm::axis(q);
            if (inc.enabled)
            {
                d.angle = gizmo::snapAngle(d.angle, inc.step);
                q = glm::angleAxis(glm::radians(d.angle), d.rotateAxis);
            }
            d.rotation = q;
        }
        else
        {
            if (h == H::ViewRing)
            {
                a = toEye;
                theta = wrapPi(screenAngle(*c, cur) - d.startAngle);
            }
            else
            {
                a = d.frame[ai];
                double facing = glm::dot(a, toEye);
                if (d.linearRotate)
                    theta = QPointF::dotProduct(cur - d.pressPx, d.linearDir) * glm::radians(0.5) * d.linearSign;
                else if (std::abs(facing) > 0.2)
                    theta = wrapPi(screenAngle(*c, cur) - d.startAngle) * (facing > 0 ? 1.0 : -1.0);
                else
                {
                    // Edge-on ring: screen angles degenerate, so pull along the ring's tangent at the grab point.
                    glm::dvec3 tangent = glm::normalize(glm::cross(a, d.startHit - d.startPivot));
                    auto g0 = mView.project(d.startHit), g1 = mView.project(d.startHit + tangent * handleLength() * 0.1);
                    if (g0 && g1)
                    {
                        QPointF sd = *g1 - *g0;
                        double len = std::hypot(sd.x(), sd.y());
                        double ringPx = 0.85 * pref("/manipulator/size", 100.0);
                        if (len > 1e-6)
                            theta = QPointF::dotProduct(cur - d.pressPx, sd / len) / ringPx;
                    }
                }
            }
            // Accumulate past +-180 by tracking the unwrapped angle.
            double prev = glm::radians(d.angle);
            double unwrapped = prev + wrapPi(theta - prev);
            if (h != H::ViewRing && (d.linearRotate || std::abs(glm::dot(a, toEye)) <= 0.2))
                unwrapped = theta;
            double deg = glm::degrees(unwrapped);
            if (gridSnap)
            {
                double step = pref("/manipulator/rotateStep", 15.0);
                deg = std::round(deg / step) * step;
            }
            else if (inc.enabled)
                deg = gizmo::snapAngle(deg, inc.step);
            d.angle = deg;
            d.rotateAxis = a;
            d.rotation = glm::angleAxis(glm::radians(deg), a);
        }
    }
    else
    {
        auto c = mView.project(d.startPivot);
        if (!c)
            return;
        glm::dvec3 s(1.0);
        if (h == H::Center)
        {
            double f = 1.0 + (cur.x() - d.pressPx.x()) / 100.0;
            s = glm::dvec3(f);
        }
        else if (ai >= 0)
        {
            // A plane scales its two axes together, pulled along their diagonal.
            glm::dvec3 along = isPlane(h) ? d.frame[(ai + 1) % 3] + d.frame[(ai + 2) % 3] : d.frame[ai];
            double f = 1.0 + (cur.x() - d.pressPx.x()) / 100.0;
            if (auto tip = mView.project(d.startPivot + along * handleLength()))
            {
                QPointF sd = *tip - *c;
                double len = std::hypot(sd.x(), sd.y());
                if (len > 1e-6)
                    f = 1.0 + QPointF::dotProduct(cur - d.pressPx, sd / len) / len;
            }
            if (isPlane(h))
                s[(ai + 1) % 3] = s[(ai + 2) % 3] = f;
            else
                s[ai] = f;
        }
        for (int i = 0; i < 3; ++i)
        {
            if (gridSnap)
                s[i] = std::round(s[i] * 10.0) / 10.0;
            else if (inc.enabled)
                s[i] = gizmo::snapScale(s[i], inc.step);
            // Components stop at 0 rather than flipping through, so scaling down an axis flattens them exactly.
            if (d.components)
                s[i] = std::max(s[i], 0.0);
            else if (std::abs(s[i]) < 1e-3)
                s[i] = 1e-3;
        }
        d.scale = s;
    }
    apply();
}

std::optional<glm::dvec3> Manipulator::snapTarget(QPointF px, unsigned snaps) const
{
    LILA_ZONE();

    if (!(snaps & (SnapVertex | SnapEdge)))
        return std::nullopt;
    Graph& g = Graph::current();
    const Drag& d = *mDrag;
    std::unordered_set<NodeId> moving;
    if (!d.components)
        for (const Saved& s : d.saved)
        {
            moving.insert(s.id);
            for (NodeId c : g.descendants(s.id))
                moving.insert(c);
        }
    double best = 16.0; // px
    std::optional<glm::dvec3> out;
    for (NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh") || moving.contains(id) || !scene::effectivelyVisible(g, id) || !isolate::shows(scene::isolation::active(), id))
            continue;
        scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!mesh)
            continue;
        const auto vis = isolate::visibility(scene::isolation::active(), id, mesh);
        const geo::Mesh& m = *mesh;
        glm::dmat4 world = n->as<glm::dmat4>(Transform::kWorldMatrix);
        // Components being dragged can't be their own snap targets.
        std::vector<char> skip;
        if (d.components)
        {
            skip.assign(m.pointCapacity(), 0);
            for (uint32_t p : scene::components::pointsOf(id))
                if (p < skip.size())
                    skip[p] = 1;
        }
        auto worldOf = [&](geo::Index p) { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); };
        if (snaps & SnapVertex)
            for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            {
                if (!m.pointValid(p) || !vis->point(p) || (!skip.empty() && skip[p]))
                    continue;
                glm::dvec3 w = worldOf(p);
                if (auto s = mView.project(w))
                    if (double dd = dist(*s, px); dd < best)
                    {
                        best = dd;
                        out = w;
                    }
            }
        if (snaps & SnapEdge)
            for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
            {
                if (!m.edgeValid(e) || !vis->edge(e))
                    continue;
                geo::Index p0 = m.edgePoint(e, 0), p1 = m.edgePoint(e, 1);
                if (!skip.empty() && (skip[p0] || skip[p1]))
                    continue;
                glm::dvec3 a = worldOf(p0), b = worldOf(p1);
                auto sa = mView.project(a), sb = mView.project(b);
                if (!sa || !sb)
                    continue;
                QPointF dir = *sb - *sa;
                double len2 = QPointF::dotProduct(dir, dir);
                double t = len2 > 0 ? std::clamp(QPointF::dotProduct(px - *sa, dir) / len2, 0.0, 1.0) : 0.0;
                if (double dd = dist(*sa + dir * t, px); dd < best)
                {
                    best = dd;
                    // Screen-space t is close enough along short edges; perspective skew is tiny at snap distances.
                    out = glm::mix(a, b, t);
                }
            }
    }
    return out;
}

std::optional<glm::dvec3> Manipulator::surfaceSlide(QPointF px)
{
    LILA_ZONE();

    Drag& d = *mDrag;
    auto hit = surfaceSnap::underCursor(px, mView, d.surfaceExclude);
    if (!hit)
        return std::nullopt;
    if (!d.components && !d.pivotEdit && surfaceSnap::alignToNormal())
    {
        glm::dvec3 up = PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z" ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0);
        for (const glm::dmat3& start : d.startFrames)
            d.alignRotations.push_back(glm::quat_cast(surfaceSnap::alignUp(start, up, hit->normal) * glm::transpose(start)));
    }
    return hit->position;
}

void Manipulator::restore()
{
    Graph& g = Graph::current();
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    for (const Saved& s : mDrag->saved)
        if (g.node(s.id))
        {
            if (mDrag->components)
                g.setValue(s.plug, s.values[0]);
            else
                for (int k = 0; k < kSavedCount; ++k)
                    g.setValue({s.id, kSavedAttrs[k]}, s.values[k]);
        }
    u.setEnabled(was);
}

glm::dmat4 Manipulator::componentXform() const
{
    const Drag& d = *mDrag;
    glm::dmat4 toPivot = glm::translate(glm::dmat4(1.0), d.startPivot), fromPivot = glm::translate(glm::dmat4(1.0), -d.startPivot);
    switch (d.tool)
    {
    case Tool::Rotate: return toPivot * glm::dmat4(glm::mat3_cast(d.rotation)) * fromPivot;
    case Tool::Scale:
    {
        glm::dmat3 s = d.frame * glm::dmat3(glm::scale(glm::dmat4(1.0), d.scale)) * glm::transpose(d.frame);
        return toPivot * glm::dmat4(s) * fromPivot;
    }
    default: return glm::translate(glm::dmat4(1.0), d.moveDelta);
    }
}

void Manipulator::apply()
{
    LILA_ZONE();

    Drag& d = *mDrag;
    maskLocked();
    restore();
    Graph& g = Graph::current();
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    std::vector<NodeId> ids;
    for (const Saved& s : d.saved)
        ids.push_back(s.id);
    Tool t = pivotTool(d.pivotEdit, d.tool);
    try
    {
        if (d.componentPivot && gComponentPivot)
        {
            if (t == Tool::Rotate)
                gComponentPivot->frame = glm::mat3_cast(d.rotation) * d.frame;
            else
                gComponentPivot->position = d.startPivot + d.moveDelta;
        }
        else if (d.alongNormal)
            scene::offsetSelectedComponents("Drag Components", glm::transpose(d.frame) * d.moveDelta);
        else if (d.components && d.conform && t == Tool::Move)
            scene::transformSelectedComponentsOnto("Drag Components", componentXform(), d.conform);
        else if (d.components)
            scene::transformSelectedComponents("Drag Components", componentXform());
        else if (d.pivotEdit && t == Tool::Rotate)
        {
            glm::dmat3 r = glm::mat3_cast(d.rotation);
            for (size_t i = 0; i < ids.size(); ++i)
                scene::xformops::setWorldPivotFrame(g, ids[i], r * d.startPivotFrames[i]);
        }
        else if (d.pivotEdit)
        {
            for (size_t i = 0; i < ids.size(); ++i)
                scene::xformops::setWorldPivot(g, ids[i], d.startPivots[i] + d.moveDelta);
        }
        else if (t == Tool::Move)
        {
            scene::xformops::moveBy(g, ids, d.moveDelta);
            for (size_t i = 0; i < ids.size() && i < d.alignRotations.size(); ++i)
                scene::xformops::rotateBy(g, {ids[i]}, d.alignRotations[i]);
            for (NodeId id : ids)
                if (scene::rig::centerEnabled(*g.node(id)))
                    scene::rig::centerNode(g, id);
        }
        else if (t == Tool::Rotate)
            scene::xformops::rotateBy(g, ids, d.rotation);
        else
            scene::xformops::scaleBy(g, ids, d.scale);
    }
    catch (const std::exception& e)
    {
        log::warn("manipulator: {}", e.what());
    }
    u.setEnabled(was);
    mPivot = d.startPivot + (t == Tool::Move ? d.moveDelta : glm::dvec3(0.0));
    // Locked channels can hold the lead back from the delta, and the manipulator stays on it.
    if (t == Tool::Move && !d.components && !d.pivotEdit && g.node(mLead))
        mPivot = scene::xformops::worldPivot(g, mLead);
}

void Manipulator::drag(QPointF px, Qt::KeyboardModifiers mods)
{
    if (mExtrude.dragging())
        mExtrude.drag(px, mView);
    else if (mDrag)
        update(px, mods);
}

void Manipulator::warped(QPointF offset)
{
    if (mDrag)
        mDrag->lastPx += offset;
}

void Manipulator::commit()
{
    LILA_ZONE();

    Drag d = *mDrag;
    restore();
    mDrag.reset();

    json names = json::array();
    for (const Saved& s : d.saved)
        names.push_back(nameOf(s.id));
    Tool t = pivotTool(d.pivotEdit, d.tool);
    if (d.componentPivot)
        return;
    try
    {
        if (d.components)
        {
            json pivot = vecJson(d.startPivot);
            if (d.alongNormal)
            {
                const glm::dvec3 uvn = glm::transpose(d.frame) * d.moveDelta;
                if (std::abs(uvn.x) <= 1e-9 && std::abs(uvn.y) <= 1e-9 && std::abs(uvn.z) > 1e-9)
                    ops::execute("moveComponents", {{"normal", tidy(uvn.z)}}, CallSource::UI);
                else if (glm::length(uvn) > 1e-9)
                    ops::execute("moveComponents", {{"uvn", vecJson(uvn)}}, CallSource::UI);
            }
            else if (t == Tool::Move && glm::length(d.moveDelta) > 1e-9)
            {
                json args = {{"by", vecJson(d.moveDelta)}};
                if (d.conform)
                {
                    args["onto"] = json::array();
                    for (NodeId id : d.conformTo)
                        args["onto"].push_back(nameOf(id));
                }
                ops::execute("moveComponents", args, CallSource::UI);
                if (gComponentPivot)
                {
                    gComponentPivot->position += d.moveDelta;
                    gComponentPivot->selection = scene::components::selected(gComponentPivot->mode);
                }
            }
            else if (t == Tool::Rotate && std::abs(d.angle) > 1e-9)
            {
                glm::dvec3 euler = scene::matrixToEuler(glm::mat3_cast(d.rotation), scene::RotateOrder::XYZ);
                ops::execute("rotateComponents", {{"by", vecJson(euler)}, {"pivot", pivot}}, CallSource::UI);
            }
            else if (t == Tool::Scale && glm::length(d.scale - glm::dvec3(1.0)) > 1e-9)
            {
                json args = {{"by", vecJson(d.scale)}, {"pivot", pivot}};
                if (glm::length(glm::dvec3(d.frame[0]) - glm::dvec3(1, 0, 0)) + glm::length(glm::dvec3(d.frame[1]) - glm::dvec3(0, 1, 0)) > 1e-9)
                    args["orient"] = vecJson(scene::matrixToEuler(d.frame, scene::RotateOrder::XYZ));
                ops::execute("scaleComponents", args, CallSource::UI);
            }
            return;
        }
        if (d.pivotEdit && t == Tool::Rotate)
        {
            if (std::abs(d.angle) < 1e-9)
                return;
            Graph& g = Graph::current();
            glm::dmat3 r = glm::mat3_cast(d.rotation);
            UndoChunk chunk("Orient Pivot");
            for (size_t i = 0; i < d.saved.size(); ++i)
            {
                NodeId id = d.saved[i].id;
                glm::dmat3 local = glm::transpose(scene::xformops::frame(g, id, scene::Space::Object)) * r * d.startPivotFrames[i];
                glm::dvec3 hint = g.node(id)->as<glm::dvec3>(Transform::kPivotOrient);
                glm::dvec3 euler = scene::xformops::closestEuler(local, scene::RotateOrder::XYZ, hint);
                ops::execute("setPivot", {{"args", {names[i]}}, {"orient", vecJson(euler)}}, CallSource::UI);
            }
        }
        else if (d.pivotEdit)
        {
            if (glm::length(d.moveDelta) < 1e-9)
                return;
            UndoChunk chunk("Move Pivot");
            for (size_t i = 0; i < d.saved.size(); ++i)
                ops::execute("setPivot", {{"args", {names[i]}}, {"position", vecJson(d.startPivots[i] + d.moveDelta)}}, CallSource::UI);
        }
        else if (t == Tool::Move)
        {
            if (glm::length(d.moveDelta) < 1e-9)
                return;
            json centred = json::array();
            for (const Saved& s : d.saved)
                if (scene::Node* n = Graph::current().node(s.id); n && scene::rig::centerEnabled(*n))
                    centred.push_back(n->path());
            std::optional<UndoChunk> chunk;
            if (!centred.empty() || !d.alignRotations.empty())
                chunk.emplace("Move");
            ops::execute("move", {{"args", names}, {"by", vecJson(d.moveDelta)}}, CallSource::UI);
            for (size_t i = 0; i < d.alignRotations.size() && i < names.size(); ++i)
                if (glm::angle(d.alignRotations[i]) > 1e-9)
                {
                    glm::dvec3 euler = scene::matrixToEuler(glm::mat3_cast(d.alignRotations[i]), scene::RotateOrder::XYZ);
                    ops::execute("rotate", {{"args", {names[i]}}, {"by", vecJson(euler)}}, CallSource::UI);
                }
            if (!centred.empty())
                ops::execute("centerJoint", {{"args", centred}}, CallSource::UI);
        }
        else if (t == Tool::Rotate)
        {
            if (std::abs(d.angle) < 1e-9)
                return;
            glm::dvec3 euler = scene::matrixToEuler(glm::mat3_cast(d.rotation), scene::RotateOrder::XYZ);
            ops::execute("rotate", {{"args", names}, {"by", vecJson(euler)}}, CallSource::UI);
        }
        else
        {
            if (glm::length(d.scale - glm::dvec3(1.0)) < 1e-9)
                return;
            ops::execute("scale", {{"args", names}, {"by", vecJson(d.scale)}}, CallSource::UI);
        }
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void Manipulator::release()
{
    if (mExtrude.dragging())
        mExtrude.end();
    else if (mDrag)
        commit();
}

void Manipulator::cancel()
{
    mExtrude.cancel();
    if (!mDrag)
        return;
    restore();
    mDrag.reset();
}

bool Manipulator::keyPress(QKeyEvent* e)
{
    if (mExtrude.dragging() && e->key() == Qt::Key_Escape)
    {
        mExtrude.cancel();
        return true;
    }
    if (!mDrag)
        return false;
    Drag& d = *mDrag;
    int k = e->key();
    if (k == Qt::Key_Escape)
    {
        cancel();
        return true;
    }
    QString text = e->text();
    if (!text.isEmpty() && (text[0].isDigit() || text == "." || text == "-"))
    {
        d.typed += text;
        return true;
    }
    if (k == Qt::Key_Backspace)
    {
        d.typed.chop(1);
        return true;
    }
    if ((k == Qt::Key_Return || k == Qt::Key_Enter) && !d.typed.isEmpty())
    {
        bool ok = false;
        double v = d.typed.toDouble(&ok);
        if (!ok)
            return true;
        Tool t = pivotTool(d.pivotEdit, d.tool);
        int ai = axisOf(d.handle);
        if (t == Tool::Move)
        {
            glm::dvec3 dir = ai >= 0 && !isPlane(d.handle) ? d.frame[ai] : glm::length(d.moveDelta) > 1e-9 ? glm::normalize(d.moveDelta) : d.frame[0];
            d.moveDelta = dir * v;
        }
        else if (t == Tool::Rotate)
        {
            glm::dvec3 a = glm::length(d.rotateAxis) > 0.5 ? d.rotateAxis : glm::dvec3(0, 1, 0);
            d.angle = v;
            d.rotation = glm::angleAxis(glm::radians(v), glm::normalize(a));
        }
        else
        {
            if (std::abs(v) < 1e-6)
                return true;
            d.scale = glm::dvec3(1.0);
            if (ai >= 0 && !isPlane(d.handle))
                d.scale[ai] = v;
            else
                d.scale = glm::dvec3(v);
        }
        apply();
        commit();
        return true;
    }
    return false;
}

// ------------------------------------------------------------------- drawing

glm::vec4 Manipulator::handleColor(H h, int axisIndex) const
{
    if (handleLocked(h))
        return kLocked;
    // The remembered handle stays highlighted, so it shows what a middle drag will do.
    bool hot = (mDrag && mDrag->handle == h) || (!mDrag && (mHover == h || (handlesCurrent() && mLastHandle == h)));
    if (axisIndex >= 0 && viewportDisplay::accentColors(viewportDisplay::kManipulatorAxisColors))
    {
        glm::vec4 c(viewportDisplay::axisColor(axisIndex), 1.0f);
        return hot ? glm::mix(c, themeSrgb("manipActive", kHot), 0.55f) : c;
    }
    return hot ? themeSrgb("manipActive", kHot) : themeSrgb("manipNeutral", kGray);
}

void Manipulator::draw(OverlayPainter& p)
{
    LILA_ZONE();

    bool gizmo = mDrag || (!mExtrude.dragging() && visible());
    bool extrude = !mDrag && mExtrude.active();
    if (!gizmo && !extrude)
        return;
    p.setShadow(true);
    if (gizmo)
        switch (effectiveTool())
        {
        case Tool::Rotate: drawRotate(p); break;
        case Tool::Scale: drawScale(p); break;
        default: drawMove(p); break;
        }
    if (extrude)
        mExtrude.draw(p, mView, mExtrudeHover != ExtrudeHandles::Handle::None ? mExtrudeHover
                                : handlesCurrent()                             ? mLastExtrude
                                                                               : ExtrudeHandles::Handle::None);
    p.setShadow(false);
    drawReadout(p);
}

void Manipulator::drawMove(OverlayPainter& p)
{
    auto c = mView.project(mPivot);
    if (!c)
        return;
    double L = handleLength();
    glm::dvec3 viewDir = mView.viewDir(mPivot);
    bool pivotMode = mDrag ? mDrag->pivotEdit : tools::pivotEdit();
    H active = mDrag ? mDrag->handle : H::None;

    for (int i = 0; i < 3; ++i)
    {
        H h = H(int(H::PlaneYZ) + i);
        if (std::abs(glm::dot(axis(i), viewDir)) < 0.12 || (active != H::None && active != h) || pivotMode || mNormal)
            continue;
        glm::dvec3 a = axis((i + 1) % 3), b = axis((i + 2) % 3);
        std::vector<QPointF> quad;
        for (auto [s, u] : {std::pair{0.25, 0.25}, {0.45, 0.25}, {0.45, 0.45}, {0.25, 0.45}})
            if (auto q = mView.project(mPivot + (a * s + b * u) * L))
                quad.push_back(*q);
        if (quad.size() != 4)
            continue;
        glm::vec4 col = handleColor(h, -1);
        p.fill(quad, {col.r, col.g, col.b, mHover == h || active == h ? 0.45f : 0.16f});
        p.polyline(quad, 1.0f, {col.r, col.g, col.b, 0.6f}, true);
    }

    for (int i = 0; i < 3; ++i)
    {
        H h = H(int(H::X) + i);
        if (active != H::None && active != h && active != H::Center && !isPlane(active))
            continue;
        if (isPlane(active) && axisOf(active) == i)
            continue;
        auto tip = mView.project(mPivot + axis(i) * L);
        if (!tip || dist(*tip, *c) < 12)
            continue; // looking straight down this axis
        glm::vec4 col = handleColor(h, i);
        bool hot = mHover == h || active == h;
        QPointF dirPx = (*tip - *c) / dist(*tip, *c);
        QPointF start = *c + dirPx * 10.0;
        if (pivotMode)
            p.dashedLine(start, *tip, hot ? 3.0f : 2.0f, 5.0f, col);
        else
            p.line(start, *tip - dirPx * 6.0, hot ? 3.5f : 2.0f, col);
        QPointF n(-dirPx.y(), dirPx.x());
        QPointF arrowTip = *tip + dirPx * 10.0;
        p.triangle(arrowTip, *tip - dirPx * 4.0 + n * 5.0, *tip - dirPx * 4.0 - n * 5.0, col);
        p.text(arrowTip + dirPx * 12.0 + QPointF(0, 4), QString(mNormal ? QChar(u"UVN"[i]) : QChar('X' + i)), col, OverlayPainter::Center);
    }

    glm::vec4 centerCol = handleColor(H::Center, -1);
    if (pivotMode)
    {
        p.polyline({*c + QPointF(-7, 0), *c + QPointF(7, 0)}, 1.5f, centerCol);
        p.polyline({*c + QPointF(0, -7), *c + QPointF(0, 7)}, 1.5f, centerCol);
        std::vector<QPointF> ring;
        for (int k = 0; k < 24; ++k)
            ring.push_back(*c + QPointF(std::cos(k * kPi / 12), std::sin(k * kPi / 12)) * 9.0);
        p.polyline(ring, 1.5f, centerCol, true);
    }
    else
    {
        QRectF box(c->x() - 6, c->y() - 6, 12, 12);
        p.polyline({box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()}, mHover == H::Center ? 2.5f : 1.5f, centerCol, true);
    }
}

void Manipulator::drawRotate(OverlayPainter& p)
{
    auto c = mView.project(mPivot);
    if (!c)
        return;
    H active = mDrag ? mDrag->handle : H::None;
    double ringPx = 0.85 * pref("/manipulator/size", 100.0);
    bool pivotMode = mDrag ? mDrag->pivotEdit : tools::pivotEdit();
    auto stroke = [&](const std::vector<QPointF>& pts, float w, const glm::vec4& col, bool closed) {
        if (pivotMode)
            p.dashedPolyline(pts, w, 6.0f, col, closed);
        else
            p.polyline(pts, w, col, closed);
    };

    std::vector<QPointF> circle;
    for (int k = 0; k < 64; ++k)
        circle.push_back(*c + QPointF(std::cos(k * kPi / 32), std::sin(k * kPi / 32)) * ringPx);
    if (active == H::None || active == H::Free)
        stroke(circle, mHover == H::Free || active == H::Free ? 1.5f : 1.0f, {0.6f, 0.6f, 0.6f, 0.35f}, true);

    for (int i = 0; i < 3; ++i)
    {
        H h = H(int(H::X) + i);
        if (active != H::None && active != h)
            continue;
        auto pts = ringPoints(i, active != h);
        glm::vec4 col = handleColor(h, i);
        float w = (mHover == h || active == h) ? 3.5f : 2.0f;
        std::vector<QPointF> run;
        auto flush = [&] {
            if (run.size() > 1)
                stroke(run, w, col, false);
            run.clear();
        };
        for (QPointF q : pts)
        {
            if (std::isnan(q.x()))
                flush();
            else
                run.push_back(q);
        }
        flush();
    }

    if (active == H::None || active == H::ViewRing)
    {
        std::vector<QPointF> outer;
        for (int k = 0; k < 64; ++k)
            outer.push_back(*c + QPointF(std::cos(k * kPi / 32), std::sin(k * kPi / 32)) * ringPx * 1.18);
        stroke(outer, mHover == H::ViewRing || active == H::ViewRing ? 3.0f : 1.5f, handleColor(H::ViewRing, -1), true);
    }
}

void Manipulator::drawScale(OverlayPainter& p)
{
    auto c = mView.project(mPivot);
    if (!c)
        return;
    double L = handleLength();
    H active = mDrag ? mDrag->handle : H::None;
    glm::dvec3 s = mDrag ? mDrag->scale : glm::dvec3(1.0);
    for (int i = 0; i < 3; ++i)
    {
        H h = H(int(H::X) + i);
        if (active != H::None && active != h && active != H::Center && !(isPlane(active) && axisOf(active) != i))
            continue;
        auto tip = mView.project(mPivot + axis(i) * L * std::clamp(s[i], 0.05, 20.0));
        if (!tip || dist(*tip, *c) < 12)
            continue;
        glm::vec4 col = handleColor(h, i);
        bool hot = mHover == h || active == h;
        QPointF dirPx = (*tip - *c) / dist(*tip, *c);
        p.line(*c + dirPx * 10.0, *tip, hot ? 3.5f : 2.0f, col);
        p.rect(QRectF(tip->x() - 5, tip->y() - 5, 10, 10), col);
        p.text(*tip + dirPx * 16.0 + QPointF(0, 4), QString(QChar('X' + i)), col, OverlayPainter::Center);
    }
    glm::vec4 centerCol = handleColor(H::Center, -1);
    p.rect(QRectF(c->x() - 6, c->y() - 6, 12, 12), {centerCol.r, centerCol.g, centerCol.b, 0.9f});
}

QString Manipulator::readout() const
{
    if (!mDrag)
        return {};
    const Drag& d = *mDrag;
    Tool t = pivotTool(d.pivotEdit, d.tool);
    QString text;
    if (t == Tool::Move)
    {
        int ai = axisOf(d.handle);
        if (d.handle >= H::X && d.handle <= H::Z)
        {
            double signedLen = glm::dot(d.moveDelta, d.frame[ai]);
            text = QString("%1 %2 cm").arg(d.alongNormal ? QChar(u"UVN"[ai]) : QChar('X' + ai)).arg(signedLen, 0, 'f', 2);
        }
        else
        {
            glm::dvec3 local = glm::transpose(d.frame) * d.moveDelta;
            text = QString("%1 cm  (%2, %3, %4)").arg(glm::length(d.moveDelta), 0, 'f', 2).arg(local.x, 0, 'f', 2).arg(local.y, 0, 'f', 2).arg(local.z, 0, 'f', 2);
        }
    }
    else if (t == Tool::Rotate)
        text = QString::fromUtf8("%1°").arg(d.angle, 0, 'f', 1);
    else
    {
        glm::dvec3 s = d.scale;
        if (s.x == s.y && s.y == s.z)
            text = QString::fromUtf8("×%1").arg(s.x, 0, 'f', 3);
        else
            text = QString::fromUtf8("× %1, %2, %3").arg(s.x, 0, 'f', 3).arg(s.y, 0, 'f', 3).arg(s.z, 0, 'f', 3);
    }
    if (heldSnap() || tools::incrementSnap(t).enabled)
        text += "  snap";
    return text;
}

void Manipulator::drawReadout(OverlayPainter& p)
{
    if (!mDrag)
        return;
    const Drag& d = *mDrag;
    auto start = mView.project(d.startPivot);
    if (!start)
        return;
    Tool t = pivotTool(d.pivotEdit, d.tool);
    QPointF at = d.lastPx + QPointF(18, -14);

    if (t == Tool::Move)
    {
        auto now = mView.project(mPivot);
        if (now)
            p.dashedLine(*start, *now, 1.5f, 4.0f, kTrail);
        p.disc(*start, 3.0f, kTrail, 10);
    }
    else if (t == Tool::Rotate)
    {
        // Filled wedge in the rotation plane from the grab direction to the current one.
        if (d.handle != H::Free)
        {
            glm::dvec3 a = glm::normalize(d.rotateAxis);
            glm::dvec3 ref = d.handle == H::ViewRing || std::abs(glm::dot(a, glm::normalize(d.startHit - d.startPivot))) > 0.9
                                 ? glm::normalize(glm::cross(a, std::abs(a.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)))
                                 : glm::normalize(d.startHit - d.startPivot);
            if (d.handle == H::ViewRing)
            {
                glm::dvec3 o, dir;
                mView.ray(d.pressPx, o, dir);
                glm::dvec3 hit;
                if (rayPlane(o, dir, d.startPivot, a, hit) && glm::length(hit - d.startPivot) > 1e-9)
                    ref = glm::normalize(hit - d.startPivot);
            }
            double r = handleLength() * 0.85 * (d.handle == H::ViewRing ? 1.18 : 1.0);
            int steps = std::max(2, int(std::abs(d.angle) / 4.0));
            std::vector<QPointF> fan;
            fan.push_back(*start);
            for (int k = 0; k <= steps; ++k)
            {
                double ang = glm::radians(d.angle) * k / steps;
                glm::dvec3 v = glm::angleAxis(ang, a) * ref;
                if (auto q = mView.project(d.startPivot + v * r))
                    fan.push_back(*q);
            }
            for (size_t k = 1; k + 1 < fan.size(); ++k)
                p.triangle(fan[0], fan[k], fan[k + 1], {1.0f, 1.0f, 1.0f, 0.16f});
            if (fan.size() > 2)
            {
                p.line(fan[0], fan[1], 1.0f, kTrail);
                p.line(fan[0], fan.back(), 1.0f, kTrail);
            }
        }
    }
    p.label(at, readout());
    if (!d.typed.isEmpty())
        p.label(at + QPointF(0, 22), QString::fromUtf8("= %1…").arg(d.typed));
}
} // namespace lila::ui
