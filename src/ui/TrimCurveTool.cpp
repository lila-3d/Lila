#include "ui/CurveUi.h"
#include "ui/CurvePick.h"
#include "ui/CurveToolsCommon.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/Intersect.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"

namespace lila::ui
{
namespace
{
using namespace curveTools;
using curvePick::SceneCurve;

const glm::vec4 kTrimCut(1.0f, 0.42f, 0.28f, 1.0f);
const glm::vec4 kGrow(0.45f, 0.85f, 1.0f, 1.0f);

/**
 * Trim: hover a curve and the stretch between the intersections around the cursor lights up; click deletes it. E switches
 * to extend: hover near an end and the extension to the next curve it meets shows; click makes it (M cycles smooth, line
 * and arc). P intersects as seen in the view, so curves that only cross on screen count. Every click is its own undo step.
 */
class TrimCurveTool final : public ViewportTool
{
public:
    const char* name() const override { return "Trim Curves"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    void activate() override
    {
        mProjected = prefs().value<bool>("general", "/curveTrim/projected", false);
        mExtendMode = curve::ExtendMode(std::clamp(prefs().value<int>("general", "/curveTrim/extendMode", 2), 0, 2));
        mVersion = ~uint64_t(0);
        log::info("Trim Curves: click a highlighted stretch to delete it, E: extend instead, P: intersect as seen, Esc: exit");
    }

    void deactivate() override { viewportTools::requestRedraw(); }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        refresh(view);
        mHoverView = view.camera;
        hover(e->position(), view);
        viewportTools::requestRedraw();
        return false;
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        refresh(view);
        hover(e->position(), view);
        if (!mHover)
            return true;
        const SceneCurve& c = mCurves[mHover->curve];
        json call = {{"args", {c.path}}, {"tolerance", tolerance()}};
        if (mProjected)
            call["direction"] = vecJson(curvePick::viewAxis(view));
        try
        {
            if (mExtending)
            {
                call["point"] = vecJson(curve::pointAt(c.s, mAtEnd ? curve::spanCount(c.s) : 0.0));
                call["mode"] = curve::extendModeName(mExtendMode);
                ops::execute("extendCurve", call, CallSource::UI);
            }
            else
            {
                call["point"] = vecJson(mHover->point);
                ops::execute("trimCurve", call, CallSource::UI);
            }
        }
        catch (const CommandError& err)
        {
            log::warn("{}", err.what());
        }
        mVersion = ~uint64_t(0);
        mHover.reset();
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo& view) override
    {
        switch (e->key())
        {
        case Qt::Key_E: mExtending = !mExtending; break;
        case Qt::Key_P:
            mProjected = !mProjected;
            prefs().set("general", "/curveTrim/projected", mProjected);
            mVersion = ~uint64_t(0);
            refresh(view);
            break;
        case Qt::Key_M:
            mExtendMode = curve::ExtendMode((int(mExtendMode) + 1) % 3);
            prefs().set("general", "/curveTrim/extendMode", int(mExtendMode));
            break;
        case Qt::Key_Escape: exitTool(); return true;
        default: return false;
        }
        mHover.reset();
        viewportTools::requestRedraw();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        if (mHover && view.camera == mHoverView && mHover->curve < mCurves.size())
        {
            const SceneCurve& c = mCurves[mHover->curve];
            for (double u : mCuts[mHover->curve])
                dot(p, view, curve::pointAt(c.s, u), 3.5f, kTrimCut);
            if (!mPreview.empty())
                curvePick::drawPolyline(p, view, mPreview, false, 3.5f, mExtending ? kGrow : kTrimCut);
            if (mExtending && !mPreview.empty())
                dot(p, view, mPreview.back(), 4.0f, kGrow);
        }
        QString mode = mExtending ? QString("Extend (%1): hover near an end, click to extend it to the next curve  M: smooth/line/arc")
                                        .arg(curve::extendModeName(mExtendMode))
                                  : QString("Trim: click the highlighted stretch to delete it");
        if (mHover && mExtending && mPreview.empty())
            mode += "  (no curve in reach)";
        drawToolMessage(p, view, QString("%1  E: %2, P: intersect %3, Esc: exit").arg(mode, mExtending ? "trim" : "extend", mProjected ? "as seen" : "in 3D"));
    }

private:
    double tolerance() const { return std::max(1e-6, prefs().value<double>("general", "/curveTrim/tolerance", 0.01)); }

