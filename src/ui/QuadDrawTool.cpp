#include "ui/QuadDrawTool.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Topology.h"
#include "retopo/RetopoCommands.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"

#include <QApplication>
#include <QPainter>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>

namespace lila::ui
{
namespace
{
using geo::Index;
using geo::kInvalid;

constexpr double kPickRadius = 12.0; // px
constexpr double kWeldRadius = 10.0; // px

// Overlay colours are sRGB; the 3D streams take linear.
const glm::vec4 kDotFill(0.95f, 0.95f, 0.95f, 1.0f);
const glm::vec4 kDotRim(0.08f, 0.08f, 0.08f, 1.0f);
const glm::vec4 kGhostDot(0.6f, 0.6f, 0.6f, 0.8f);
const glm::vec4 kVertexFill(0.15f, 0.15f, 0.15f, 1.0f);
const glm::vec4 kVertexRim(0.8f, 0.8f, 0.8f, 1.0f);
const glm::vec4 kHot(1.0f, 1.0f, 1.0f, 1.0f);
const glm::vec4 kBrush(0.85f, 0.85f, 0.85f, 0.8f);
const glm::vec4 kWireLinear(0.35f, 0.35f, 0.35f, 1.0f);
const glm::vec4 kHotLinear(1.0f, 1.0f, 1.0f, 1.0f);
const glm::vec4 kFillPreview(0.55f, 0.55f, 0.55f, 0.5f);
const glm::vec4 kDeleteFace(0.8f, 0.8f, 0.8f, 0.6f);

double distPx(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

double segmentDistPx(QPointF p, QPointF a, QPointF b, double* t = nullptr)
{
    QPointF ab = b - a;
    double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double u = len2 > 0.0 ? std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2, 0.0, 1.0) : 0.0;
    if (t)
        *t = u;
    return distPx(p, a + ab * u);
}

bool insidePolygon(QPointF p, const std::vector<QPointF>& poly)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y() > p.y()) != (poly[j].y() > p.y()) &&
            p.x() < (poly[j].x() - poly[i].x()) * (p.y() - poly[i].y()) / (poly[j].y() - poly[i].y()) + poly[i].x())
            in = !in;
    return in;
}

bool convex(const std::vector<QPointF>& poly)
{
    int sign = 0;
    for (size_t i = 0; i < poly.size(); ++i)
    {
        QPointF a = poly[i], b = poly[(i + 1) % poly.size()], c = poly[(i + 2) % poly.size()];
        double cr = (b.x() - a.x()) * (c.y() - b.y()) - (b.y() - a.y()) * (c.x() - b.x());
        int s = cr > 0.0 ? 1 : cr < 0.0 ? -1 : 0;
        if (s == 0)
            return false;
        if (sign == 0)
            sign = s;
        else if (s != sign)
            return false;
    }
    return true;
}

json vec(const glm::dvec3& p) { return json::array({p.x, p.y, p.z}); }

json vecs(const std::vector<glm::dvec3>& ps)
{
    json out = json::array();
    for (const glm::dvec3& p : ps)
        out.push_back(vec(p));
    return out;
}

/** An X, Maya's Quad Draw delete cursor. */
QCursor makeDeleteCursor()
{
    const int size = 24;
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing);
    for (auto [width, color] : {std::pair{4.5, QColor(20, 20, 20)}, std::pair{2.0, QColor(240, 240, 240)}})
    {
        painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(6, 6), QPointF(18, 18));
        painter.drawLine(QPointF(18, 6), QPointF(6, 18));
    }
    painter.end();
    return QCursor(pm, size / 2, size / 2);
}

/** Tab never reaches widgets (focus chain), and the tool needs its release too; this sees keys first. */
class KeyFilter : public QObject
{
public:
    explicit KeyFilter(QuadDrawTool* tool) : mTool(tool) {}

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease && e->type() != QEvent::ShortcutOverride)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        // The fill preview and delete cursor follow Shift and Ctrl without waiting for the mouse to move.
        if ((k->key() == Qt::Key_Shift || k->key() == Qt::Key_Control) && e->type() != QEvent::ShortcutOverride)
            viewportTools::requestRedraw();
        QWidget* focus = QApplication::focusWidget();
        if (!focus || obj != focus || !focus->inherits("lila::ui::ViewportWidget"))
            return false;
        if (k->key() == Qt::Key_Tab)
        {
            if (e->type() == QEvent::ShortcutOverride)
            {
                e->accept();
                return true;
            }
            if (!k->isAutoRepeat())
                mTool->setTabDown(e->type() == QEvent::KeyPress);
            return true;
        }
        if (e->type() == QEvent::KeyPress && k->key() == Qt::Key_Escape && k->modifiers() == Qt::NoModifier)
        {
            // Leaving deletes the tool, so it has to wait until this event is done with it.
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

private:
    QuadDrawTool* mTool;
};
} // namespace

