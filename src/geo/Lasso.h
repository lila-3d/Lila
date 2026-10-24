#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <utility>
#include <vector>

/** A freehand lasso in screen space: the outline the cursor drew, closed from its last point back to its first. */
namespace lila::geo::lasso
{
/** Even-odd containment, so a lasso drawn crossing itself leaves its overlaps out. Fewer than 3 points hold nothing. */
inline bool contains(const std::vector<glm::dvec2>& poly, glm::dvec2 p)
{
    if (poly.size() < 3)
        return false;
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        const glm::dvec2& a = poly[i];
        const glm::dvec2& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

/** Whether segments ab and cd cross or touch. */
inline bool segmentsCross(glm::dvec2 a, glm::dvec2 b, glm::dvec2 c, glm::dvec2 d)
{
    auto cross = [](glm::dvec2 o, glm::dvec2 p, glm::dvec2 q) { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
    auto within = [](glm::dvec2 o, glm::dvec2 p, glm::dvec2 q) {
        return std::min(o.x, p.x) <= q.x && q.x <= std::max(o.x, p.x) && std::min(o.y, p.y) <= q.y && q.y <= std::max(o.y, p.y);
    };
    const double d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return true;
    return (d1 == 0 && within(c, d, a)) || (d2 == 0 && within(c, d, b)) || (d3 == 0 && within(a, b, c)) || (d4 == 0 && within(a, b, d));
}

/**
 * Whether a closed region and a shape overlap at all: a point of either inside the other, or outlines crossing. A shape
 * of two points is a segment.
 */
inline bool touches(const std::vector<glm::dvec2>& region, const std::vector<glm::dvec2>& shape)
{
    for (const glm::dvec2& p : shape)
        if (contains(region, p))
            return true;
    if (!region.empty() && contains(shape, region.front()))
        return true;
    const size_t shapeEdges = shape.size() == 2 ? 1 : shape.size();
    for (size_t i = 0, j = region.size() - 1; i < region.size(); j = i++)
        for (size_t k = 0; k < shapeEdges; ++k)
            if (segmentsCross(region[j], region[i], shape[k], shape[(k + 1) % shape.size()]))
                return true;
    return false;
}

/** Appends p unless it is within minDistance of the last point. Returns whether it was added. */
inline bool extend(std::vector<glm::dvec2>& poly, glm::dvec2 p, double minDistance = 2.0)
{
    if (!poly.empty() && glm::distance(poly.back(), p) < minDistance)
        return false;
    poly.push_back(p);
    return true;
}

/** Min and max corners; both zero when empty. */
inline std::pair<glm::dvec2, glm::dvec2> bounds(const std::vector<glm::dvec2>& poly)
{
    if (poly.empty())
        return {};
    glm::dvec2 lo = poly.front(), hi = poly.front();
    for (const glm::dvec2& p : poly)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    return {lo, hi};
}
} // namespace lila::geo::lasso
