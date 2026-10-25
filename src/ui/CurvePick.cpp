#include "ui/CurvePick.h"

#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Isolate.h"

#include <cmath>
#include <limits>

namespace lila::ui::curvePick
{
namespace
{
/** Distance from p to segment ab in pixels, and the fraction along it. */
std::pair<double, double> toSegment(QPointF p, QPointF a, QPointF b)
{
    QPointF d = b - a;
    double len2 = QPointF::dotProduct(d, d);
    double t = len2 > 0.0 ? std::clamp(QPointF::dotProduct(p - a, d) / len2, 0.0, 1.0) : 0.0;
    QPointF q = a + d * t - p;
    return {std::hypot(q.x(), q.y()), t};
}

bool shown(scene::Graph& g, scene::NodeId id) { return scene::effectivelyVisible(g, id) && isolate::shows(scene::isolation::active(), id); }
} // namespace

std::vector<SceneCurve> visibleCurves()
{
    LILA_ZONE();

    std::vector<SceneCurve> out;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    curve::TessOptions o;
    o.chordError = 0.005;
    o.maxAngle = 0.05;
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("curve") || !shown(g, id))
            continue;
        const curve::Splines& ss = curve::splinesOf(n->value(curve::CurveShape::kWorldSpace));
        for (size_t k = 0; k < ss.size(); ++k)
        {
            if (!curve::isValid(ss[k]) || curve::spanCount(ss[k]) == 0)
                continue;
            SceneCurve c;
            c.node = id;
            c.path = n->path();
            c.spline = k;
            c.s = ss[k];
            c.pl = curve::tessellate(ss[k], o);
            out.push_back(std::move(c));
        }
    }
    return out;
}

std::optional<CurveHover> nearestCurve(const std::vector<SceneCurve>& curves, QPointF px, const ViewInfo& view, double maxPx)
{
    std::optional<CurveHover> best;
    for (size_t ci = 0; ci < curves.size(); ++ci)
    {
        const curve::Polyline& pl = curves[ci].pl;
        const size_t n = pl.size();
        const size_t segs = pl.closed ? n : (n ? n - 1 : 0);
        const double spans = curve::spanCount(curves[ci].s);
        for (size_t i = 0; i < segs; ++i)
        {
            size_t j = (i + 1) % n;
            auto a = view.project(pl.points[i]), b = view.project(pl.points[j]);
            if (!a || !b)
                continue;
            auto [d, t] = toSegment(px, *a, *b);
            if (d > maxPx || (best && d >= best->px))
                continue;
            double u1 = j == 0 ? spans : pl.params[j];
            double u = pl.params[i] + (u1 - pl.params[i]) * t;
            best = CurveHover{ci, u, curve::pointAt(curves[ci].s, u), d};
        }
    }
    return best;
}

std::optional<BorderHover> nearestBorder(QPointF px, const ViewInfo& view, double maxPx)
{
    std::optional<BorderHover> best;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || !shown(g, id))
            continue;
        scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!m)
            continue;
        glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        {
            if (!m->edgeValid(e) || !m->isBoundaryEdge(e))
                continue;
            D3 pa = D3(w * glm::dvec4(m->position(m->edgePoint(e, 0)), 1.0)), pb = D3(w * glm::dvec4(m->position(m->edgePoint(e, 1)), 1.0));
            auto a = view.project(pa), b = view.project(pb);
            if (!a || !b)
                continue;
            auto [d, t] = toSegment(px, *a, *b);
            if (d > maxPx || (best && d >= best->px))
                continue;
            best = BorderHover{id, n->path(), e, t, glm::mix(pa, pb, t), d};
        }
    }
    return best;
}

std::optional<geo::Mesh> worldMesh(scene::NodeId mesh)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.node(mesh);
    if (!n || !n->type().isA("mesh"))
        return std::nullopt;
    scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!m)
        return std::nullopt;
    geo::Mesh out = *m;
    glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    for (D3& p : out.positions())
        p = D3(w * glm::dvec4(p, 1.0));
    return out;
}

void drawPolyline(OverlayPainter& p, const ViewInfo& view, const std::vector<D3>& pts, bool closed, float width, const glm::vec4& color)
{
    std::vector<QPointF> run;
    auto flush = [&] {
        if (run.size() >= 2)
            p.polyline(run, width, color, false);
        run.clear();
    };
    for (const D3& q : pts)
    {
        if (auto s = view.project(q))
            run.push_back(*s);
        else
            flush();
    }
    if (closed && !pts.empty())
        if (auto s = view.project(pts.front()))
            run.push_back(*s);
    flush();
}

D3 viewAxis(const ViewInfo& view) { return glm::normalize(D3(glm::inverse(view.view)[2])); }
} // namespace lila::ui::curvePick