QuadDrawTool::QuadDrawTool(scene::NodeId reference, std::string retopoName) : mReference(reference), mRetopoName(std::move(retopoName)) {}

QuadDrawTool::~QuadDrawTool() = default;

void QuadDrawTool::activate()
{
    mFilter = std::make_unique<KeyFilter>(this);
    qApp->installEventFilter(mFilter.get());
    refresh();
    log::info("Quad Draw on {} into {}: click dots, Shift-click to fill, Ctrl-drag loops, Tab-drag extends, Shift-drag relaxes, "
              "Ctrl+Shift-click deletes, Esc exits",
              mRefMesh ? scene::Graph::current().node(mReference)->name() : std::string("(no reference)"), mRetopoName);
}

void QuadDrawTool::deactivate()
{
    if (mFilter)
        qApp->removeEventFilter(mFilter.get());
    mFilter.reset();
}

void QuadDrawTool::refresh()
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* ref = g.node(mReference);
    scene::MeshPtr refMesh = ref && ref->type().isA("mesh") ? ref->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
    glm::dmat4 refWorld = ref ? ref->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    if (refMesh != mRefMesh || refWorld != mRefWorld)
    {
        mRefMesh = refMesh;
        mRefWorld = refWorld;
        if (refMesh)
        {
            geo::Mesh world = *refMesh;
            world.compact();
            for (glm::dvec3& p : world.positions())
                p = glm::dvec3(refWorld * glm::dvec4(p, 1.0));
            mRefBvh.build(world);
        }
        else
            mRefBvh = geo::MeshBvh();
    }
    mRetopo = nullptr;
    mSymmetry.reset();
    if (scene::Node* n = retopoNode())
    {
        mRetopoId = n->id();
        mRetopoName = n->name();
        mRetopo = scene::modifiers::cage(g, n->id());
        mRetopoWorld = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        int64_t axis = n->as<int64_t>(scene::MeshNode::kSymmetry);
        if (axis > 0 && axis <= 3)
        {
            // The object-space plane through the origin, carried into world space.
            glm::dvec3 nl(0.0);
            nl[int(axis - 1)] = 1.0;
            glm::dvec3 o = glm::dvec3(mRetopoWorld * glm::dvec4(0, 0, 0, 1));
            glm::dvec3 nw = glm::normalize(glm::transpose(glm::inverse(glm::dmat3(mRetopoWorld))) * nl);
            mSymmetry = geo::SymmetryPlane{nw, glm::dot(nw, o)};
        }
        const std::string& dots = n->as<std::string>(scene::MeshNode::kQuadDrawDots);
        if (dots != mDotsText || mRetopoWorld != mDotsWorld)
        {
            mDotsText = dots;
            mDotsWorld = mRetopoWorld;
            mDots = retopo::quadDrawDots(*n);
        }
    }
    else
    {
        mDots.clear();
        mDotsText.clear();
    }
    if (mHover.dot >= int(mDots.size()))
        mHover.dot = -1;
}

scene::Node* QuadDrawTool::retopoNode() const
{
    scene::Graph& g = scene::Graph::current();
    if (scene::Node* n = g.node(mRetopoId); n && n->type().isA("mesh"))
        return n;
    scene::Node* n = g.find(mRetopoName);
    return n && n->type().isA("mesh") ? n : nullptr;
}

bool QuadDrawTool::deleting() const
{
    Qt::KeyboardModifiers m = QGuiApplication::queryKeyboardModifiers();
    return (m & Qt::ShiftModifier) && (m & Qt::ControlModifier) && (mDrag == Drag::None || mDrag == Drag::DeleteClick);
}

std::optional<QCursor> QuadDrawTool::cursor() const
{
    static const QCursor kDelete = makeDeleteCursor();
    return deleting() ? std::optional<QCursor>(kDelete) : std::nullopt;
}

glm::dvec3 QuadDrawTool::pointWorld(Index p) const { return glm::dvec3(mRetopoWorld * glm::dvec4(mRetopo->position(p), 1.0)); }

bool QuadDrawTool::surfaceHit(QPointF px, const ViewInfo& view, glm::dvec3& out) const
{
    if (mRefBvh.empty())
        return false;
    glm::dvec3 o, d;
    view.ray(px, o, d);
    geo::BvhHit hit;
    if (!mRefBvh.raycast(o, d, hit))
        return false;
    out = hit.point;
    return true;
}

glm::dvec3 QuadDrawTool::onSurface(const glm::dvec3& p) const
{
    geo::BvhClosest c;
    return !mRefBvh.empty() && mRefBvh.closestPoint(p, c) ? c.point : p;
}

bool QuadDrawTool::visible(const glm::dvec3& p, const ViewInfo& view) const
{
    if (mRefBvh.empty())
        return true;
    glm::dvec3 d = p - view.eye;
    double len = glm::length(d);
    // A little slack so points lying on the surface aren't hidden by it.
    double slack = view.unitsPerPixel(p) * 3.0;
    return len <= slack || !mRefBvh.occluded(view.eye, d / len, len - slack);
}

