#include "ui/CurveUi.h"
#include "ui/CurveToolsCommon.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/Cable.h"
#include "curve/CurveNodes.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/StepHistory.h"
#include "ui/ViewportTool.h"

#include <algorithm>

namespace lila::ui
{
namespace
{
using namespace curveTools;

/** A placed anchor and the mesh it landed on (none on the construction plane). */
struct Anchor
{
    D3 point;
    scene::NodeId surface = scene::kNoNode;
};

/**
 * Cable: click anchor points (on surfaces by default) and see the cables hang between them as you go. B-drag or Ctrl+scroll
 * sets the slack (or the sag depth, M switches), +/- or Shift+scroll the cables per span, R reseeds a bundle, D drapes them
 * over the meshes the anchors are on, P adds pipe geometry. Enter builds them with live history; with nothing placed, Enter
 * hangs cables from the selected curves' points. Clicking the first anchor again closes the loop.
 */
class CableTool final : public ViewportTool
{
public:
    const char* name() const override { return "Cable"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }
    bool ownsBrushRadius() const override { return true; }

    void activate() override
    {
        mDepth = prefB("depthMode", false);
        mSlack = prefD("slack", 0.1);
        mSag = prefD("sag", 20.0);
        mCount = int(prefD("count", 1.0));
        mSeed = int(prefD("seed", 1.0));
        mDrape = prefB("drape", false);
        mPipe = prefB("pipe", true);
        mPipeRadius = prefD("pipeRadius", 0.6);
        mSurface = prefB("surface", true);
        mPlacer.onSurface(mSurface, 0.0);
        log::info("Cable: click anchor points, B-drag or Ctrl+scroll sets the slack, +/- cables per span, Enter builds them");
    }

