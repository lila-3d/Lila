#include "brush/Symmetry.h"

#include <algorithm>
#include <cmath>

namespace lila::brush
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

glm::dvec3 reflectVector(const geo::SymmetryPlane& plane, const glm::dvec3& v) { return v - 2.0 * glm::dot(v, plane.normal) * plane.normal; }

glm::dvec3 rotate(const glm::dvec3& v, const glm::dvec3& axis, double c, double s)
{
    return v * c + glm::cross(axis, v) * s + axis * glm::dot(axis, v) * (1.0 - c);
}
} // namespace

Dab mirrorDab(const Dab& d, const geo::SymmetryPlane& plane)
{
    Dab m = d;
    m.center = plane.reflect(d.center);
    m.eye = plane.reflect(d.eye);
    m.normal = reflectVector(plane, d.normal);
    m.viewDir = reflectVector(plane, d.viewDir);
    m.delta = reflectVector(plane, d.delta);
    m.direction = reflectVector(plane, d.direction);
    m.tangent = reflectVector(plane, d.tangent);
    // The reflection runs the tip's turn the other way.
    m.angle = -d.angle;
    return m;
}

Dab rotateDab(const Dab& d, const glm::dvec3& axis, double angle)
{
    const double c = std::cos(angle), s = std::sin(angle);
    Dab r = d;
    r.center = rotate(d.center, axis, c, s);
    r.eye = rotate(d.eye, axis, c, s);
    r.normal = rotate(d.normal, axis, c, s);
    r.viewDir = rotate(d.viewDir, axis, c, s);
    r.delta = rotate(d.delta, axis, c, s);
    r.direction = rotate(d.direction, axis, c, s);
    r.tangent = rotate(d.tangent, axis, c, s);
    return r;
}

std::vector<Dab> radialDabs(const Dab& d, const Radial& radial)
{
    std::vector<Dab> out{d};
    if (!radial.any())
        return out;
    glm::dvec3 axis(0.0);
    axis[std::clamp(radial.axis, 0, 2)] = 1.0;
    for (int k = 1; k < radial.count; ++k)
    {
        out.push_back(rotateDab(d, axis, 2.0 * kPi * k / radial.count));
        out.back().copy = uint32_t(k);
    }
    return out;
}

std::vector<Dab> symmetricDabs(const Dab& d, const geo::SymmetryPlane* mirror, const Radial& radial)
{
    std::vector<Dab> out = radialDabs(d, radial);
    if (mirror)
    {
        const size_t n = out.size();
        for (size_t i = 0; i < n; ++i)
        {
            out.push_back(mirrorDab(out[i], *mirror));
            out.back().copy = uint32_t(n + i);
        }
    }
    return out;
}

void mergeHits(std::vector<std::pair<geo::Index, float>>& hits)
{
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first || (a.first == b.first && a.second > b.second); });
    hits.erase(std::unique(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), hits.end());
}

void mirrorHits(std::vector<std::pair<geo::Index, float>>& hits, const geo::SymmetryMap& map, std::span<const uint8_t> mask)
{
    if (map.empty())
        return;
    const size_t n = hits.size();
    for (size_t i = 0; i < n; ++i)
        if (geo::Index q = map.mirrorPoint(hits[i].first); q != geo::kInvalid && q != hits[i].first && (mask.empty() || (q < mask.size() && mask[q])))
            hits.emplace_back(q, hits[i].second);
    mergeHits(hits);
}
} // namespace lila::brush
