#include "ui/CurveUi.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/DrawOverrides.h"
#include "ui/SketchUi.h"
#include "ui/Theme.h"

#include <cmath>
#include <map>
#include <set>

namespace lila::ui
{
namespace
{
using D3 = glm::dvec3;
using curve::CurveShape;

// Linear for 3D lines; sRGB for the 2D overlay.
const glm::vec4 kCurve(0.392f, 0.392f, 0.392f, 1.0f); // Monotone's "curve" token
const glm::vec4 kWireDefault(0.02f, 0.02f, 0.02f, 1.0f);
const glm::vec4 kHalo(0.008f, 0.008f, 0.008f, 1.0f);
const glm::vec4 kHull(0.45f, 0.45f, 0.45f, 1.0f);
const glm::vec4 kComb(0.35f, 0.35f, 0.35f, 1.0f);
const glm::vec4 kPointRim(0.05f, 0.05f, 0.05f, 1.0f);
const glm::vec4 kPoint(0.62f, 0.62f, 0.62f, 1.0f);
const glm::vec4 kPointSelected(1.0f, 1.0f, 1.0f, 1.0f);

/** Tessellations by node, reused while the node's splines and display settings stay the same. */
struct Cached
{
    const void* data = nullptr;
    int64_t mode = -1, segments = -1;
    std::vector<curve::Polyline> lines;
};

std::map<scene::NodeId, Cached>& cache()
{
    static std::map<scene::NodeId, Cached> c;
    return c;
}

const std::vector<curve::Polyline>& linesOf(const scene::Node& n)
{
    const scene::PlugValue& v = n.value(CurveShape::kLocal);
    const scene::DataPtr* d = std::get_if<scene::DataPtr>(&v);
    Cached& c = cache()[n.id()];
    int64_t mode = n.as<int64_t>(CurveShape::kTessMode), seg = n.as<int64_t>(CurveShape::kTessSegments);
    const void* key = d ? d->get() : nullptr;
    if (c.data == key && c.mode == mode && c.segments == seg)
        return c.lines;
    c.data = key;
    c.mode = mode;
    c.segments = seg;
    c.lines.clear();
    curve::TessOptions o;
    o.mode = curve::TessMode(std::clamp<int64_t>(mode, 0, 2));
    o.perSpan = int(std::max<int64_t>(1, seg));
    o.chordError = 0.01;
    o.maxAngle = 0.07;
    for (const curve::Spline& s : curve::splinesOf(v))
        c.lines.push_back(curve::tessellate(s, o));
    return c.lines;
}

/** The node is being edited: vertex mode with it selected or holding selected points. */
bool editing(const scene::Node& n)
{
    if (scene::components::mode() != scene::ComponentMode::Vertex)
        return false;
    scene::Graph& g = scene::Graph::current();
    return g.isSelected(n.id()) || !scene::components::selected(n.id(), scene::ComponentMode::Vertex).empty();
}

D3 xf(const glm::dmat4& m, const D3& p) { return D3(m * glm::dvec4(p, 1.0)); }

void line(render::FrameState& f, const D3& a, const D3& b, const glm::vec4& c, bool onTop)
{
    auto& out = onTop ? f.linesOnTop : f.lines;
    out.push_back({glm::vec3(a), c});
    out.push_back({glm::vec3(b), c});
}

/** The curve's own line colour: the theme's curve colour unless a highlight or display layer gives another. */
glm::vec4 curveColor(const drawOverrides::DrawContext& c)
{
    if (c.highlight == drawOverrides::Highlight::None && c.color == themeLinear("wireframe", kWireDefault))
        return themeLinear("curve", kCurve);
    return c.color;
}

void drawCurve(drawOverrides::DrawContext& c)
{
    LILA_ZONE();

    const scene::Node& n = c.node;
    const curve::Splines& splines = curve::splinesOf(n.value(CurveShape::kLocal));
    bool edit = editing(n);
    using HL = drawOverrides::Highlight;
    bool bold = c.highlight == HL::Selected || c.highlight == HL::Preselected;
    auto& halo = bold ? c.frame.curveHaloBold : c.frame.curveHalo;
    auto& body = bold ? c.frame.curveLinesBold : c.frame.curveLines;
    const glm::vec4 color = curveColor(c);
    // Fully constrained sketch geometry takes its own colour unless a highlight or layer colour says otherwise.
    const sketchUi::Display* sketch = color == themeLinear("curve", kCurve) ? sketchUi::display(n) : nullptr;
    const glm::vec4 fixedColor = sketchUi::constrainedColor();
    const std::vector<curve::Polyline>& lines = linesOf(n);
    for (size_t li = 0; li < lines.size(); ++li)
    {
        const curve::Polyline& pl = lines[li];
        size_t segs = pl.closed ? pl.size() : (pl.size() ? pl.size() - 1 : 0);
        for (size_t i = 0; i < segs; ++i)
        {
            glm::vec3 a(xf(c.world, pl.points[i])), b(xf(c.world, pl.points[(i + 1) % pl.size()]));
            const glm::vec4& col = sketch && sketchUi::spanFixed(*sketch, splines, li, pl.params[i]) ? fixedColor : color;
            halo.push_back({a, kHalo});
            halo.push_back({b, kHalo});
            body.push_back({a, col});
            body.push_back({b, col});
        }
    }
    if (n.as<bool>(CurveShape::kDisplayComb))
    {
        double scale = n.as<double>(CurveShape::kCombScale);
        int samples = int(std::max<int64_t>(1, n.as<int64_t>(CurveShape::kCombSamples)));
        for (const curve::Spline& s : splines)
        {
            std::vector<curve::CombTooth> teeth = curve::curvatureComb(s, samples, scale);
            for (size_t i = 0; i < teeth.size(); ++i)
            {
                line(c.frame, xf(c.world, teeth[i].base), xf(c.world, teeth[i].tip), kComb, false);
                if (i + 1 < teeth.size())
                    line(c.frame, xf(c.world, teeth[i].tip), xf(c.world, teeth[i + 1].tip), kComb, false);
            }
        }
    }
    if (!edit)
        return;
    const auto& sel = scene::components::selected(n.id(), scene::ComponentMode::Vertex);
    std::set<uint32_t> picked(sel.begin(), sel.end());
    bool hull = n.as<bool>(CurveShape::kDisplayHull), points = n.as<bool>(CurveShape::kDisplayPoints);
    for (size_t si = 0; si < splines.size(); ++si)
    {
        const curve::Spline& s = splines[si];
        if (hull && s.kind == curve::Kind::Nurbs)
            for (size_t i = 0; i + 1 < s.size() + (s.closed ? 1 : 0); ++i)
                line(c.frame, xf(c.world, s.points[i]), xf(c.world, s.points[(i + 1) % s.size()]), kHull, true);
        if (hull && s.kind == curve::Kind::Bezier && s.handleL.size() == s.size())
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s.closed || i > 0)
                    line(c.frame, xf(c.world, s.points[i]), xf(c.world, s.handleL[i]), kHull, true);
                if (s.closed || i + 1 < s.size())
                    line(c.frame, xf(c.world, s.points[i]), xf(c.world, s.handleR[i]), kHull, true);
            }
    }
    if (!points)
        return;
    for (auto& [id, p] : curve::componentPositions(splines))
    {
        size_t si, pi;
        curve::PointRole role;
        curve::decodeComponent(id, &si, &pi, &role);
        const curve::Spline& s = splines[si];
        // Open ends' outer handles shape nothing, so they aren't offered.
        if ((role == curve::PointRole::HandleL && !s.closed && pi == 0) || (role == curve::PointRole::HandleR && !s.closed && pi + 1 == s.size()))
            continue;
        auto q = c.view.project(xf(c.world, p));
        if (!q)
            continue;
        bool on = picked.contains(id);
        const glm::vec4& fill = on ? kPointSelected : kPoint;
        switch (role)
        {
        case curve::PointRole::Point:
            c.painter.disc(*q, on ? 5.0f : 4.5f, kPointRim, 16);
            c.painter.disc(*q, on ? 3.5f : 3.0f, fill, 16);
            break;
        case curve::PointRole::HandleL:
        case curve::PointRole::HandleR:
            c.painter.rect(QRectF(*q - QPointF(3.5, 3.5), QSizeF(7, 7)), kPointRim);
            c.painter.rect(QRectF(*q - QPointF(2.0, 2.0), QSizeF(4, 4)), fill);
            break;
        case curve::PointRole::Edit:
            c.painter.line(*q - QPointF(4.5, 4.5), *q + QPointF(4.5, 4.5), on ? 4.5f : 3.5f, kPointRim);
            c.painter.line(*q - QPointF(4.5, -4.5), *q + QPointF(4.5, -4.5), on ? 4.5f : 3.5f, kPointRim);
            c.painter.line(*q - QPointF(4, 4), *q + QPointF(4, 4), on ? 2.5f : 1.5f, fill);
            c.painter.line(*q - QPointF(4, -4), *q + QPointF(4, -4), on ? 2.5f : 1.5f, fill);
            break;
        }
    }
}

