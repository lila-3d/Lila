#include "uv/Flow.h"

#include "core/Profile.h"
#include "retopo/Internal.h"
#include "uv/Layout.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <limits>

namespace lila::uv
{
namespace
{
constexpr Index kMinTriangles = 8;
/** Polygon edges this much in agreement are a grid (hard surface quads), which beats any flow. */
constexpr double kGridAgreement = 0.4;
/** Flow this much in agreement runs straight across the island. */
constexpr double kFlowAgreement = 0.5;
/** How much bigger than its smallest box an island may get to follow its flow. */
constexpr double kFlowRoom = 1.15;
/** Mean |k1 - k2| times the island's size below which an uncreased island counts as flat. */
constexpr double kMinBend = 0.25;

/** The one of q's four quarter turns about n nearest ref. */
glm::dvec3 matchTurn(const glm::dvec3& q, const glm::dvec3& n, const glm::dvec3& ref)
{
    const glm::dvec3 p = glm::cross(n, q);
    const glm::dvec3 options[] = {q, p, -q, -p};
    glm::dvec3 best = q;
    double bestDot = -2.0;
    for (const glm::dvec3& o : options)
        if (const double d = glm::dot(o, ref); d > bestDot)
        {
            bestDot = d;
            best = o;
        }
    return best;
}
} // namespace

std::vector<glm::dvec3> chartFlow(const Chart& ch)
{
    LILA_ZONE();

    if (ch.triangleCount() < kMinTriangles)
        return {};
    retopo::detail::Surface s;
    s.pos = ch.pos;
    s.tris = ch.tris;
    s.density.assign(s.pos.size(), 1.0f);
    s.buildTopology();
    s.buildGeometry();
    s.detectFeatures(40.0, 75.0, 0.0);
    // Seams are where the island was cut, not how the surface runs: a border holds the field only along a crease.
    bool creased = false;
    for (Index v = 0; v < s.vertexCount(); ++v)
        if (s.onBoundary[v])
            s.feature[v] = retopo::detail::FeatureKind::None;
        else
            creased |= s.feature[v] == retopo::detail::FeatureKind::Line;
    // A flat island has no flow of its own: the field there is arbitrary.
    if (!creased)
    {
        const retopo::detail::Curvature k = retopo::detail::estimateCurvature(s);
        double bend = 0.0, area = 0.0;
        for (Index v = 0; v < s.vertexCount(); ++v)
        {
            bend += s.area[v] * k.anisotropy[v];
            area += s.area[v];
        }
        if (area <= 0.0 || bend / area * std::sqrt(area) < kMinBend)
            return {};
    }
    retopo::detail::FieldOptions fo;
    fo.scale = 4.0 * s.meanEdgeLength();
    fo.jointStiffness = 0.0;
    return retopo::detail::solveCrossField(s, fo);
}

double flowAngle(const Chart& ch, double* agreement)
{
    LILA_ZONE();

    if (agreement)
        *agreement = 0.0;
    const std::vector<glm::dvec3> q = chartFlow(ch);
    if (q.size() != ch.pos.size())
        return 0.0;
    double s = 0.0, c = 0.0, total = 0.0;
    for (const auto& t : ch.tris)
    {
        const glm::dvec3 e1 = ch.pos[t[1]] - ch.pos[t[0]], e2 = ch.pos[t[2]] - ch.pos[t[0]];
        const glm::dvec3 n = glm::cross(e1, e2);
        const double area = 0.5 * glm::length(n);
        if (area <= 0.0)
            continue;
        const glm::dvec3 nn = n / (2.0 * area);
        glm::dvec3 d = q[t[0]];
        d += matchTurn(q[t[1]], nn, d) + matchTurn(q[t[2]], nn, d);
        d -= nn * glm::dot(nn, d);
        // d = a e1 + b e2 in the triangle's plane, carried to uv by the same weights.
        const double g11 = glm::dot(e1, e1), g12 = glm::dot(e1, e2), g22 = glm::dot(e2, e2);
        const double det = g11 * g22 - g12 * g12;
        if (std::abs(det) < 1e-30)
            continue;
        const double r1 = glm::dot(d, e1), r2 = glm::dot(d, e2);
        const double a = (r1 * g22 - r2 * g12) / det, b = (r2 * g11 - r1 * g12) / det;
        const glm::dvec2 u = a * (ch.uv[t[1]] - ch.uv[t[0]]) + b * (ch.uv[t[2]] - ch.uv[t[0]]);
        if (glm::length(u) <= 0.0)
            continue;
        const double ang = 4.0 * std::atan2(u.y, u.x);
        s += area * std::sin(ang);
        c += area * std::cos(ang);
        total += area;
    }
    if (total <= 0.0)
        return 0.0;
    if (agreement)
        *agreement = std::hypot(s, c) / total;
    return -0.25 * std::atan2(s, c);
}

double uprightAngle(const Chart& ch)
{
    if (ch.uv.size() < 2)
        return 0.0;
    double agreement = 0.0;
    const double grid = gridAngle(ch, &agreement);
    if (agreement >= kGridAgreement)
        return grid;
    const std::vector<glm::dvec2> hull = convexHull(ch.uv);
    glm::dvec2 size(0.0);
    const double box = minAreaRectAngle(hull, &size);
    const double flow = flowAngle(ch, &agreement);
    if (agreement < kFlowAgreement)
        return box;
    const double c = std::cos(flow), sn = std::sin(flow);
    glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const glm::dvec2& p : hull)
    {
        const glm::dvec2 r(p.x * c - p.y * sn, p.x * sn + p.y * c);
        lo = glm::min(lo, r);
        hi = glm::max(hi, r);
    }
    const double flowArea = (hi.x - lo.x) * (hi.y - lo.y), boxArea = size.x * size.y;
    if (flowArea > kFlowRoom * boxArea)
        return box;
    // The flow is the same a quarter turn round; take the one that lays the long side along U, as the box does.
    return hi.y - lo.y > hi.x - lo.x ? flow + 0.5 * glm::pi<double>() : flow;
}
} // namespace lila::uv
