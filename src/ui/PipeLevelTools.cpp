#include "ui/CurveUi.h"
#include "ui/CurveToolsCommon.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/LevelToolkit.h"
#include "ui/ViewportTool.h"

#include <algorithm>

namespace lila::ui
{
namespace
{
using namespace curveTools;

constexpr const char* kRoutePrefs = "/pipeRouteTool/";

double routePref(const char* key, double def) { return prefs().value<double>("general", std::string(kRoutePrefs) + key, def); }
void setRoutePref(const char* key, const json& v) { prefs().set("general", std::string(kRoutePrefs) + key, v); }

/** An end of the run: where it was clicked, the surface's normal there (zero off surfaces) and the mesh. */
struct RouteEnd
{
    D3 point{0.0};
    D3 normal{0.0};
    scene::NodeId surface = scene::kNoNode;
};

/**
 * Pipe Route: click where the pipe starts and where it ends, on surfaces. It leaves the first surface along its normal
 * and arrives square onto the second, routed around the selected meshes and the clicked ones, and builds a live pipe on
 * the run. [ and ] set the radius; Enter with one end placed does nothing, Esc starts again.
 */
class PipeRouteTool final : public ViewportTool
{
public:
    const char* name() const override { return "Pipe Route"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    void activate() override
    {
        mRadius = routePref("radius", 5.0);
        mPlacer.onSurface(true, 0.0);
        log::info("Pipe Route: click the start and the end; the run goes round the selected meshes");
    }

    void deactivate() override { viewportTools::requestRedraw(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        RouteEnd end;
        if (auto p = mPlacer.place(e->position(), view, &end.normal, &end.surface))
        {
            end.point = *p;
            mEnds.push_back(end);
            if (mEnds.size() == 2)
                build();
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHoverView = view.camera;
        mHover = mPlacer.place(e->position(), view);
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override { return e->button() == Qt::LeftButton; }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_BracketLeft:
        case Qt::Key_BracketRight:
            mRadius = std::clamp(mRadius * (e->key() == Qt::Key_BracketRight ? 1.25 : 0.8), 0.05, 1e4);
            setRoutePref("radius", mRadius);
            break;
        case Qt::Key_Backspace:
            if (!mEnds.empty())
                mEnds.pop_back();
            break;
        case Qt::Key_Escape:
            if (mEnds.empty())
                exitTool();
            mEnds.clear();
            break;
        default: return false;
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override { return plainKey(e) && keyPress(const_cast<QKeyEvent*>(&e), ViewInfo{}); }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        if (!mEnds.empty() && mHover && view.camera == mHoverView)
        {
            curve::Spline s;
            s.kind = curve::Kind::Poly;
            s.points = {mEnds.front().point, *mHover};
            s.normalize();
            drawSpline(f, s, kGhost);
        }
        for (const RouteEnd& e : mEnds)
            dot(p, view, e.point);
        if (mHover && view.camera == mHoverView)
            dot(p, view, *mHover, 3.0f, kGhost);
        drawToolStatus(p, view,
                       {.tool = "Pipe Route",
                        .fields = {{"radius", QString("%1 cm").arg(mRadius, 0, 'f', 2)}},
                        .prompt = mEnds.empty() ? QString("click the start, then the end") : QString("click the end"),
                        .keys = {{"[ ]", "radius"}, {"Backspace", "undo point"}, {"Esc", "exit"}}});
    }

private:
    /** The axis the run takes off a surface: its normal snapped to the nearest axis, or none off surfaces. */
    static json axisOf(const D3& n)
    {
        if (glm::length(n) < 1e-9)
            return json::array({0, 0, 0});
        int a = 0;
        for (int k = 1; k < 3; ++k)
            if (std::abs(n[k]) > std::abs(n[a]))
                a = k;
        D3 v(0.0);
        v[a] = n[a] < 0.0 ? -1.0 : 1.0;
        return json::array({v.x, v.y, v.z});
    }

    void build()
    {
        scene::Graph& g = scene::Graph::current();
        json obstacles = json::array();
        auto addObstacle = [&](scene::NodeId id) {
            if (const scene::Node* n = g.node(id); n && n->type().isA("mesh") && std::ranges::find(obstacles, json(n->path())) == obstacles.end())
                obstacles.push_back(n->path());
        };
        for (scene::NodeId id : g.selection())
            addObstacle(id);
        for (const RouteEnd& e : mEnds)
            if (e.surface != scene::kNoNode)
                addObstacle(e.surface);
        const RouteEnd &a = mEnds[0], &b = mEnds[1];
        json call = {{"start", {a.point.x, a.point.y, a.point.z}},
                     {"end", {b.point.x, b.point.y, b.point.z}},
                     {"startDir", axisOf(a.normal)},
                     {"endDir", axisOf(-b.normal)},
                     {"obstacles", obstacles},
                     {"pipe", true},
                     {"pipeOptions", {{"radius", mRadius}}}};
        actions::runWithOptions("pipeRoute", call);
        mEnds.clear();
    }

    std::vector<RouteEnd> mEnds;
    std::optional<D3> mHover;
    std::string mHoverView;
    Placer mPlacer;
    double mRadius = 5.0;
};
} // namespace

void registerPipeLevelTools()
{
    if (!viewportTools::registry().find("curve.pipeRouteTool"))
        viewportTools::add({.id = "curve.pipeRouteTool", .label = "Pipe Route", .icon = "route-alt-left", .create = [] { return std::make_unique<PipeRouteTool>(); }});
    actions::add({.id = "curve.pipeRouteTool",
                  .label = "Pipe Route Tool",
                  .category = "Curves",
                  .run = [] { viewportTools::activate("curve.pipeRouteTool"); },
                  .doc = "Routes a live pipe between two points you click on surfaces: square runs round the selected meshes, leaving "
                         "and arriving along the surfaces' normals. [ and ] set the radius.",
                  .python = "lila.ui.run_action(\"curve.pipeRouteTool\")"});
    actions::add({.id = "curve.pipeSupports",
                  .label = "Pipe Supports",
                  .category = "Curves",
                  .doc = "Places hangers, brackets or stands along the selected pipes, reaching to the other selected meshes (walls, "
                         "floors, ceilings). They follow the pipe live.",
                  .python = ops::toPython("pipeSupports", json::object()),
                  .command = "pipeSupports",
                  .call = json::object()});
    actions::add({.id = "curve.carveChannel",
                  .label = "Carve Channel",
                  .category = "Curves",
                  .doc = "Carves a channel along the selected curves into the selected mesh: U channel, V groove, half round or stepped, "
                         "kept live as a modifier on the mesh.",
                  .python = ops::toPython("carveChannel", json::object()),
                  .command = "carveChannel",
                  .call = json::object()});
    levelToolkit::add({.id = "curves.pipes",
                       .title = "Pipes and Channels",
                       .items = {{"curve.pipeTool", "route-2", "Pipe", "Pipe"},
                                 {"curve.pipeRouteTool", "route-alt-left", "Route", "Route"},
                                 {"curve.pipeSupports", "building-bridge", "Supports"},
                                 {"curve.cableTool", "plug", "Cable"},
                                 {"curve.carveChannel", "shovel", "Channel", "Channel"}},
                       .order = 60,
                       .docs = "curves?id=pipes-and-hoses"});
}
} // namespace lila::ui