std::vector<std::optional<QPointF>> QuadDrawTool::projectPoints(const ViewInfo& view) const
{
    std::vector<std::optional<QPointF>> out;
    if (!mRetopo)
        return out;
    out.resize(mRetopo->pointCapacity());
    for (Index p = 0; p < mRetopo->pointCapacity(); ++p)
        if (mRetopo->pointValid(p))
        {
            glm::dvec3 w = mMoved.contains(p) ? mMoved.at(p) : pointWorld(p);
            if (visible(w, view))
                out[p] = view.project(w);
        }
    return out;
}

QuadDrawTool::Hover QuadDrawTool::pick(QPointF px, const ViewInfo& view) const
{
    Hover h;
    double best = kPickRadius;
    for (size_t i = 0; i < mDots.size(); ++i)
        if (auto s = view.project(mDots[i]); s && distPx(*s, px) < best)
        {
            best = distPx(*s, px);
            h.dot = int(i);
        }
    if (!mRetopo)
        return h;
    const geo::Mesh& m = *mRetopo;
    auto screen = projectPoints(view);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (screen[p] && distPx(*screen[p], px) < best)
        {
            best = distPx(*screen[p], px);
            h.point = p;
            h.dot = -1;
        }
    if (h.point != kInvalid || h.dot >= 0)
        return h;
    double bestEdge = kPickRadius;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        auto a = screen[m.edgePoint(e, 0)], b = screen[m.edgePoint(e, 1)];
        if (!a || !b)
            continue;
        double d = segmentDistPx(px, *a, *b);
        if (d < bestEdge)
        {
            bestEdge = d;
            h.edge = e;
        }
    }
    // The face under the cursor, nearest first, by ray against its fan triangles.
    glm::dvec3 o, d;
    view.ray(px, o, d);
    double bestT = std::numeric_limits<double>::max();
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<Index> pts = m.facePoints(f);
        glm::dvec3 a = pointWorld(pts[0]);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
        {
            glm::dvec3 b = pointWorld(pts[k]), c = pointWorld(pts[k + 1]);
            glm::dvec3 e1 = b - a, e2 = c - a, pv = glm::cross(d, e2);
            double det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-18)
                continue;
            glm::dvec3 tv = o - a;
            double u = glm::dot(tv, pv) / det;
            glm::dvec3 qv = glm::cross(tv, e1);
            double v = glm::dot(d, qv) / det, t = glm::dot(e2, qv) / det;
            if (u >= 0.0 && v >= 0.0 && u + v <= 1.0 && t > 0.0 && t < bestT)
            {
                bestT = t;
                h.face = f;
            }
        }
    }
    if (h.face != kInvalid)
    {
        glm::dvec3 hit;
        // Faces behind the reference surface don't count.
        if (surfaceHit(px, view, hit) && glm::length(hit - o) + view.unitsPerPixel(hit) * 4.0 < bestT)
            h.face = kInvalid;
    }
    return h;
}

