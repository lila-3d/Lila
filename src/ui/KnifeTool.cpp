#include "ui/KnifeTool.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Actions.h"

#include <QApplication>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <set>

namespace lila::ui
{
namespace
{
using geo::Index;
using geo::kInvalid;
using geo::KnifePoint;
using Kind = KnifePoint::Kind;

constexpr double kVertexRadius = 12.0; // px
constexpr double kEdgeRadius = 10.0;   // px

// Overlay colours are sRGB.
const glm::vec4 kCut(0.95f, 0.95f, 0.95f, 1.0f);
const glm::vec4 kMirror(0.6f, 0.6f, 0.6f, 0.85f);
const glm::vec4 kFailed(0.55f, 0.55f, 0.55f, 0.8f);
const glm::vec4 kRim(0.08f, 0.08f, 0.08f, 1.0f);
const glm::vec4 kNewVertex(0.85f, 0.85f, 0.85f, 1.0f);
const glm::vec4 kHot(1.0f, 1.0f, 1.0f, 1.0f);

double distPx(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

double segmentDistPx(QPointF p, QPointF a, QPointF b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    const double u = len2 > 0.0 ? std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2, 0.0, 1.0) : 0.0;
    return distPx(p, a + ab * u);
}

/** Parameter along the segment p0-p1 of its closest point to the ray. */
double raySegmentParam(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& p0, const glm::dvec3& p1)
{
    const glm::dvec3 e = p1 - p0, w = p0 - o;
    const double a = glm::dot(e, e), b = glm::dot(e, d), c = glm::dot(d, d), dd = glm::dot(e, w), ee = glm::dot(d, w);
    const double denom = a * c - b * b;
    if (std::abs(denom) < 1e-18 * a * c)
        return 0.5;
    return std::clamp((b * ee - c * dd) / denom, 0.0, 1.0);
}

double snapStepFraction()
{
    const double pct = PrefStore::get().value<double>("general", "/knife/snapStep", 10.0);
    return std::clamp(pct, 0.1, 50.0) / 100.0;
}

/** Keys reach the hotkeys before the viewport, so the tool's own keys are taken here first. */
class KeyFilter : public QObject
{
public:
    explicit KeyFilter(KnifeTool* tool) : mTool(tool) {}

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease && e->type() != QEvent::ShortcutOverride)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        if ((k->key() == Qt::Key_Shift || k->key() == Qt::Key_Control) && e->type() != QEvent::ShortcutOverride)
            viewportTools::requestRedraw();
        QWidget* focus = QApplication::focusWidget();
        if (!focus || obj != focus || !focus->inherits("lila::ui::ViewportWidget"))
            return false;
        if (!owns(k->key(), k->modifiers()))
            return false;
        if (e->type() == QEvent::ShortcutOverride)
        {
            e->accept();
            return true;
        }
        if (e->type() == QEvent::KeyPress && !k->isAutoRepeat())
            mTool->key(k->key(), k->modifiers());
        return true;
    }

    static bool owns(int key, Qt::KeyboardModifiers mods)
    {
        mods &= ~Qt::KeypadModifier;
        switch (key)
        {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Backspace:
        case Qt::Key_Escape:
        case Qt::Key_A:
        case Qt::Key_X: return mods == Qt::NoModifier;
        default: return false;
        }
    }

private:
    KnifeTool* mTool;
};

json pointJson(const KnifePoint& k, const glm::dvec3& world)
{
    switch (k.kind)
    {
    case Kind::Point: return {{"vertex", k.id}};
    case Kind::Edge: return {{"edge", k.id}, {"t", k.t}};
    case Kind::Face: return {{"face", k.id}, {"position", {world.x, world.y, world.z}}};
    }
    return nullptr;
}

bool samePoint(const KnifePoint& a, const KnifePoint& b)
{
    return a.kind == b.kind && a.id == b.id && (a.kind != Kind::Edge || std::abs(a.t - b.t) < 1e-6) &&
           (a.kind != Kind::Face || glm::distance(a.position, b.position) < 1e-9);
}

/** The direction the view looks along: the ray through its centre. */
glm::dvec3 viewForward(const ViewInfo& view)
{
    glm::dvec3 o, d;
    view.ray(QPointF(view.width * 0.5, view.height * 0.5), o, d);
    return glm::normalize(d);
}
} // namespace

KnifeTool::KnifeTool() = default;