    void deactivate() override { viewportTools::requestRedraw(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (tools::brushRadiusDrag(e, false))
        {
            mAmountDrag = true;
            mPressPx = e->position();
            mStartAmount = amount();
            return true;
        }
        if (e->button() != Qt::LeftButton)
            return false;
        if (mAnchors.size() >= 3)
            if (auto first = view.project(mAnchors.front().point); first && distPx(*first, e->position()) < kClosePx)
            {
                finish(true);
                return true;
            }
        Anchor a;
        if (auto p = mPlacer.place(e->position(), view, nullptr, &a.surface))
        {
            a.point = *p;
            if (mAnchors.empty())
                mPlacer.lockTo(*p);
            mAnchors.push_back(a);
            mHistory.clear();
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHoverView = view.camera;
        if (mAmountDrag)
        {
            setAmount(mStartAmount * std::exp((e->position().x() - mPressPx.x()) * 0.01));
            return true;
        }
        mHover = mPlacer.place(e->position(), view);
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mAmountDrag && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton))
        {
            mAmountDrag = false;
            return true;
        }
        return e->button() == Qt::LeftButton;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        double steps = e->angleDelta().y() / 120.0;
        if (e->modifiers() & Qt::ControlModifier)
        {
            setAmount(amount() * std::pow(1.1, steps));
            return true;
        }
        if (e->modifiers() & Qt::ShiftModifier)
        {
            setCount(mCount + (steps > 0 ? 1 : -1));
            return true;
        }
        return false;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter: finish(false); return true;
        case Qt::Key_Plus:
        case Qt::Key_Equal: setCount(mCount + 1); return true;
        case Qt::Key_Minus:
        case Qt::Key_Underscore: setCount(mCount - 1); return true;
        case Qt::Key_M:
            mDepth = !mDepth;
            setPref("depthMode", mDepth);
            break;
        case Qt::Key_R:
            mSeed = mSeed % 9999 + 1;
            setPref("seed", mSeed);
            break;
        case Qt::Key_D:
            mDrape = !mDrape;
            setPref("drape", mDrape);
            break;
        case Qt::Key_P:
            mPipe = !mPipe;
            setPref("pipe", mPipe);
            break;
        case Qt::Key_BracketLeft:
        case Qt::Key_BracketRight:
            mPipeRadius = std::clamp(mPipeRadius * (e->key() == Qt::Key_BracketRight ? 1.25 : 0.8), 0.01, 1e4);
            setPref("pipeRadius", mPipeRadius);
            break;
        case Qt::Key_S:
            mSurface = !mSurface;
            setPref("surface", mSurface);
            mPlacer.onSurface(mSurface, 0.0);
            break;
        case Qt::Key_L: mPlacer.toggleLock(!mPlacer.lockEnabled()); break;
        case Qt::Key_Backspace: stepBack(); break;
        case Qt::Key_Escape:
            if (mAnchors.empty())
                exitTool();
            restart();
            break;
        default: return false;
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view action) override
    {
        switch (mHistory.route(e.key(), action))
        {
        case StepKey::Undo: stepBack(); break;
        case StepKey::Redo:
            mHistory.redo(mAnchors);
            relock();
            break;
        case StepKey::None:
            // The tool's letters (S, L, D, P) are app hotkeys too; it takes them while active.
            return plainKey(e) && keyPress(const_cast<QKeyEvent*>(&e), ViewInfo{});
        }
        viewportTools::requestRedraw();
        return true;
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        std::vector<D3> pts;
        for (const Anchor& a : mAnchors)
            pts.push_back(a.point);
        if (mHover && view.camera == mHoverView)
            pts.push_back(*mHover);
        std::string warning;
        if (pts.size() >= 2)
        {
            curve::CableResult r = curve::makeCables(pts, false, options());
            for (const curve::Spline& s : r.cables)
                drawSpline(f, s, kPreview);
            if (!r.warnings.empty())
                warning = r.warnings.front();
        }
        for (const Anchor& a : mAnchors)
            dot(p, view, a.point);
        if (mHover && view.camera == mHoverView)
            dot(p, view, *mHover, 3.0f, kGhost);
        ToolStatus status{.tool = "Cable"};
        status.fields.push_back(mDepth ? ToolField{"sag", QString("%1 cm").arg(mSag, 0, 'f', 1)}
                                       : ToolField{"slack", QString("%1%").arg(mSlack * 100.0, 0, 'f', 1)});
        status.fields.push_back({"per span", QString::number(mCount)});
        if (mCount > 1)
            status.fields.push_back({"seed", QString::number(mSeed)});
        status.fields.push_back({"pipe radius", QString("%1 cm").arg(mPipeRadius, 0, 'f', 2)});
        status.flags = {{"drape", mDrape}, {"pipe", mPipe}, {"surfaces", mSurface}};
        status.keys = {{"B-drag", mDepth ? "sag" : "slack"}, {"M", "slack/sag"}, {"+/-", "cables"}, {"R", "reseed"}, {"D", "drape"},
                       {"P", "pipe"}, {"[ ]", "pipe radius"}, {"S", "surfaces"}, {"Z", "undo point"}, {"First point", "close"},
                       {"Enter", "build or hang from selected"}, {"Esc", "exit"}};
        drawToolStatus(p, view, status);
        if (!warning.empty())
            drawToolMessage(p, view, QString::fromStdString("Cable: " + warning));
    }

private:
    static constexpr const char* kPrefs = "/cableTool/";
    static double prefD(const char* key, double def) { return prefs().value<double>("general", std::string(kPrefs) + key, def); }
    static bool prefB(const char* key, bool def) { return prefs().value<bool>("general", std::string(kPrefs) + key, def); }
    static void setPref(const char* key, const json& v) { prefs().set("general", std::string(kPrefs) + key, v); }

    double amount() const { return mDepth ? mSag : std::max(mSlack, 1e-3); }

    void setAmount(double v)
    {
        if (mDepth)
        {
            mSag = std::clamp(v, 0.01, 1e5);
            setPref("sag", mSag);
        }
        else
        {
            mSlack = std::clamp(v, 0.001, 10.0);
            setPref("slack", mSlack);
        }
        viewportTools::requestRedraw();
    }

    void setCount(int n)
    {
        mCount = std::clamp(n, 1, 64);
        setPref("count", mCount);
        viewportTools::requestRedraw();
    }

    /** The meshes the anchors landed on, which a draped cable settles onto (at most the four a cable node takes). */
    std::vector<scene::NodeId> surfaces() const
    {
        std::vector<scene::NodeId> out;
        for (const Anchor& a : mAnchors)
            if (a.surface != scene::kNoNode && std::ranges::find(out, a.surface) == out.end() && out.size() < 4)
                out.push_back(a.surface);
        return out;
    }