bool QuadDrawTool::run(const char* command, const json& call, json* result)
{
    try
    {
        json r = cmds::execute(command, call, CallSource::UI);
        if (result)
            *result = std::move(r);
        return true;
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
}

json QuadDrawTool::targetCall(json flags) const
{
    flags["mesh"] = mRetopoName;
    if (scene::Node* ref = scene::Graph::current().node(mReference))
        flags["reference"] = ref->name();
    flags["create"] = true;
    return flags;
}

bool QuadDrawTool::setDots(const std::vector<glm::dvec3>& dots)
{
    json name;
    if (!run("quadDrawSetDots", targetCall({{"dots", vecs(dots)}}), &name))
        return false;
    if (name.is_string())
        mRetopoName = name.get<std::string>();
    refresh();
    return true;
}

void QuadDrawTool::placeDot(QPointF px, const ViewInfo& view)
{
    glm::dvec3 hit;
    if (!surfaceHit(px, view, hit))
        return;
    // Too close to an existing vertex or dot: that one is already there to use.
    Hover h = pick(px, view);
    if (h.point != kInvalid || h.dot >= 0)
        return;
    if (mSymmetry && std::abs(mSymmetry->distance(hit)) < view.unitsPerPixel(hit) * kWeldRadius)
        hit = onSurface(mSymmetry->project(hit));
    std::vector<glm::dvec3> dots = mDots;
    dots.push_back(hit);
    setDots(dots);
}

std::optional<QuadDrawTool::Fill> QuadDrawTool::findFill(QPointF px, const ViewInfo& view) const
{
    // Over an existing face there is nothing to fill.
    if (mRetopo && pick(px, view).face != kInvalid)
        return std::nullopt;
    struct Candidate
    {
        QPointF screen;
        glm::dvec3 world;
        int dot;
    };
    std::vector<Candidate> cands;
    for (size_t i = 0; i < mDots.size(); ++i)
        if (auto s = view.project(mDots[i]))
            cands.push_back({*s, mDots[i], int(i)});
    if (mRetopo)
    {
        auto screen = projectPoints(view);
        for (Index p = 0; p < mRetopo->pointCapacity(); ++p)
            if (screen[p])
                cands.push_back({*screen[p], pointWorld(p), -1});
    }
    std::stable_sort(cands.begin(), cands.end(), [&](const Candidate& a, const Candidate& b) { return distPx(a.screen, px) < distPx(b.screen, px); });
    if (cands.size() > 7)
        cands.resize(7);

    // The smallest convex quad (else triangle) of nearby candidates that contains the cursor.
    std::vector<int> best;
    double bestPerimeter = std::numeric_limits<double>::max();
    for (int size : {4, 3})
    {
        int n = int(cands.size());
        std::vector<int> pick(size);
        auto tryCombo = [&]() {
            std::vector<int> order = pick;
            QPointF c(0, 0);
            for (int i : order)
                c += cands[i].screen / double(size);
            // Counter-clockwise as seen on screen (y points down), so the face points at the camera.
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return std::atan2(-(cands[a].screen.y() - c.y()), cands[a].screen.x() - c.x()) <
                       std::atan2(-(cands[b].screen.y() - c.y()), cands[b].screen.x() - c.x());
            });
            std::vector<QPointF> poly;
            for (int i : order)
                poly.push_back(cands[i].screen);
            if (!convex(poly) || !insidePolygon(px, poly))
                return;
            double per = 0.0;
            for (size_t k = 0; k < poly.size(); ++k)
                per += distPx(poly[k], poly[(k + 1) % poly.size()]);
            if (per < bestPerimeter)
            {
                bestPerimeter = per;
                best = order;
            }
        };
        std::function<void(int, int)> rec = [&](int start, int depth) {
            if (depth == size)
            {
                tryCombo();
                return;
            }
            for (int i = start; i < n; ++i)
            {
                pick[depth] = i;
                rec(i + 1, depth + 1);
            }
        };
        rec(0, 0);
        if (!best.empty())
            break;
    }
    if (best.empty())
        return std::nullopt;
    Fill fill;
    for (int i : best)
    {
        fill.points.push_back(cands[i].world);
        if (cands[i].dot >= 0)
            fill.dots.push_back(cands[i].dot);
    }
    return fill;
}

void QuadDrawTool::fillAt(QPointF px, const ViewInfo& view)
{
    std::optional<Fill> fill = findFill(px, view);
    if (!fill)
        return;
    std::vector<glm::dvec3> remaining;
    for (int i = 0; i < int(mDots.size()); ++i)
        if (std::find(fill->dots.begin(), fill->dots.end(), i) == fill->dots.end())
            remaining.push_back(mDots[i]);
    double weld = view.unitsPerPixel(fill->points[0]) * 0.5;
    // The face and the dots it used up are one undo step.
    UndoChunk chunk("Quad Draw Fill");
    json name;
    if (!run("quadDrawAddFace", targetCall({{"points", vecs(fill->points)}, {"weld", weld}}), &name))
        return;
    if (name.is_string())
        mRetopoName = name.get<std::string>();
    if (remaining.size() != mDots.size())
        setDots(remaining);
    refresh();
}

void QuadDrawTool::deleteAt(QPointF px, const ViewInfo& view)
{
    Hover h = pick(px, view);
    if (h.dot >= 0)
    {
        std::vector<glm::dvec3> dots = mDots;
        dots.erase(dots.begin() + h.dot);
        setDots(dots);
        return;
    }
    if (h.point != kInvalid)
        run("quadDrawDelete", {{"mesh", mRetopoName}, {"points", {h.point}}});
    else if (h.face != kInvalid)
        run("quadDrawDelete", {{"mesh", mRetopoName}, {"faces", {h.face}}});
    refresh();
}

void QuadDrawTool::relaxStep(QPointF px, const ViewInfo& view)
{
    if (!mRetopo)
        return;
    const geo::Mesh& m = *mRetopo;
    auto screen = projectPoints(view);
    auto at = [&](Index p) { return mMoved.contains(p) ? mMoved.at(p) : pointWorld(p); };
    std::map<Index, glm::dvec3> next;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!screen[p] || m.isBoundaryPoint(p))
            continue;
        double d = distPx(*screen[p], px);
        if (d > mBrush)
            continue;
        glm::dvec3 avg(0.0);
        int count = 0;
        m.forEachPointEdge(p, [&](Index e) {
            avg += at(m.edgeOtherPoint(e, p));
            ++count;
        });
        if (count == 0)
            continue;
        double falloff = 1.0 - d / mBrush;
        glm::dvec3 target = glm::mix(at(p), avg / double(count), 0.35 * falloff * (2.0 - falloff));
        next[p] = onSurface(target);
    }
    for (auto& [p, pos] : next)
        mMoved[p] = pos;
}