KnifeTool::~KnifeTool() = default;

void KnifeTool::activate()
{
    mFilter = std::make_unique<KeyFilter>(this);
    qApp->installEventFilter(mFilter.get());
    refresh();
    log::info("Knife: click points to cut, Enter or RMB finishes, Ctrl+click an edge inserts a loop, Shift snaps, A angle, X cut through, "
              "Backspace removes a point, Esc cancels or exits");
}

void KnifeTool::deactivate()
{
    if (mFilter)
        qApp->removeEventFilter(mFilter.get());
    mFilter.reset();
}

std::optional<QCursor> KnifeTool::cursor() const { return QCursor(Qt::CrossCursor); }

void KnifeTool::refresh()
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::set<scene::NodeId> ids;
    if (mNode != scene::kNoNode)
        ids.insert(mNode);
    else
    {
        for (scene::NodeId id : g.selection())
            ids.insert(id);
        if (scene::components::mode() != scene::ComponentMode::Object)
            for (auto& [node, sel] : scene::components::selected(scene::components::mode()))
                ids.insert(node);
        std::erase_if(ids, [&](scene::NodeId id) {
            scene::Node* n = g.node(id);
            return !n || !n->type().isA("mesh");
        });
        // Nothing selected: any visible mesh can be cut, as in Maya.
        if (ids.empty())
            for (scene::NodeId id : g.nodes())
                if (scene::Node* n = g.node(id); n && n->type().isA("mesh") && scene::effectivelyVisible(g, id))
                    ids.insert(id);
    }
    std::erase_if(mTargets, [&](const auto& kv) { return !ids.contains(kv.first); });
    for (scene::NodeId id : ids)
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        scene::MeshPtr cage = scene::modifiers::cage(g, id);
        const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        Target& t = mTargets[id];
        if (cage == t.mesh && world == t.world)
            continue;
        // The pending points name ids of the mesh they were clicked on (an undo may have changed it since).
        if (id == mNode && cage != t.mesh)
            cancel();
        t.mesh = cage;
        t.world = world;
        t.symmetry.reset();
        if (!cage)
        {
            t.bvh = geo::MeshBvh();
            continue;
        }
        // Not compacted: hits must carry the cage's own face ids.
        geo::Mesh w = *cage;
        for (glm::dvec3& p : w.positions())
            p = glm::dvec3(world * glm::dvec4(p, 1.0));
        t.bvh.build(w);
        const int64_t axis = n->as<int64_t>(scene::MeshNode::kSymmetry);
        if (axis >= 1 && axis <= 3)
        {
            geo::Mesh copy = *cage;
            const double tol = std::max(1e-4, glm::length(copy.bounds().size()) * 1e-5);
            t.symmetry = geo::SymmetryMap::persistent(copy, int(axis - 1), tol);
        }
    }
    if (mNode != scene::kNoNode && (!mTargets.contains(mNode) || !mTargets[mNode].mesh))
        cancel();
}

KnifeTool::Target* KnifeTool::target(scene::NodeId id)
{
    auto it = mTargets.find(id);
    return it != mTargets.end() && it->second.mesh ? &it->second : nullptr;
}

bool KnifeTool::visible(const Target& t, const glm::dvec3& world, const ViewInfo& view) const
{
    if (mThrough || t.bvh.empty())
        return true;
    const glm::dvec3 d = world - view.eye;
    const double len = glm::length(d);
    // A little slack so points lying on the surface aren't hidden by it.
    const double slack = view.unitsPerPixel(world) * 3.0;
    return len <= slack || !t.bvh.occluded(view.eye, d / len, len - slack);
}

QPointF KnifeTool::constrained(QPointF px, const ViewInfo& view) const
{
    if (!mAngle || mPoints.empty())
        return px;
    std::optional<QPointF> last = view.project(mPoints.back().world);
    if (!last)
        return px;
    const QPointF v = px - *last;
    const double step = 3.141592653589793 / 4.0;
    const double angle = std::round(std::atan2(v.y(), v.x()) / step) * step;
    const QPointF dir(std::cos(angle), std::sin(angle));
    return *last + dir * (v.x() * dir.x() + v.y() * dir.y());
}

