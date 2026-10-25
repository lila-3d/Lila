#pragma once

#include "curve/Tessellate.h"
#include "geo/Mesh.h"
#include "scene/Node.h"
#include "ui/Overlay.h"
#include "ui/ViewInfo.h"

#include <QPointF>

#include <optional>
#include <string>
#include <vector>

/** Hover picking shared by the blend and trim tools: curves and mesh border edges under the cursor, in world space. */
namespace lila::ui::curvePick
{
using D3 = glm::dvec3;

/** One spline of a visible curve node, world space, with a fine tessellation for screen picking. */
struct SceneCurve
{
    scene::NodeId node = scene::kNoNode;
    std::string path;
    size_t spline = 0;
    curve::Spline s;
    curve::Polyline pl;
};

/** Every spline of every visible curve. */
std::vector<SceneCurve> visibleCurves();

struct CurveHover
{
    size_t curve = 0; // index into the list searched
    double u = 0.0;   // span parameter
    D3 point{0.0};
    double px = 0.0; // screen distance
};
/** The curve nearest the cursor within maxPx. */
std::optional<CurveHover> nearestCurve(const std::vector<SceneCurve>& curves, QPointF px, const ViewInfo& view, double maxPx);

struct BorderHover
{
    scene::NodeId mesh = scene::kNoNode;
    std::string path;
    geo::Index edge = geo::kInvalid;
    double t = 0.5;
    D3 point{0.0};
    double px = 0.0;
};
/** The border edge of a visible mesh nearest the cursor within maxPx. */
std::optional<BorderHover> nearestBorder(QPointF px, const ViewInfo& view, double maxPx);

/** The visible mesh's world-space copy (for border geometry). */
std::optional<geo::Mesh> worldMesh(scene::NodeId mesh);

/** A polyline drawn in the overlay, so it sits over everything. */
void drawPolyline(OverlayPainter& p, const ViewInfo& view, const std::vector<D3>& pts, bool closed, float width, const glm::vec4& color);
/** The view's looking direction, for intersecting as seen. */
D3 viewAxis(const ViewInfo& view);
} // namespace lila::ui::curvePick