double QuadDrawTool::edgeFraction(Index edge, QPointF px, const ViewInfo& view) const
{
    auto a = view.project(pointWorld(mRetopo->edgePoint(edge, 0))), b = view.project(pointWorld(mRetopo->edgePoint(edge, 1)));
    if (!a || !b)
        return 0.5;
    double t = 0.5;
    segmentDistPx(px, *a, *b, &t);
    return std::clamp(t, 0.05, 0.95);
}

void QuadDrawTool::loopPreview(Index edge, double fraction, std::vector<std::pair<glm::dvec3, glm::dvec3>>& out) const
{
    // Walk the ring both ways, carrying which end of each edge is the "start" side so the fraction lines up.
    const geo::Mesh& m = *mRetopo;
    auto point = [&](Index e, Index startPoint) {
        Index other = m.edgeOtherPoint(e, startPoint);
        return glm::mix(pointWorld(startPoint), pointWorld(other), fraction);
    };
    std::vector<Index> seenFaces;
    for (int dir = 0; dir < 2; ++dir)
    {
        Index e = edge, start = m.edgePoint(edge, 0);
        std::vector<Index> faces;
        m.forEachEdgeCorner(e, [&](Index c) { faces.push_back(m.cornerFace(c)); });
        if (size_t(dir) >= faces.size())
            break;
        Index f = faces[dir];
        for (int guard = 0; guard < 100000 && f != kInvalid; ++guard)
        {
            if (std::find(seenFaces.begin(), seenFaces.end(), f) != seenFaces.end())
                break;
            seenFaces.push_back(f);
            Index opp = geo::quadOppositeEdge(m, e, f);
            if (opp == kInvalid)
                break;
            // The start side continues along the quad's side edge from start.
            Index next = kInvalid;
            m.forEachFaceCorner(f, [&](Index c) {
                Index a = m.cornerPoint(c), b = m.cornerNextPoint(c);
                if (m.cornerEdge(c) == e || m.cornerEdge(c) == opp)
                    return;
                if (a == start)
                    next = b;
                else if (b == start)
                    next = a;
            });
            if (next == kInvalid)
                break;
            out.push_back({point(e, start), point(opp, next)});
            Index nf = m.oppositeFace(opp, f);
            e = opp;
            start = next;
            f = nf;
        }
    }
}

bool QuadDrawTool::mousePress(QMouseEvent* e, const ViewInfo& view)
{
    refresh();
    mPressPx = e->position();
    mCursor = e->position();
    mCursorView = view.camera;
    mMods = e->modifiers();
    mMoved.clear();
    mDotTo.reset();
    Hover h = pick(e->position(), view);
    const bool shift = mMods & Qt::ShiftModifier, ctrl = mMods & Qt::ControlModifier;

    if (e->button() == Qt::MiddleButton)
    {
        if (h.point != kInvalid)
        {
            mDrag = Drag::Tweak;
            mDragPoint = h.point;
        }
        else if (h.dot >= 0)
        {
            mDrag = Drag::DotMove;
            mDragDot = h.dot;
        }
        else
            return false;
        return true;
    }
    if (e->button() != Qt::LeftButton)
        return false;
    if (shift && ctrl)
        mDrag = Drag::DeleteClick;
    else if (mTabDown)
    {
        if (h.edge == kInvalid || !mRetopo->isBoundaryEdge(h.edge))
            return true;
        mDrag = Drag::Extend;
        mDragEdge = h.edge;
        surfaceHit(e->position(), view, mDragStartHit);
        mExtendTo = {pointWorld(mRetopo->edgePoint(h.edge, 0)), pointWorld(mRetopo->edgePoint(h.edge, 1))};
    }
    else if (ctrl)
    {
        if (h.edge == kInvalid)
            return true;
        mDrag = Drag::Loop;
        mDragEdge = h.edge;
        mLoopFraction = edgeFraction(h.edge, e->position(), view);
    }
    else if (shift)
        mDrag = Drag::FillClick;
    else if (h.point != kInvalid)
    {
        mDrag = Drag::Tweak;
        mDragPoint = h.point;
    }
    else if (h.dot >= 0)
    {
        mDrag = Drag::DotMove;
        mDragDot = h.dot;
    }
    else
        mDrag = Drag::DotClick;
    return true;
}