std::optional<std::pair<scene::NodeId, KnifeTool::Spot>> KnifeTool::pick(QPointF px, const ViewInfo& view, bool snapStep)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    scene::NodeId hitNode = scene::kNoNode;
    geo::BvhHit hit;
    for (auto& [id, t] : mTargets)
    {
        if (!t.mesh || t.bvh.empty() || (mNode != scene::kNoNode && id != mNode))
            continue;
        geo::BvhHit h;
        if (t.bvh.raycast(o, d, h) && h.t < hit.t)
        {
            hit = h;
            hitNode = id;
        }
    }
    if (hitNode == scene::kNoNode)
        return std::nullopt;
    const Target& t = mTargets[hitNode];
    const geo::Mesh& m = *t.mesh;
    if (!m.faceValid(hit.face))
        return std::nullopt;
    auto world = [&](Index p) { return glm::dvec3(t.world * glm::dvec4(m.position(p), 1.0)); };

    // Vertices and edges of the faces around the hit face.
    std::set<Index> points, edges;
    for (Index p : m.facePoints(hit.face))
        for (Index f : m.pointFaces(p))
            m.forEachFaceCorner(f, [&](Index c) {
                points.insert(m.cornerPoint(c));
                edges.insert(m.cornerEdge(c));
            });

    Index bestPoint = kInvalid;
    double best = kVertexRadius;
    for (Index p : points)
        if (std::optional<QPointF> s = view.project(world(p)); s && distPx(*s, px) < best && visible(t, world(p), view))
        {
            best = distPx(*s, px);
            bestPoint = p;
        }
    if (bestPoint != kInvalid)
        return std::pair{hitNode, Spot{KnifePoint::point(bestPoint), world(bestPoint)}};

    Index bestEdge = kInvalid;
    double bestT = 0.5;
    best = kEdgeRadius;
    for (Index e : edges)
    {
        const glm::dvec3 a = world(m.edgePoint(e, 0)), b = world(m.edgePoint(e, 1));
        std::optional<QPointF> sa = view.project(a), sb = view.project(b);
        if (!sa || !sb)
            continue;
        const double dist = segmentDistPx(px, *sa, *sb);
        if (dist >= best)
            continue;
        const double s = raySegmentParam(o, d, a, b);
        if (!visible(t, glm::mix(a, b, s), view))
            continue;
        best = dist;
        bestEdge = e;
        bestT = s;
    }
    if (bestEdge != kInvalid)
    {
        if (snapStep)
        {
            const double step = snapStepFraction();
            bestT = std::clamp(std::round(bestT / step) * step, 0.0, 1.0);
        }
        KnifePoint k = KnifePoint::edge(bestEdge, bestT);
        if (bestT <= 1e-6)
            k = KnifePoint::point(m.edgePoint(bestEdge, 0));
        else if (bestT >= 1.0 - 1e-6)
            k = KnifePoint::point(m.edgePoint(bestEdge, 1));
        const glm::dvec3 w = glm::mix(world(m.edgePoint(bestEdge, 0)), world(m.edgePoint(bestEdge, 1)), bestT);
        return std::pair{hitNode, Spot{k, w}};
    }
    const glm::dvec3 local = glm::dvec3(glm::inverse(t.world) * glm::dvec4(hit.point, 1.0));
    return std::pair{hitNode, Spot{KnifePoint::face(hit.face, local), hit.point}};
}

geo::KnifeOptions KnifeTool::options(const Target& t, const ViewInfo& view) const
{
    geo::KnifeOptions o;
    const glm::dmat4 toLocal = glm::inverse(t.world);
    if (view.ortho)
        o.viewDir = glm::inverse(glm::dmat3(t.world)) * viewForward(view);
    else
        o.eye = glm::dvec3(toLocal * glm::dvec4(view.eye, 1.0));
    o.through = mThrough;
    o.symmetry = t.symmetry ? &*t.symmetry : nullptr;
    return o;
}

void KnifeTool::cancel()
{
    mPoints.clear();
    mNode = scene::kNoNode;
    mHoverFirst = false;
}

