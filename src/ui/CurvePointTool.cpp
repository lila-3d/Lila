#include "ui/CurveUi.h"
#include "ui/CurveToolsCommon.h"

#include "core/Log.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "ui/Actions.h"
#include "ui/CurvePick.h"
#include "ui/Isolate.h"
#include "ui/ViewportTool.h"

#include <algorithm>
#include <map>

namespace lila::ui
{
namespace
{
using namespace curveTools;

const glm::vec4 kCurvePointColor(1.0f, 0.85f, 0.15f, 1.0f);
const glm::vec4 kExistingPointColor(0.75f, 0.35f, 0.95f, 1.0f);

/** A curve point: a place along a spline, not one of its control points (Maya's curve point component). */
struct CurvePoint
{
    scene::NodeId node = scene::kNoNode;
    size_t spline = 0;
    double u = 0.0;
};

std::vector<CurvePoint>& picked()
{
    static std::vector<CurvePoint> p;
    return p;
}

bool curveSelected()
{
    scene::Graph& g = scene::Graph::current();
    return std::ranges::any_of(g.selection(), [&](scene::NodeId id) {
        const scene::Node* n = g.node(id);
        return n && n->type().isA("curve");
    });
}

/** The selected curves' splines, world space, for picking along. */
std::vector<curvePick::SceneCurve> selectedCurves()
{
    std::vector<curvePick::SceneCurve> out;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    curve::TessOptions o;
    o.chordError = 0.005;
    o.maxAngle = 0.05;
    for (scene::NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("curve") || !scene::effectivelyVisible(g, id) || !isolate::shows(scene::isolation::active(), id))
            continue;
        const curve::Splines& ss = curve::splinesOf(n->value(curve::CurveShape::kWorldSpace));
        for (size_t k = 0; k < std::min(ss.size(), curve::ownSplineCount(*n)); ++k)
            if (curve::isValid(ss[k]) && curve::spanCount(ss[k]) > 0)
                out.push_back({id, n->path(), k, ss[k], curve::tessellate(ss[k], o)});
    }
    return out;
}

/**
 * Inserts a point at every curve point, then selects the new points in vertex mode. Per spline the highest parameter
 * goes first, so the parameters still to insert keep meaning the same place.
 */
void insertAtCurvePoints()
{
    std::vector<CurvePoint> pts = picked();
    if (pts.empty())
        return;
    std::ranges::sort(pts, [](const CurvePoint& a, const CurvePoint& b) {
        return std::tie(a.node, a.spline, b.u) < std::tie(b.node, b.spline, a.u);
    });
    scene::Graph& g = scene::Graph::current();
    std::map<std::pair<scene::NodeId, size_t>, std::vector<size_t>> made;
    try
    {
        UndoChunk chunk("Insert Curve Points");
        for (const CurvePoint& p : pts)
        {
            const scene::Node* n = g.node(p.node);
            if (!n)
                continue;
            json at = ops::execute("insertCurvePoint", {{"args", {n->path()}}, {"parameter", p.u}, {"spline", int(p.spline)}}, CallSource::UI);
            if (!at.is_number_integer())
                continue;
            const size_t index = at.get<size_t>();
            std::vector<size_t>& ids = made[{p.node, p.spline}];
            for (size_t& i : ids)
                if (i >= index)
                    ++i;
            ids.push_back(index);
        }
        picked().clear();
        scene::components::setModeUndoable(scene::ComponentMode::Vertex);
        bool first = true;
        for (scene::NodeId node : g.selection())
        {
            scene::components::Ids ids;
            for (const auto& [key, points] : made)
                if (key.first == node)
                    for (size_t i : points)
                        ids.push_back(curve::componentId(key.second, i, curve::PointRole::Point));
            if (ids.empty())
                continue;
            scene::components::select(node, scene::ComponentMode::Vertex, ids, first ? scene::components::Op::Replace : scene::components::Op::Add);
            first = false;
        }
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    viewportTools::requestRedraw();
}

/**
 * Curve Point mode: click a selected curve to pick a place along it, Shift+click to add more, Ctrl+click to drop the
 * nearest; dragging slides the pick along the curve. The picks feed Insert Point.
 */
class CurvePointTool final : public ViewportTool
{
public:
    const char* name() const override { return "Curve Point"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    void activate() override
    {
        picked().clear();
        log::info("Curve Point: click a selected curve to pick a point on it, Shift+click adds, Ctrl+click removes, drag slides");
    }
    void deactivate() override { picked().clear(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mCurves = selectedCurves();
        auto hit = curvePick::nearestCurve(mCurves, e->position(), view, kSnapPx);
        std::vector<CurvePoint>& pts = picked();
        if (e->modifiers() & Qt::ControlModifier)
        {
            if (auto i = pickedNear(e->position(), view))
                pts.erase(pts.begin() + std::ptrdiff_t(*i));
        }
        else if (hit)
        {
            if (!(e->modifiers() & Qt::ShiftModifier))
                pts.clear();
            const curvePick::SceneCurve& c = mCurves[hit->curve];
            pts.push_back({c.node, c.spline, hit->u});
            mDragging = pts.size() - 1;
        }
        else if (!(e->modifiers() & Qt::ShiftModifier))
            pts.clear();
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mCurves.empty() || !mDragging)
            mCurves = selectedCurves();
        mHover = curvePick::nearestCurve(mCurves, e->position(), view, kSnapPx);
        if (mDragging && *mDragging < picked().size() && mHover)
        {
            CurvePoint& p = picked()[*mDragging];
            const curvePick::SceneCurve& c = mCurves[mHover->curve];
            // Slides along the curve it was picked on; the cursor passing over another curve doesn't move it there.
            if (c.node == p.node && c.spline == p.spline)
                p.u = mHover->u;
        }
        viewportTools::requestRedraw();
        return mDragging.has_value();
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mDragging.reset();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter: insertAtCurvePoints(); return true;
        case Qt::Key_Escape:
            if (picked().empty())
                exitTool();
            picked().clear();
            viewportTools::requestRedraw();
            return true;
        default: return false;
        }
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        scene::Graph& g = scene::Graph::current();
        drawExistingPoints(g, p, view);
        for (const CurvePoint& c : picked())
            if (scene::Node* n = g.node(c.node))
            {
                const curve::Splines& ss = curve::splinesOf(n->value(curve::CurveShape::kWorldSpace));
                if (c.spline < ss.size())
                    dot(p, view, curve::pointAt(ss[c.spline], c.u), 4.5f, kCurvePointColor);
            }
        if (mHover && !mDragging)
            dot(p, view, mHover->point, 3.0f, kGhost);
        drawToolMessage(p, view,
                        curveSelected() ? QString("Curve Point: click a curve to pick a point, Shift+click: add, Ctrl+click: remove, drag: slide, "
                                                  "Enter or Shift+RMB > Insert Point: insert, Esc: exit")
                                        : QString("Curve Point: select a curve first, Esc: exit"));
    }

private:
    /**
     * The selected curves' own points, so a new one can be placed relative to them: points that lie on the curve (Bezier
     * anchors, NURBS edit points) as squares, NURBS control points off it as small dots.
     */
    static void drawExistingPoints(scene::Graph& g, OverlayPainter& p, const ViewInfo& view)
    {
        auto square = [&](const D3& w) {
            if (auto q = view.project(w))
                p.rect(QRectF(*q - QPointF(3, 3), QSizeF(6, 6)), kExistingPointColor);
        };
        for (scene::NodeId id : g.selection())
        {
            scene::Node* n = g.node(id);
            if (!n || !n->type().isA("curve"))
                continue;
            for (const curve::Spline& s : curve::splinesOf(n->value(curve::CurveShape::kWorldSpace)))
            {
                if (!curve::isValid(s))
                    continue;
                if (s.kind != curve::Kind::Nurbs)
                {
                    for (const D3& q : s.points)
                        square(q);
                    continue;
                }
                for (const D3& q : s.points)
                    dot(p, view, q, 2.0f, kGhost);
                const int spans = curve::spanCount(s);
                for (int k = 0; k <= spans; ++k)
                    if (!s.closed || k < spans)
                        square(curve::pointAt(s, double(k)));
            }
        }
    }

    std::optional<size_t> pickedNear(QPointF px, const ViewInfo& view) const
    {
        scene::Graph& g = scene::Graph::current();
        std::optional<size_t> best;
        double bestPx = kSnapPx;
        for (size_t i = 0; i < picked().size(); ++i)
        {
            const CurvePoint& c = picked()[i];
            scene::Node* n = g.node(c.node);
            if (!n)
                continue;
            const curve::Splines& ss = curve::splinesOf(n->value(curve::CurveShape::kWorldSpace));
            if (c.spline >= ss.size())
                continue;
            if (auto q = view.project(curve::pointAt(ss[c.spline], c.u)); q && distPx(*q, px) < bestPx)
            {
                bestPx = distPx(*q, px);
                best = i;
            }
        }
        return best;
    }

    std::vector<curvePick::SceneCurve> mCurves;
    std::optional<curvePick::CurveHover> mHover;
    std::optional<size_t> mDragging;
};
} // namespace

void registerCurvePointTool()
{
    if (!viewportTools::registry().find("curve.curvePointMode"))
        viewportTools::add({.id = "curve.curvePointMode", .label = "Curve Point", .icon = "point",
                            .create = [] { return std::make_unique<CurvePointTool>(); }});
    actions::add({.id = "curve.curvePointMode", .label = "Curve Point", .category = "Curves",
                  .run = [] { viewportTools::activate("curve.curvePointMode"); }, .enabled = curveSelected,
                  .doc = "Pick points anywhere along the selected curves (Shift+click adds, Ctrl+click removes, drag slides), for Insert Point.",
                  .python = "lila.ui.run_action(\"curve.curvePointMode\")"});
    actions::add({.id = "curve.insertAtCurvePoints", .label = "Insert Point", .category = "Curves", .run = insertAtCurvePoints,
                  .enabled = [] { return !picked().empty(); },
                  .doc = "Inserts a curve point at each point picked in Curve Point mode, keeping the curve's shape, and selects the new points.",
                  .python = "lila.ops.insertCurvePoint(\"Curve\", parameter=1.5)"});
}
} // namespace lila::ui