bool QuadDrawTool::mouseMove(QMouseEvent* e, const ViewInfo& view)
{
    mCursor = e->position();
    mCursorView = view.camera;
    mMods = e->modifiers();
    if (mDrag == Drag::None)
    {
        refresh();
        mHover = pick(e->position(), view);
        if (mMods & Qt::ControlModifier && mHover.edge != kInvalid)
            mLoopFraction = edgeFraction(mHover.edge, e->position(), view);
        viewportTools::requestRedraw();
        return true;
    }
    const bool moved = distPx(e->position(), mPressPx) > 4.0;
    switch (mDrag)
    {
    case Drag::FillClick:
        if (moved)
            mDrag = Drag::Relax;
        break;
    case Drag::Relax: relaxStep(e->position(), view); break;
    case Drag::Tweak:
    {
        glm::dvec3 hit;
        if (surfaceHit(e->position(), view, hit))
            mMoved[mDragPoint] = hit;
        break;
    }
    case Drag::DotMove:
    {
        glm::dvec3 hit;
        if (moved && surfaceHit(e->position(), view, hit))
            mDotTo = hit;
        break;
    }
    case Drag::Loop:
    {
        // Follow the cursor onto whichever edge of the ring it is over.
        Hover h = pick(e->position(), view);
        if (h.edge != kInvalid)
            mDragEdge = h.edge;
        mLoopFraction = edgeFraction(mDragEdge, e->position(), view);
        break;
    }
    case Drag::Extend:
    {
        glm::dvec3 hit;
        if (surfaceHit(e->position(), view, hit))
        {
            glm::dvec3 offset = hit - mDragStartHit;
            mExtendTo = {onSurface(pointWorld(mRetopo->edgePoint(mDragEdge, 0)) + offset),
                         onSurface(pointWorld(mRetopo->edgePoint(mDragEdge, 1)) + offset)};
        }
        break;
    }
    default: break;
    }
    viewportTools::requestRedraw();
    return true;
}

bool QuadDrawTool::mouseRelease(QMouseEvent* e, const ViewInfo& view)
{
    mCursor = e->position();
    commitDrag(view);
    mDrag = Drag::None;
    mMoved.clear();
    mDotTo.reset();
    mDragDot = -1;
    refresh();
    mHover = pick(e->position(), view);
    return true;
}

void QuadDrawTool::commitDrag(const ViewInfo& view)
{
    switch (mDrag)
    {
    case Drag::DotClick: placeDot(mCursor, view); break;
    case Drag::FillClick: fillAt(mCursor, view); break;
    case Drag::DeleteClick: deleteAt(mCursor, view); break;
    case Drag::DotMove:
        if (mDotTo && mDragDot >= 0 && mDragDot < int(mDots.size()))
        {
            glm::dvec3 to = *mDotTo;
            if (mSymmetry && std::abs(mSymmetry->distance(to)) < view.unitsPerPixel(to) * kWeldRadius)
                to = onSurface(mSymmetry->project(to));
            std::vector<glm::dvec3> dots = mDots;
            dots[mDragDot] = to;
            setDots(dots);
        }
        break;
    case Drag::Loop:
        if (mRetopo && mRetopo->edgeValid(mDragEdge))
            run("quadDrawInsertLoop", {{"mesh", mRetopoName}, {"edge", mDragEdge}, {"fraction", mLoopFraction}});
        break;
    case Drag::Extend:
        if (distPx(mCursor, mPressPx) > 4.0 && mRetopo && mRetopo->edgeValid(mDragEdge))
        {
            Index a = mRetopo->edgePoint(mDragEdge, 0), b = mRetopo->edgePoint(mDragEdge, 1);
            double weld = view.unitsPerPixel(pointWorld(a)) * 0.5;
            run("quadDrawAddFace", targetCall({{"points", {vec(pointWorld(a)), vec(pointWorld(b)), vec(mExtendTo[1]), vec(mExtendTo[0])}},
                                               {"weld", weld}}));
        }
        break;
    case Drag::Tweak:
    case Drag::Relax:
        if (!mMoved.empty())
        {
            json points = json::array(), positions = json::array();
            for (auto& [p, pos] : mMoved)
            {
                points.push_back(p);
                positions.push_back(vec(pos));
            }
            double weld = 0.0;
            if (mDrag == Drag::Tweak)
            {
                // Dropped onto another vertex: snap to it and let the command weld them.
                auto screen = projectPoints(view);
                for (Index p = 0; p < screen.size(); ++p)
                    if (p != mDragPoint && screen[p] && distPx(*screen[p], mCursor) < kWeldRadius)
                    {
                        positions[0] = vec(pointWorld(p));
                        weld = view.unitsPerPixel(pointWorld(p)) * 0.5;
                        break;
                    }
            }
            run("quadDrawMove", {{"mesh", mRetopoName}, {"points", points}, {"positions", positions}, {"weld", weld}});
        }
        break;
    default: break;
    }
}

bool QuadDrawTool::wheel(QWheelEvent* e, const ViewInfo&)
{
    if (!(e->modifiers() & Qt::ShiftModifier))
        return false;
    // Shift turns the wheel's vertical delta horizontal on some platforms.
    int delta = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    mBrush = std::clamp(mBrush * std::pow(1.1, delta / 120.0), 10.0, 400.0);
    viewportTools::requestRedraw();
    return true;
}