void KnifeTool::finish(bool closed)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(mNode);
    if (mPoints.size() < 2 || !n)
    {
        cancel();
        return;
    }
    json points = json::array();
    for (const Spot& s : mPoints)
        points.push_back(pointJson(s.point, s.world));
    json call = {{"mesh", n->name()}, {"points", points}, {"closed", closed}, {"through", mThrough}};
    if (mView.ortho)
    {
        const glm::dvec3 f = viewForward(mView);
        call["direction"] = {f.x, f.y, f.z};
    }
    else
        call["eye"] = {mView.eye.x, mView.eye.y, mView.eye.z};
    try
    {
        ops::execute("polyKnife", call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    cancel();
    refresh();
}

void KnifeTool::insertLoop(const Spot& spot)
{
    if (spot.point.kind != Kind::Edge)
        return;
    scene::Node* n = scene::Graph::current().node(mNode != scene::kNoNode ? mNode : mHover->first);
    if (!n)
        return;
    try
    {
        ops::execute("polyKnife", {{"mesh", n->name()}, {"loopEdge", spot.point.id}, {"fraction", spot.point.t}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    refresh();
}

bool KnifeTool::mousePress(QMouseEvent* e, const ViewInfo& view)
{
    refresh();
    mView = view;
    mCursor = e->position();
    mCursorView = view.camera;
    const Qt::KeyboardModifiers mods = e->modifiers();
    if (e->button() == Qt::RightButton)
    {
        // Without a cut under way the right button keeps its marking menus.
        if (mPoints.empty())
            return false;
        finish(false);
        return true;
    }
    if (e->button() != Qt::LeftButton)
        return false;
    if (mods & Qt::ControlModifier)
    {
        if (mPoints.empty())
            if (auto h = pick(e->position(), view, mods & Qt::ShiftModifier); h && h->second.point.kind == Kind::Edge)
            {
                mHover = h;
                insertLoop(h->second);
            }
        return true;
    }
    if (mHoverFirst && mPoints.size() >= 3)
    {
        finish(true);
        return true;
    }
    auto h = pick(constrained(e->position(), view), view, mods & Qt::ShiftModifier);
    if (!h || (mNode != scene::kNoNode && h->first != mNode))
        return true;
    if (!mPoints.empty() && samePoint(mPoints.back().point, h->second.point))
        return true;
    mNode = h->first;
    mPoints.push_back(h->second);
    viewportTools::requestRedraw();
    return true;
}

bool KnifeTool::mouseMove(QMouseEvent* e, const ViewInfo& view)
{
    mView = view;
    mCursor = e->position();
    mCursorView = view.camera;
    refresh();
    const Qt::KeyboardModifiers mods = e->modifiers();
    mHoverFirst = false;
    if (mPoints.size() >= 3 && !(mods & Qt::ControlModifier))
        if (std::optional<QPointF> first = view.project(mPoints.front().world); first && distPx(*first, e->position()) < kVertexRadius)
            mHoverFirst = true;
    const QPointF at = mods & Qt::ControlModifier ? e->position() : constrained(e->position(), view);
    mHover = pick(at, view, mods & Qt::ShiftModifier);
    viewportTools::requestRedraw();
    return true;
}

bool KnifeTool::mouseRelease(QMouseEvent*, const ViewInfo&) { return true; }

bool KnifeTool::keyPress(QKeyEvent* e, const ViewInfo& view)
{
    mView = view;
    return key(e->key(), e->modifiers());
}

bool KnifeTool::key(int key, Qt::KeyboardModifiers mods)
{
    if (!KeyFilter::owns(key, mods))
        return false;
    switch (key)
    {
    case Qt::Key_Return:
    case Qt::Key_Enter: finish(false); break;
    case Qt::Key_Backspace:
        if (!mPoints.empty())
            mPoints.pop_back();
        if (mPoints.empty())
            mNode = scene::kNoNode;
        break;
    case Qt::Key_Escape:
        if (mPoints.empty())
        {
            // Leaving deletes the tool, so it has to wait until this event is done with it.
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        cancel();
        break;
    case Qt::Key_A: mAngle = !mAngle; break;
    case Qt::Key_X: mThrough = !mThrough; break;
    default: return false;
    }
    viewportTools::requestRedraw();
    return true;
}

void KnifeTool::draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    refresh();
    const Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
    const bool here = view.camera == mCursorView;
    const bool ctrl = mods & Qt::ControlModifier;
    const scene::NodeId node = mNode != scene::kNoNode ? mNode : mHover ? mHover->first : scene::kNoNode;
    const Target* t = target(node);
    auto toWorld = [&](const glm::dvec3& local) { return glm::dvec3(t->world * glm::dvec4(local, 1.0)); };

    if (t)
    {
        const geo::Mesh& m = *t->mesh;
        const bool hoverUsable = here && mHover && mHover->first == node && !mHoverFirst;
        geo::KnifePlan plan;
        if (ctrl && mPoints.empty() && hoverUsable && mHover->second.point.kind == Kind::Edge)
            plan = geo::knifeLoopPlan(m, mHover->second.point.id, mHover->second.point.t, t->symmetry ? &*t->symmetry : nullptr);
        else if (!ctrl)
        {
            std::vector<KnifePoint> pts;
            for (const Spot& s : mPoints)
                pts.push_back(s.point);
            if (hoverUsable && !mPoints.empty() && !samePoint(mPoints.back().point, mHover->second.point))
                pts.push_back(mHover->second.point);
            geo::KnifeOptions o = options(*t, view);
            o.closed = mHoverFirst;
            plan = geo::knifePlan(m, pts, o);
            // Pairs with no path along the surface: a plain line, so the click still shows where it went.
            for (int i : plan.failed)
            {
                const glm::dvec3 a = i < int(mPoints.size()) ? mPoints[size_t(i)].world : mHover->second.world;
                const size_t j = (size_t(i) + 1) % pts.size();
                const glm::dvec3 b = j < mPoints.size() ? mPoints[j].world : mHover->second.world;
                if (auto sa = view.project(a), sb = view.project(b); sa && sb)
                    p.dashedLine(*sa, *sb, 1.5f, 6.0f, kFailed);
            }
        }
        for (const geo::KnifeSegment& seg : plan.segments)
        {
            const glm::dvec3 a = toWorld(geo::knifePosition(m, seg.a)), b = toWorld(geo::knifePosition(m, seg.b));
            auto sa = view.project(a), sb = view.project(b);
            if (!sa || !sb)
                continue;
            p.line(*sa, *sb, seg.mirrored ? 1.5f : 2.0f, seg.mirrored ? kMirror : kCut);
            for (const KnifePoint* k : {&seg.a, &seg.b})
                if (k->kind == Kind::Edge)
                    p.disc(k == &seg.a ? *sa : *sb, 3.0f, seg.mirrored ? kMirror : kNewVertex, 12);
        }

        // The hovered edge or vertex, and the edge percentage while on an edge.
        if (hoverUsable)
        {
            const KnifePoint& k = mHover->second.point;
            if (k.kind == Kind::Edge && m.edgeValid(k.id))
            {
                auto a = view.project(toWorld(m.position(m.edgePoint(k.id, 0)))), b = view.project(toWorld(m.position(m.edgePoint(k.id, 1))));
                if (a && b)
                    p.line(*a, *b, 2.5f, kHot);
            }
            if (auto s = view.project(mHover->second.world))
            {
                p.disc(*s, k.kind == Kind::Face ? 3.5f : 5.5f, kRim, 16);
                p.disc(*s, k.kind == Kind::Face ? 2.0f : 3.5f, kHot, 16);
                if (k.kind == Kind::Edge)
                    p.label(*s + QPointF(12, -10), QString("%1%").arg(k.t * 100.0, 0, 'f', 0));
            }
        }
    }

    for (size_t i = 0; i < mPoints.size(); ++i)
        if (auto s = view.project(mPoints[i].world))
        {
            const bool hot = i == 0 && mHoverFirst;
            p.disc(*s, hot ? 8.0f : 6.0f, kRim, 16);
            p.disc(*s, hot ? 6.0f : 4.0f, kHot, 16);
        }

    QString help = "Knife: click to cut, Enter/RMB finish, Ctrl+click edge: loop, Shift: snap %1%, Backspace: remove point, Esc: cancel";
    help = help.arg(snapStepFraction() * 100.0, 0, 'g', 3);
    help += QString("   A angle: %1   X through: %2").arg(mAngle ? "on" : "off", mThrough ? "on" : "off");
    drawToolMessage(p, view, help);
}

void registerKnifeActions()
{
    actions::add({
        .id = "mesh.knife",
        .label = "Knife",
        .category = "Mesh",
        .run = [] { viewportTools::set(std::make_unique<KnifeTool>()); },
        .doc = "Multi-cut knife: click points across a mesh to cut it, snapping to vertices and edges (Shift: snap step), Ctrl+click an "
               "edge for an edge loop, Enter or RMB finishes, A angle constraint, X cut through, Esc exits.",
        .python = "lila.ui.run_action(\"mesh.knife\")",
    });
}
} // namespace lila::ui