std::optional<double> pickCurve(const drawOverrides::PickContext& c)
{
    QRectF box = QRectF(c.a, c.b).normalized();
    std::optional<double> best;
    for (const curve::Polyline& pl : linesOf(c.node))
    {
        size_t segs = pl.closed ? pl.size() : (pl.size() ? pl.size() - 1 : 0);
        for (size_t i = 0; i < segs; ++i)
        {
            auto p0 = c.view.project(xf(c.world, pl.points[i])), p1 = c.view.project(xf(c.world, pl.points[(i + 1) % pl.size()]));
            if (!p0 || !p1)
                continue;
            if (!c.click)
            {
                if (box.contains(*p0) || box.contains(*p1))
                    return 0.0;
                continue;
            }
            QPointF d = *p1 - *p0;
            double len2 = QPointF::dotProduct(d, d);
            double t = len2 > 0 ? std::clamp(QPointF::dotProduct(c.a - *p0, d) / len2, 0.0, 1.0) : 0.0;
            QPointF q = *p0 + d * t - c.a;
            double dist = std::hypot(q.x(), q.y());
            if (!best || dist < *best)
                best = dist;
        }
    }
    return best;
}

void runCommand(const char* name, const json& call)
{
    try
    {
        ops::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void cmd(std::string id, std::string label, const char* command, json call, std::string doc)
{
    std::string py = ops::toPython(command, call);
    actions::add({.id = std::move(id), .label = std::move(label), .category = "Curves", .doc = std::move(doc), .python = py, .command = command,
                  .call = std::move(call)});
}

void registerActions()
{
    cmd("curve.circle", "Circle", "circle", json::object(), "Creates an exact circle (a rational NURBS).");
    cmd("curve.fromEdges", "Edges to Curve", "polyEdgeToCurve", json::object(),
        "Makes a curve from each chain of the selected mesh edges, or the borders of the selected faces.");
    cmd("curve.extrude", "Sweep", "extrude", json::object(),
        "Sweeps a circle, rectangle or a second selected curve along the first selected curve into quads, live.");
    cmd("curve.revolve", "Revolve", "revolve", json::object(), "Revolves the selected profile curve around an axis into quads, live.");
    cmd("curve.loft", "Loft", "loft", json::object(), "Lofts quads through the selected curves in selection order, live.");
    cmd("curve.patchFill", "Patch Fill", "patchFill", json::object(), "Fills four selected boundary curves with a quad patch, live.");
    cmd("curve.fillet", "Fillet Corners", "filletCurve", json::object(), "Rounds, chamfers or G2-blends the selected curves' corners.");
    cmd("curve.offset", "Offset", "offsetCurve", json::object(), "Offsets the selected curves within their plane into new curves.");
    cmd("curve.rebuild", "Rebuild", "rebuildCurve", json::object(), "Rebuilds the selected curves with even spans, corners kept.");
    cmd("curve.fit", "Fit", "fitCurve", json::object(), "Refits the selected curves as Beziers with as few points as the tolerance allows.");
    actions::add({.id = "curve.project",
                  .label = "Project onto Mesh",
                  .category = "Curves",
                  .run =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          json curves = json::array();
                          std::string mesh;
                          for (scene::NodeId id : g.selection())
                              if (scene::Node* n = g.node(id))
                              {
                                  if (n->type().isA("curve"))
                                      curves.push_back(n->path());
                                  else if (n->type().isA("mesh"))
                                      mesh = n->path();
                              }
                          if (curves.empty() || mesh.empty())
                          {
                              log::error("Project onto Mesh: select curves and the mesh to project onto");
                              return;
                          }
                          actions::runWithOptions("projectCurve", {{"args", curves}, {"mesh", mesh}});
                      },
                  .doc = "Projects the selected curves onto the selected mesh (closest point, or along the direction in the option box), live.",
                  .python = "lila.ops.projectCurve(\"Curve\", mesh=\"Mesh\")",
                  .command = "projectCurve"});
    cmd("curve.keepOnSurface", "Keep on Surface", "conformCurve", json::object(),
        "Keeps the selected curves on the selected mesh: their points stay on it however they or the mesh move (Make Live).");
    cmd("curve.releaseFromSurface", "Release from Surface", "conformCurve", {{"release", true}},
        "Lets the selected curves off the surface they were kept on, leaving them where they are.");
    cmd("curve.sendToUnreal", "Send Curve to Unreal", "unrealSendCurve", json::object(),
        "Sends the selected curves to the connected Unreal editor as spline actors (or landscape splines), resent on every save.");
    cmd("curve.attach", "Attach", "attachCurve", json::object(), "Joins the second selected curve onto the first at their nearest ends.");
    cmd("curve.detach", "Detach", "detachCurve", json::object(), "Splits the curve at the selected points.");
    cmd("curve.reverse", "Reverse Direction", "reverseCurve", json::object(), "Reverses the selected curves' direction.");
    cmd("curve.close", "Close", "closeCurve", json::object(), "Closes the selected open curves.");
    cmd("curve.open", "Open", "openCurve", json::object(), "Opens the selected closed curves at their start.");
    cmd("curve.deletePoints", "Delete Points", "deleteCurvePoints", json::object(), "Deletes the selected curve points.");
    for (const char* t : {"auto", "vector", "aligned", "free"})
    {
        std::string label = std::string(1, char(std::toupper(t[0]))) + (t + 1);
        cmd(std::string("curve.handle") + label, label + " Handles", "setCurveHandleType", {{"type", t}},
            "Sets the selected Bezier points' handles to " + std::string(t) + ".");
    }
    cmd("curve.bend", "Bend", "bendCurve", json::object(), "Bends the selected curves (or selected points) around an axis by an angle.");
    cmd("curve.curl", "Curl", "curlCurve", json::object(), "Coils the selected curves around themselves.");
    cmd("curve.scaleCurvature", "Scale Curvature", "scaleCurvature", json::object(),
        "Flattens (below 1) or exaggerates (above 1) the selected curves' bends, keeping their length.");
    cmd("curve.smooth", "Smooth", "smoothCurve", json::object(), "Smooths the selected curves, or only the selected points.");
    cmd("curve.evenSpacing", "Even Spacing", "rebuildCurve", {{"keepPointCount", true}},
        "Spreads the selected curves' points evenly along them, keeping their kind, point count and corners.");
    cmd("curve.deformAlongCurve", "Deform Along Curve", "deformAlongCurve", json::object(),
        "Bends the selected meshes along the selected curve with a live modifier.");
    cmd("curve.arrayAlongCurve", "Array Along Curve", "arrayAlongCurve", json::object(),
        "Repeats the selected meshes along the selected curve with a live modifier.");
    cmd("curve.instanceAlongCurve", "Instance Along Curve", "arrayAlongCurve", {{"output", "instances"}},
        "Places instances of the selected meshes along the selected curve, under a group.");
    cmd("curve.flowAlongSurface", "Flow Along Surface", "flowAlongSurface", json::object(),
        "Flows the selected meshes onto the surface selected last (its UVs, or its selected quad patch), live.");
    cmd("curve.toBezier", "Convert to Bezier", "convertCurve", {{"type", "bezier"}}, "Converts the selected curves to Beziers.");
    cmd("curve.toNurbs", "Convert to NURBS", "convertCurve", {{"type", "nurbs"}}, "Converts the selected curves to NURBS (exact).");
    cmd("curve.toPoly", "Convert to Poly", "convertCurve", {{"type", "poly"}}, "Converts the selected curves to poly lines.");
    actions::add({.id = "curve.toggleComb",
                  .label = "Curvature Comb",
                  .category = "Curves",
                  .run =
                      [] {
                          scene::Graph& g = scene::Graph::current();
                          for (scene::NodeId id : g.selection())
                              if (scene::Node* n = g.node(id); n && n->type().isA("curve"))
                                  runCommand("setAttr", {{"args", {n->path() + ".displayComb", !n->as<bool>(CurveShape::kDisplayComb)}}});
                      },
                  .doc = "Shows or hides the selected curves' curvature combs.",
                  .python = "lila.ops.setAttr(\"Curve.displayComb\", True)"});
}
} // namespace

void registerCurveUi()
{
    if (!drawOverrides::registry().find("lila.curve"))
        drawOverrides::add({.id = "lila.curve", .nodeType = "curve", .draw = drawCurve, .pick = pickCurve});
    registerActions();
    registerCurveTools();
    registerPipeTool();
    registerCableTool();
    registerSectionTool();
    registerVehicleCurveUi();
    registerSketchTool();
    registerCurveSculptTool();
    registerPlanarCurveUi();
}
} // namespace lila::ui