    curve::IntersectOptions options(const ViewInfo& view) const
    {
        curve::IntersectOptions o;
        o.tolerance = tolerance();
        if (mProjected)
            o.direction = curvePick::viewAxis(view);
        return o;
    }

    curve::Splines othersThan(size_t k) const
    {
        curve::Splines out;
        for (size_t j = 0; j < mCurves.size(); ++j)
            if (j != k)
                out.push_back(mCurves[j].s);
        return out;
    }

    /** Re-reads the curves and their cuts when the scene (or, intersecting as seen, the view) has changed. */
    void refresh(const ViewInfo& view)
    {
        uint64_t version = scene::Graph::current().version();
        D3 axis = curvePick::viewAxis(view);
        if (version == mVersion && (!mProjected || glm::dot(axis, mAxis) > 1.0 - 1e-9))
            return;
        mVersion = version;
        mAxis = axis;
        mCurves = curvePick::visibleCurves();
        mCuts.assign(mCurves.size(), {});
        curve::IntersectOptions o = options(view);
        for (size_t k = 0; k < mCurves.size(); ++k)
            mCuts[k] = curve::intersectionParams(mCurves[k].s, othersThan(k), o);
    }

    void hover(QPointF px, const ViewInfo& view)
    {
        mPreview.clear();
        mHover = curvePick::nearestCurve(mCurves, px, view, kSnapPx);
        if (!mHover)
            return;
        const SceneCurve& c = mCurves[mHover->curve];
        curve::TessOptions to;
        to.chordError = 0.005;
        if (!mExtending)
        {
            std::optional<curve::CurveInterval> iv = curve::intervalAt(c.s, mCuts[mHover->curve], mHover->u);
            if (!iv)
                return;
            curve::Spline piece = c.s.closed && std::abs(iv->u1 - iv->u0) < 1e-12 ? c.s : curve::subCurve(c.s, iv->u0, iv->u1);
            if (curve::isValid(piece))
            {
                curve::Polyline pl = curve::tessellate(piece, to);
                mPreview = pl.points;
                if (pl.closed && !mPreview.empty())
                    mPreview.push_back(mPreview.front());
            }
            return;
        }
        if (c.s.closed)
            return;
        mAtEnd = mHover->u > curve::spanCount(c.s) * 0.5;
        curve::ExtendOptions eo;
        eo.mode = mExtendMode;
        eo.intersect = options(view);
        curve::ExtendResult r = curve::extendTo(c.s, mAtEnd, othersThan(mHover->curve), eo);
        if (!r.hit)
            return;
        // Only the added part: the extended curve minus the original's length.
        curve::Spline ext = r.curve;
        double total = curve::length(ext), added = r.added;
        double from = curve::paramAtLength(ext, mAtEnd ? total - added : 0.0), to2 = curve::paramAtLength(ext, mAtEnd ? total : added);
        curve::Spline part = curve::subCurve(ext, from, to2);
        if (curve::isValid(part))
            mPreview = curve::tessellate(part, to).points;
        if (!mAtEnd)
            std::reverse(mPreview.begin(), mPreview.end());
    }

    std::vector<SceneCurve> mCurves;
    std::vector<std::vector<double>> mCuts;
    uint64_t mVersion = ~uint64_t(0);
    D3 mAxis{0.0};
    std::optional<curvePick::CurveHover> mHover;
    std::string mHoverView;
    std::vector<D3> mPreview;
    bool mExtending = false, mProjected = false, mAtEnd = true;
    curve::ExtendMode mExtendMode = curve::ExtendMode::Smooth;
};
} // namespace

void registerTrimCurveTool()
{
    if (!viewportTools::registry().find("curve.trimTool"))
        viewportTools::add({.id = "curve.trimTool", .label = "Trim Curves", .icon = "scissors", .create = [] { return std::make_unique<TrimCurveTool>(); }});
    actions::add({.id = "curve.trimTool",
                  .label = "Trim / Extend Tool",
                  .category = "Curves",
                  .run = [] { viewportTools::activate("curve.trimTool"); },
                  .doc = "Hover a curve to light up the stretch between its intersections and click to delete it; E extends ends to the next "
                         "curve instead; P intersects as seen in the view.",
                  .python = "lila.ui.run_action(\"curve.trimTool\")"});
}
} // namespace lila::ui