bool QuadDrawTool::keyPress(QKeyEvent* e, const ViewInfo&)
{
    if (e->key() == Qt::Key_Escape)
    {
        QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
        return true;
    }
    if (e->key() == Qt::Key_Backspace && !mDots.empty())
    {
        std::vector<glm::dvec3> dots = mDots;
        dots.pop_back();
        setDots(dots);
        return true;
    }
    return false;
}

void QuadDrawTool::removeOwnDraw(render::FrameState& s) const
{
    // The retopo mesh is drawn here, see-through; drop the viewport's opaque draw of it.
    if (!mRetopo)
        return;
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.find(mRetopoName);
    if (!n)
        return;
    scene::MeshPtr out = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!out)
        return;
    geo::Bounds b = out->bounds();
    glm::mat4 world(mRetopoWorld);
    float tol = float(std::max(1e-4, glm::length(b.size()) * 1e-4));
    std::erase_if(s.meshes, [&](const render::MeshDraw& d) {
        if (!d.mesh || d.world != world)
            return false;
        return glm::length(d.mesh->boundsMin - glm::vec3(b.min)) < tol && glm::length(d.mesh->boundsMax - glm::vec3(b.max)) < tol;
    });
}

void QuadDrawTool::draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    refresh();
    removeOwnDraw(s);
    if (mDrag == Drag::None)
        mMods = QGuiApplication::queryKeyboardModifiers();
    const bool here = view.camera == mCursorView;
    const bool shift = mMods & Qt::ShiftModifier, ctrl = mMods & Qt::ControlModifier;
    const bool del = here && deleting();
    auto world = [&](Index i) { return mMoved.contains(i) ? mMoved.at(i) : pointWorld(i); };

    if (mRetopo)
    {
        const geo::Mesh& m = *mRetopo;
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            std::vector<Index> pts = m.facePoints(f);
            glm::dvec3 a = world(pts[0]), n(0.0);
            for (size_t k = 1; k + 1 < pts.size(); ++k)
                n += glm::cross(world(pts[k]) - a, world(pts[k + 1]) - a);
            double len = glm::length(n);
            double facing = len > 0.0 ? std::abs(glm::dot(n / len, view.viewDir(a))) : 1.0;
            float g = float(0.04 + 0.2 * facing);
            glm::vec4 c(g, g, g, 0.45f);
            for (size_t k = 1; k + 1 < pts.size(); ++k)
                for (Index q : {pts[0], pts[k], pts[k + 1]})
                    s.tris3d.push_back({glm::vec3(world(q)), c});
        }
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e))
            {
                glm::vec4 c = here && e == mHover.edge && mDrag == Drag::None && !shift ? kHotLinear : kWireLinear;
                s.linesOnTop.push_back({glm::vec3(world(m.edgePoint(e, 0))), c});
                s.linesOnTop.push_back({glm::vec3(world(m.edgePoint(e, 1))), c});
            }

        // Edge loop preview while Ctrl is held over an edge, or during the drag.
        Index loopEdge = mDrag == Drag::Loop ? mDragEdge : (here && mDrag == Drag::None && ctrl && !shift) ? mHover.edge : kInvalid;
        if (loopEdge != kInvalid && m.edgeValid(loopEdge))
        {
            std::vector<std::pair<glm::dvec3, glm::dvec3>> segs;
            loopPreview(loopEdge, mLoopFraction, segs);
            for (auto& [a, b] : segs)
            {
                s.linesOnTop.push_back({glm::vec3(a), kHotLinear});
                s.linesOnTop.push_back({glm::vec3(b), kHotLinear});
            }
        }
        if (mDrag == Drag::Extend && m.edgeValid(mDragEdge))
        {
            glm::dvec3 q[4] = {pointWorld(m.edgePoint(mDragEdge, 0)), pointWorld(m.edgePoint(mDragEdge, 1)), mExtendTo[1], mExtendTo[0]};
            for (int k : {0, 1, 2, 0, 2, 3})
                s.tris3d.push_back({glm::vec3(q[k]), glm::vec4(0.6f, 0.6f, 0.6f, 0.5f)});
            for (int k = 0; k < 4; ++k)
            {
                s.linesOnTop.push_back({glm::vec3(q[k]), kHotLinear});
                s.linesOnTop.push_back({glm::vec3(q[(k + 1) % 4]), kHotLinear});
            }
        }

        auto screen = projectPoints(view);
        for (Index i = 0; i < screen.size(); ++i)
            if (screen[i])
            {
                bool hot = (here && i == mHover.point && !del) || (mDrag == Drag::Tweak && i == mDragPoint);
                p.disc(*screen[i], hot ? 6.0f : 4.0f, hot ? kHot : kVertexRim, 16);
                p.disc(*screen[i], hot ? 3.5f : 2.5f, hot ? kHot : kVertexFill, 12);
            }
    }

    for (size_t i = 0; i < mDots.size(); ++i)
    {
        glm::dvec3 dot = mDrag == Drag::DotMove && int(i) == mDragDot && mDotTo ? *mDotTo : mDots[i];
        if (mSymmetry)
            if (auto g = view.project(mSymmetry->reflect(dot)); g && visible(mSymmetry->reflect(dot), view))
                p.disc(*g, 3.5f, kGhostDot, 12);
        if (!visible(dot, view))
            continue;
        if (auto sp = view.project(dot))
        {
            bool hot = (here && int(i) == mHover.dot && !del) || (mDrag == Drag::DotMove && int(i) == mDragDot);
            p.disc(*sp, hot ? 6.5f : 5.0f, kDotRim, 16);
            p.disc(*sp, hot ? 5.0f : 3.5f, kDotFill, 16);
        }
    }

    std::optional<Fill> fill;
    if (here && shift && !ctrl && (mDrag == Drag::None || mDrag == Drag::FillClick))
        fill = findFill(mCursor, view);
    if (fill)
    {
        const std::vector<glm::dvec3>& q = fill->points;
        for (size_t k = 1; k + 1 < q.size(); ++k)
            for (const glm::dvec3& v : {q[0], q[k], q[k + 1]})
                s.tris3d.push_back({glm::vec3(v), kFillPreview});
        for (size_t k = 0; k < q.size(); ++k)
        {
            s.linesOnTop.push_back({glm::vec3(q[k]), kHotLinear});
            s.linesOnTop.push_back({glm::vec3(q[(k + 1) % q.size()]), kHotLinear});
        }
    }
    if (del)
        drawDeleteTarget(s, p, view);

    // The relax brush, while relaxing or when Shift has nothing to fill.
    if (here && shift && !ctrl && (mDrag == Drag::Relax || (!fill && (mDrag == Drag::None || mDrag == Drag::FillClick))))
    {
        std::vector<QPointF> circle;
        for (int k = 0; k < 48; ++k)
        {
            double a = k / 48.0 * 6.283185307179586;
            circle.push_back(mCursor + QPointF(std::cos(a), std::sin(a)) * mBrush);
        }
        p.polyline(circle, 1.5f, kBrush, true);
    }
    if (mRefBvh.empty())
        drawToolMessage(p, view, "Quad Draw: select a reference mesh first");
}