    curve::CableOptions options() const
    {
        curve::CableOptions o;
        o.mode = mDepth ? curve::SagMode::Depth : curve::SagMode::Slack;
        o.slack = mSlack;
        o.sag = mSag;
        o.gravity = -upAxis();
        o.count = mCount;
        o.seed = uint32_t(mSeed);
        o.clearance = mPipe ? mPipeRadius : 0.0;
        o.drape = mDrape;
        if (mDrape)
            if (std::vector<scene::NodeId> ids = surfaces(); !ids.empty())
                o.closest = [ids](const D3& p) -> std::optional<curve::SurfacePoint> {
                    auto hit = surfaceSnap::closestAmong(p, ids);
                    if (!hit)
                        return std::nullopt;
                    return curve::SurfacePoint{hit->position, hit->normal};
                };
        return o;
    }

    void finish(bool closed)
    {
        json call = {{"sagMode", mDepth ? "depth" : "slack"}, {"slack", mSlack}, {"sag", mSag}, {"count", mCount}, {"seed", mSeed},
                     {"drape", mDrape},  {"clearance", mPipe ? mPipeRadius : 0.0}, {"pipe", mPipe}, {"pipeRadius", mPipeRadius}};
        if (mAnchors.empty())
        {
            actions::runWithOptions("cable", call);
            restart();
            return;
        }
        if (mAnchors.size() < 2)
        {
            log::warn("Cable: place at least two anchor points");
            return;
        }
        json pts = json::array();
        for (const Anchor& a : mAnchors)
            pts.push_back({a.point.x, a.point.y, a.point.z});
        json colliders = json::array();
        for (scene::NodeId id : surfaces())
            if (const scene::Node* n = scene::Graph::current().node(id))
                colliders.push_back(n->path());
        call["points"] = pts;
        call["closed"] = closed;
        call["colliders"] = colliders;
        actions::runWithOptions("cable", call);
        restart();
    }

    void stepBack()
    {
        mHistory.undo(mAnchors);
        relock();
    }

    void relock()
    {
        if (mAnchors.empty())
            mPlacer.unlock();
        else
            mPlacer.lockTo(mAnchors.front().point);
    }

    void restart()
    {
        mAnchors.clear();
        mHistory.clear();
        mPlacer.unlock();
        viewportTools::requestRedraw();
    }

    std::vector<Anchor> mAnchors;
    StepHistory<Anchor> mHistory;
    std::optional<D3> mHover;
    std::string mHoverView;
    Placer mPlacer;
    bool mAmountDrag = false;
    QPointF mPressPx;
    double mStartAmount = 0.1;
    bool mDepth = false, mDrape = false, mPipe = true, mSurface = true;
    double mSlack = 0.1, mSag = 20.0, mPipeRadius = 0.6;
    int mCount = 1, mSeed = 1;
};
} // namespace

void registerCableTool()
{
    if (!viewportTools::registry().find("curve.cableTool"))
        viewportTools::add({.id = "curve.cableTool", .label = "Cable Tool", .icon = "line-dashed", .create = [] { return std::make_unique<CableTool>(); }});
    actions::add({.id = "curve.cableTool",
                  .label = "Cable Tool",
                  .category = "Curves",
                  .run = [] { viewportTools::activate("curve.cableTool"); },
                  .doc = "Hangs cables between points you click on surfaces: catenary sag by slack or depth, bundles, drape and pipe "
                         "geometry, kept live. B-drag or Ctrl+scroll sets the slack; Enter builds them.",
                  .python = "lila.ui.run_action(\"curve.cableTool\")"});
    actions::add({.id = "curve.cable",
                  .label = "Cables from Curves",
                  .category = "Curves",
                  .doc = "Hangs live catenary cables between the points of the selected curves; selected meshes become what they drape over.",
                  .python = ops::toPython("cable", json::object()),
                  .command = "cable",
                  .call = json::object()});
    actions::add({.id = "curve.stitch",
                  .label = "Stitch",
                  .category = "Curves",
                  .doc = "Sews live thread along the selected curves: running, saddle, zigzag, cross or blanket stitches; a selected mesh is what they sit on.",
                  .python = ops::toPython("stitch", json::object()),
                  .command = "stitch",
                  .call = json::object()});
}
} // namespace lila::ui
