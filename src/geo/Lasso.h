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

/** Distance from p to the segment ab (to a when the segment has no length). */
inline double segmentDistance(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b)
{
    const glm::dvec2 d = b - a;
    const double len2 = glm::dot(d, d);
    const double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, d) / len2, 0.0, 1.0) : 0.0;
    return glm::distance(p, a + d * t);
}

/** The four corners of the box between lo and hi, as a region for touches. */
inline std::vector<glm::dvec2> box(glm::dvec2 lo, glm::dvec2 hi) { return {{lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}}; }

/** Whether segment ab overlaps the box between lo and hi anywhere: an end inside, or crossing an edge. */
inline bool segmentTouchesBox(glm::dvec2 a, glm::dvec2 b, glm::dvec2 lo, glm::dvec2 hi)
{
    if (std::max(a.x, b.x) < lo.x || std::min(a.x, b.x) > hi.x || std::max(a.y, b.y) < lo.y || std::min(a.y, b.y) > hi.y)
        return false;
    auto inside = [&](glm::dvec2 p) { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y; };
    if (inside(a) || inside(b))
        return true;
    const std::vector<glm::dvec2> corners = box(lo, hi);
    for (size_t i = 0, j = 3; i < 4; j = i++)
        if (segmentsCross(corners[j], corners[i], a, b))
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
