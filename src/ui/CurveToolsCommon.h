#pragma once

#include "core/PrefStore.h"
#include "curve/Spline.h"
#include "math/MathCore.h"
#include "render/FrameState.h"
#include "ui/Overlay.h"
#include "ui/SurfaceSnap.h"
#include "ui/Tools.h"
#include "ui/ViewInfo.h"

#include <QPointF>
#include <QWheelEvent>

#include <algorithm>
#include <map>
#include <cmath>
#include <optional>

/** What the curve draw tools and the pipe tool share: point placement with snapping, previews and dots. */
namespace lila::ui::curveTools
{
using D3 = glm::dvec3;

inline constexpr double kSnapPx = 15.0;
inline constexpr double kClosePx = 10.0;

inline const glm::vec4 kPreview(0.95f, 0.95f, 0.95f, 1.0f);
inline const glm::vec4 kGhost(0.55f, 0.55f, 0.55f, 1.0f);
inline const glm::vec4 kRim(0.05f, 0.05f, 0.05f, 1.0f);
inline const glm::vec4 kDot(0.85f, 0.85f, 0.85f, 1.0f);

inline double distPx(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }
inline PrefStore& prefs() { return PrefStore::get(); }
inline D3 upAxis() { return math::upAxis() == math::UpAxis::Z ? D3(0, 0, 1) : D3(0, 1, 0); }

/** The nearest mesh or curve point (V) or point on a mesh edge or curve (C) within kSnapPx of the cursor. */
std::optional<D3> snapToGeometry(QPointF px, const ViewInfo& view, unsigned snaps);
/** The spline, tessellated, as lines drawn over everything. */
void drawSpline(render::FrameState& f, const curve::Spline& s, const glm::vec4& color);
void dot(OverlayPainter& p, const ViewInfo& view, const D3& w, float r = 4.0f, const glm::vec4& fill = kDot);
/** Ends the active tool, deferred so a tool can call it from its own handlers. */
void exitTool();

/** The visible mesh surface under the cursor, its point lifted along the normal by offset. */
inline std::optional<surfaceSnap::Hit> surfaceUnder(QPointF px, const ViewInfo& view, double offset)
{
    auto hit = surfaceSnap::underCursor(px, view);
    if (hit)
        hit->position += hit->normal * offset;
    return hit;
}

/**
 * Where the curve tools put points: snapped geometry, a surface, or the construction plane (grid snapped with X). Drawing on
 * surfaces (onSurface) puts them on whatever mesh is under the cursor unless grid snapping asks for the plane; the snap
 * surface and N-held surface snapping do so whatever the setting.
 */
class Placer
{
public:
    std::optional<D3> place(QPointF px, const ViewInfo& view, D3* normalOut = nullptr, scene::NodeId* surfaceOut = nullptr) const
    {
        unsigned snaps = tools::snaps();
        if (snaps & (SnapVertex | SnapEdge))
            if (auto s = snapToGeometry(px, view, snaps))
                return s;
        if ((mSurface && !(snaps & SnapGrid)) || surfaceSnap::active() || (snaps & SnapSurface))
            if (auto hit = surfaceUnder(px, view, mOffset))
            {
                if (normalOut)
                    *normalOut = hit->normal;
                if (surfaceOut)
                    *surfaceOut = hit->node;
                return hit->position;
            }
        D3 o, d;
        view.ray(px, o, d);
        auto [origin, normal] = plane(view);
        double den = glm::dot(d, normal);
        if (std::abs(den) < 1e-9)
            return std::nullopt;
        double t = glm::dot(origin - o, normal) / den;
        if (!view.ortho && t < 0.0)
            return std::nullopt;
        D3 p = o + d * t;
        if (snaps & SnapGrid)
        {
            double step = std::max(1e-6, prefs().value<double>("general", "/grid/spacing", 10.0));
            D3 q = p - origin;
            for (int i = 0; i < 3; ++i)
                q[i] = std::round(q[i] / step) * step;
            // Back onto the plane, in case rounding lifted it off.
            q -= normal * glm::dot(q, normal);
            p = origin + q;
        }
        if (normalOut)
            *normalOut = normal;
        return p;
    }

