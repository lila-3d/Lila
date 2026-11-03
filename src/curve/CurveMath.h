#pragma once

#include <glm/glm.hpp>

#include <cmath>

/** Vector shorthands and small helpers the curve kernels share. Internal to src/curve. */
namespace lila::curve
{
using D2 = glm::dvec2;
using D3 = glm::dvec3;
using D4 = glm::dvec4;

/** Length below which a vector or an interval counts as zero. */
inline constexpr double kEps = 1e-12;

/** 8-point Gauss-Legendre nodes and weights on [-1, 1]. */
inline constexpr double kGx[8] = {-0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
                                  0.1834346424956498,  0.5255324099163290,  0.7966664774136267,  0.9602898564975363};
inline constexpr double kGw[8] = {0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
                                  0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763};

/** The z of the 2D cross product: positive when b turns left of a. */
inline double cross2(const D2& a, const D2& b) { return a.x * b.y - a.y * b.x; }

/** v normalised, or zero when it is shorter than kEps. */
inline D3 unit(const D3& v)
{
    const double l = glm::length(v);
    return l > kEps ? v / l : D3(0.0);
}

/** The part of v perpendicular to unit t, normalised; zero when that part is no longer than minLength. */
inline D3 perpUnit(const D3& v, const D3& t, double minLength = 1e-12)
{
    const D3 r = v - t * glm::dot(v, t);
    const double l = glm::length(r);
    return l > minLength ? r / l : D3(0.0);
}

/** Unsigned angle between a and b in radians; 0 when either is shorter than kEps. */
inline double angleBetween(const D3& a, const D3& b)
{
    if (glm::length(a) < kEps || glm::length(b) < kEps)
        return 0.0;
    return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}
} // namespace lila::curve
