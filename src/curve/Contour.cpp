#include "curve/Contour.h"

#include "core/Profile.h"
#include "curve/Fit.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>

namespace lila::curve
{
namespace
{
using D3 = glm::dvec3;

D3 inPlaneAxis(const D3& n)
{
    D3 ref = std::abs(n.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0);
    return glm::normalize(glm::cross(n, ref));
}

/** Drops repeats (and a closed chain's copy of its start at the end). */
void dedupe(std::vector<D3>& pts, bool closed, double eps)
{
    std::vector<D3> out;
    out.reserve(pts.size());
    for (const D3& p : pts)
        if (out.empty() || glm::distance(out.back(), p) > eps)
            out.push_back(p);
    if (closed)
        while (out.size() > 1 && glm::distance(out.front(), out.back()) <= eps)
            out.pop_back();
    pts = std::move(out);
}

std::optional<Spline> chainCurve(std::vector<D3> pts, bool closed, const D3& n, const SliceOptions& o)
{
    const D3 u = inPlaneAxis(n);
    dedupe(pts, closed, 1e-9);
    if (pts.size() < (closed ? 3u : 2u))
        return std::nullopt;
    if (o.tolerance > 0.0)
    {
        std::vector<D3> kept;
        for (size_t i : simplifyIndices(pts, o.tolerance, closed))
            kept.push_back(pts[i]);
        pts = std::move(kept);
    }
    if (pts.size() < (closed ? 3u : 2u))
        return std::nullopt;
    if (closed)
    {
        D3 newell(0.0);
        for (size_t i = 0; i < pts.size(); ++i)
            newell += glm::cross(pts[i], pts[(i + 1) % pts.size()]);
        if (glm::length(newell) < 1e-12)
            return std::nullopt;
        if (glm::dot(newell, n) < 0.0)
            std::reverse(pts.begin(), pts.end());
        auto start = std::ranges::max_element(pts, [&](const D3& a, const D3& b) { return glm::dot(a, u) < glm::dot(b, u); });
        std::rotate(pts.begin(), start, pts.end());
    }
    else if (glm::dot(pts.back(), u) < glm::dot(pts.front(), u))
        std::reverse(pts.begin(), pts.end());

    if (o.smooth && pts.size() >= 3)
        return fitBezier(pts, closed, std::max(o.tolerance, 1e-4));
    Spline s;
    s.kind = Kind::Poly;
    s.closed = closed;
    s.points = std::move(pts);
    s.normalize();
    return s;
}
} // namespace

Splines sectionCurves(std::span<const geo::SectionMesh> meshes, const glm::dvec3& origin, const glm::dvec3& normal, const SliceOptions& o)
{
    LILA_ZONE();

    Splines out;
    if (!(glm::length(normal) > 1e-12))
        return out;
    const D3 n = glm::normalize(normal);
    if (o.bridge)
    {
        for (geo::SectionContour& c : geo::crossSections(meshes, origin, n))
            if (auto s = chainCurve(std::move(c.points), true, n, o))
                out.push_back(std::move(*s));
        return out;
    }
    for (geo::SectionChain& c : geo::sectionChains(meshes, origin, n))
        if (auto s = chainCurve(std::move(c.points), c.closed, n, o))
            out.push_back(std::move(*s));
    return out;
}

ContourResult contour(std::span<const geo::SectionMesh> meshes, const ContourOptions& o)
{
    LILA_ZONE();

    ContourResult r;
    if (!(glm::length(o.axis) > 1e-12))
    {
        r.warnings.push_back("the contour axis has no length");
        return r;
    }
    const D3 n = glm::normalize(o.axis);
    double lo = std::numeric_limits<double>::max(), hi = -lo;
    for (const geo::SectionMesh& m : meshes)
    {
        if (!m.mesh)
            continue;
        for (geo::Index p = 0; p < m.mesh->pointCapacity(); ++p)
            if (m.mesh->pointValid(p))
            {
                double d = glm::dot(D3(m.world * glm::dvec4(m.mesh->position(p), 1.0)), n);
                lo = std::min(lo, d);
                hi = std::max(hi, d);
            }
    }
    if (!(hi > lo))
    {
        r.warnings.push_back("the meshes have no extent along the axis");
        return r;
    }
    const double eps = (hi - lo) * 1e-9;
    if (o.count > 0)
    {
        if (o.count > o.maxPlanes)
        {
            r.warnings.push_back(std::format("{} planes asked for; at most {}", o.count, o.maxPlanes));
            return r;
        }
        for (int k = 0; k < o.count; ++k)
            r.planes.push_back(lo + (hi - lo) * double(k + 1) / double(o.count + 1));
    }
    else
    {
        if (!(o.spacing > 0.0))
        {
            r.warnings.push_back("spacing must be above 0");
            return r;
        }
        const double base = glm::dot(o.origin, n);
        const double first = base + std::ceil((lo + eps - base) / o.spacing) * o.spacing;
        if ((hi - first) / o.spacing > double(o.maxPlanes))
        {
            r.warnings.push_back(std::format("a spacing of {} cm makes over {} planes; raise it or set a count", o.spacing, o.maxPlanes));
            return r;
        }
        for (double d = first; d < hi - eps; d += o.spacing)
            r.planes.push_back(d);
    }
    if (r.planes.empty())
        r.warnings.push_back("no plane falls inside the meshes; lower the spacing");
    for (double d : r.planes)
        for (Spline& s : sectionCurves(meshes, n * d, n, o.slice))
            r.curves.push_back(std::move(s));
    return r;
}
} // namespace lila::curve
