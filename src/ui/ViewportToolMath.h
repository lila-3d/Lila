#pragma once

#include <QPointF>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace lila::ui
{
/** How close, in pixels, the cursor has to be to pick a vertex or edge in the topology tools. */
inline constexpr double kPickRadiusPx = 12.0;
/** How close, in pixels, the cursor has to be to grab a manipulator or tool handle. */
inline constexpr double kHandleGrabPx = 12.0;

inline double distPx(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

/** Pixel distance from p to the segment ab; `t` gets the closest point's parameter along it. */
inline double segmentDistPx(QPointF p, QPointF a, QPointF b, double* t = nullptr)
{
    QPointF ab = b - a;
    double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double u = len2 > 0.0 ? std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2, 0.0, 1.0) : 0.0;
    if (t)
        *t = u;
    return distPx(p, a + ab * u);
}

/** A point through a transform. */
inline glm::dvec3 xf(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

/** [x, y, z], the way commands take a point or vector. */
inline nlohmann::json vecJson(const glm::dvec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }

inline nlohmann::json vecListJson(const std::vector<glm::dvec3>& ps)
{
    nlohmann::json out = nlohmann::json::array();
    for (const glm::dvec3& p : ps)
        out.push_back(vecJson(p));
    return out;
}
} // namespace lila::ui