void QuadDrawTool::drawDeleteTarget(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) const
{
    if (mHover.dot >= 0 && mHover.dot < int(mDots.size()))
    {
        if (auto sp = view.project(mDots[mHover.dot]))
        {
            p.disc(*sp, 8.0f, kHot, 16);
            p.disc(*sp, 5.0f, kDotRim, 16);
        }
        return;
    }
    if (!mRetopo)
        return;
    const geo::Mesh& m = *mRetopo;
    // Deleting a vertex takes its faces with it, so those light up too.
    std::vector<Index> faces;
    if (mHover.point != kInvalid && m.pointValid(mHover.point))
    {
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                if (std::vector<Index> pts = m.facePoints(f); std::find(pts.begin(), pts.end(), mHover.point) != pts.end())
                    faces.push_back(f);
    }
    else if (mHover.face != kInvalid && m.faceValid(mHover.face))
        faces.push_back(mHover.face);
    for (Index f : faces)
    {
        std::vector<Index> pts = m.facePoints(f);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
            for (Index q : {pts[0], pts[k], pts[k + 1]})
                s.tris3d.push_back({glm::vec3(pointWorld(q)), kDeleteFace});
        for (size_t k = 0; k < pts.size(); ++k)
        {
            s.linesOnTop.push_back({glm::vec3(pointWorld(pts[k])), kHotLinear});
            s.linesOnTop.push_back({glm::vec3(pointWorld(pts[(k + 1) % pts.size()])), kHotLinear});
        }
    }
    if (mHover.point != kInvalid && m.pointValid(mHover.point))
        if (auto sp = view.project(pointWorld(mHover.point)))
        {
            p.disc(*sp, 7.0f, kHot, 16);
            p.disc(*sp, 4.0f, kVertexFill, 12);
        }
}

void registerRetopoActions()
{
    actions::add({
        "retopo.quadDraw",
        "Quad Draw",
        "Mesh",
        [] {
            scene::Graph& g = scene::Graph::current();
            std::optional<retopo::QuadDrawTargets> t = retopo::resolveQuadDraw(g, g.selection());
            if (!t)
            {
                log::warn("Quad Draw: select the reference mesh, or a retopo mesh to resume on");
                return;
            }
            viewportTools::set(std::make_unique<QuadDrawTool>(t->reference, t->retopo));
        },
        nullptr,
        "Draw a new quad mesh over the selected reference, or resume on a selected retopo mesh: click dots, Shift-click to fill, "
        "Ctrl-drag loops, Tab-drag extends, Shift-drag relaxes, MMB-drag tweaks, Ctrl+Shift-click deletes. Esc or Q/W/E/R exits.",
        "lila.ui.run_action(\"retopo.quadDraw\")",
    });
}
} // namespace lila::ui
