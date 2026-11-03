#pragma once

#include "geo/Primitives.h"
#include "groom/Attrs.h"
#include "groom/Groom.h"

#include <glm/glm.hpp>

namespace groomtest
{
using D3 = glm::dvec3;

/** A head-sized sphere scalp, radius 10 cm at the origin. */
inline lila::geo::Mesh sphereMesh(double radius = 10.0)
{
    lila::geo::SphereOptions o;
    o.radius = radius;
    o.subdivAxis = 48;
    o.subdivHeight = 32;
    return lila::geo::makeSphere(o);
}

inline lila::groom::Surface sphereScalp(double radius = 10.0) { return lila::groom::Surface(sphereMesh(radius)); }

/** A poly guide from root out along dir, n points over len. */
inline lila::curve::Spline guide(const D3& root, const D3& dir, double len, int n = 12, double id = 0.0)
{
    lila::curve::Spline s;
    s.kind = lila::curve::Kind::Poly;
    for (int i = 0; i < n; ++i)
        s.points.push_back(root + glm::normalize(dir) * (len * i / (n - 1)));
    if (id > 0.0)
        lila::groom::attr::set(s, lila::groom::attr::kId, id);
    return s;
}

/** A lock lying over the top of the sphere: root at the crown, then down the side over the surface. */
inline lila::curve::Spline lyingGuide(double radius, double azimuth, int n = 16, double id = 1.0)
{
    lila::curve::Spline s;
    s.kind = lila::curve::Kind::Poly;
    for (int i = 0; i < n; ++i)
    {
        const double polar = 0.15 + 1.2 * i / (n - 1);
        s.points.push_back(D3(std::sin(polar) * std::cos(azimuth), std::cos(polar), std::sin(polar) * std::sin(azimuth)) * (radius + 0.05));
    }
    lila::groom::attr::set(s, lila::groom::attr::kId, id);
    return s;
}

/** How far inside the sphere the deepest point is (0 when none is). */
inline double deepest(const lila::curve::Splines& s, double radius, double margin = 0.0)
{
    double d = 0.0;
    for (const lila::curve::Spline& sp : s)
        for (size_t i = 1; i < sp.size(); ++i)
            d = std::max(d, radius + margin - glm::length(sp.points[i]));
    return d;
}
} // namespace groomtest
