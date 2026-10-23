#pragma once

#include <glm/glm.hpp>

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