    /** Planar lock: after the first point, points stay on the plane through it. */
    void lockTo(const D3& p) { mAnchor = p; }
    void unlock() { mAnchor.reset(); }
    bool locked() const { return mAnchor.has_value(); }
    void toggleLock(bool on) { mLock = on; }
    bool lockEnabled() const { return mLock; }
    /** Points go on mesh surfaces under the cursor, lifted off them by offset cm. */
    void onSurface(bool on, double offset)
    {
        mSurface = on;
        mOffset = offset;
    }

    /** The construction plane: the ground in a perspective view, the view plane in an orthographic one. */
    std::pair<D3, D3> plane(const ViewInfo& view) const
    {
        D3 normal = view.ortho ? glm::normalize(D3(glm::inverse(view.view)[2])) : upAxis();
        D3 origin = mLock && mAnchor ? *mAnchor : D3(0.0);
        return {origin, normal};
    }

private:
    std::optional<D3> mAnchor;
    bool mLock = true;
    bool mSurface = false;
    double mOffset = 0.0;
};

/**
 * The draw tools' live surface options, kept in the general prefs under /curveTools/: draw on surfaces (S in the tools), keep
 * the finished curve on the surface it was drawn on (K), and the offset off it (Ctrl+scroll). Remembers which meshes the
 * placed points landed on, so the finished curve can stay on the one most used.
 */
class SurfaceDraw
{
public:
    static bool onPref() { return prefs().value<bool>("general", "/curveTools/onSurface", false); }
    static bool conformPref() { return prefs().value<bool>("general", "/curveTools/conform", false); }
    static double offsetPref() { return prefs().value<double>("general", "/curveTools/surfaceOffset", 0.0); }

    void load()
    {
        mOn = onPref();
        mConform = conformPref();
        mOffset = offsetPref();
        mHits.clear();
    }
    void apply(Placer& p) const { p.onSurface(mOn, mOffset); }
    /** S and K toggles; true when the key was one of them. */
    bool key(int key)
    {
        if (key == Qt::Key_S)
            prefs().set("general", "/curveTools/onSurface", mOn = !mOn);
        else if (key == Qt::Key_K)
            prefs().set("general", "/curveTools/conform", mConform = !mConform);
        else
            return false;
        return true;
    }
    /** Ctrl+scroll sets the offset while drawing on surfaces. */
    bool wheel(const QWheelEvent* e)
    {
        if (!mOn || !(e->modifiers() & Qt::ControlModifier))
            return false;
        const double steps = e->angleDelta().y() / 120.0;
        mOffset = std::max(0.0, std::round((mOffset + steps * std::max(0.1, mOffset * 0.1)) * 100.0) / 100.0);
        prefs().set("general", "/curveTools/surfaceOffset", mOffset);
        return true;
    }
    void landed(scene::NodeId id)
    {
        if (id != scene::kNoNode)
            ++mHits[id];
    }
    void clearHits() { mHits.clear(); }
    /** The mesh to keep the finished curve on: the one most points landed on, when keeping is on. */
    std::optional<scene::NodeId> conformTarget() const
    {
        if (!mConform || mHits.empty())
            return std::nullopt;
        return std::ranges::max_element(mHits, [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    }
    bool on() const { return mOn; }
    double offset() const { return mOffset; }
    QString status() const
    {
        return QString("S: on surfaces %1, K: keep on surface %2%3")
            .arg(mOn ? "on" : "off", mConform ? "on" : "off", mOn ? QString(", Ctrl+scroll: offset %1 cm").arg(mOffset, 0, 'f', 2) : QString());
    }

private:
    bool mOn = false, mConform = false;
    double mOffset = 0.0;
    std::map<scene::NodeId, int> mHits;
};
} // namespace lila::ui::curveTools
