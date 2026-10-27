#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <span>

namespace lila::curve
{
/**
 * Fits a Bezier through noisy points (a pencil stroke) within tolerance cm: Schneider's algorithm, split at corners
 * sharper than cornerAngle radians, where the handles break.
 */
LILA_SCENE_API Spline fitBezier(std::span<const glm::dvec3> pts, bool closed, double tolerance, double cornerAngle = 1.2);
/** Cubic pieces approximating any piece within tolerance (rational or high degree pieces to Bezier). */
LILA_SCENE_API std::vector<Piece> approximateCubic(const Piece& p, double tolerance);

/** NURBS of the given degree through every point (global interpolation, centripetal parameters): the EP curve. */
LILA_SCENE_API Spline interpolate(std::span<const glm::dvec3> pts, int degree, bool closed);
/** Least squares NURBS with `spans` uniform spans, ends pinned on open curves (Piegl & Tiller 9.4.1). */
LILA_SCENE_API Spline fitNurbs(std::span<const glm::dvec3> pts, int degree, int spans, bool closed);

struct RebuildOptions
{
    Kind kind = Kind::Nurbs;
    int spans = 8;
    int degree = 3;
    /** Keep tangent breaks: each run between kinks is rebuilt separately and the kinks stay sharp. */
    bool keepCorners = true;
    double tolerance = 0.01; // Bezier fitting, cm
    /** Keep the curve's kind and point count and only spread the points evenly by arc length (kind and spans are ignored). */
    bool keepPointCount = false;
    /** With keepPointCount, the point count as a percentage of the curve's own, so 50 halves it. */
    double pointPercent = 100.0;
};
/** Even, clean parameterisation: samples the curve and refits it with the given span count. */
LILA_SCENE_API Spline rebuild(const Spline& s, const RebuildOptions& o);

/** Ramer-Douglas-Peucker: indices of the points kept (ends always). */
LILA_SCENE_API std::vector<size_t> simplifyIndices(std::span<const glm::dvec3> pts, double tolerance, bool closed);
/** Fewer points within tolerance: poly curves by RDP, others refitted as Bezier with corners kept. */
LILA_SCENE_API Spline simplify(const Spline& s, double tolerance);

/** Interpolating draw modes, both returning a Bezier through every point. */
/** Centripetal Catmull-Rom (alpha 0.5; no cusps or self-intersections inside a span, Yuksel et al. 2011). */
LILA_SCENE_API Spline catmullRom(std::span<const glm::dvec3> pts, bool closed, double alpha = 0.5);
/** kappa-curves (Yan et al. 2017): every point is a curvature maximum; quadratic pieces made G2, returned as cubics. */
LILA_SCENE_API Spline kappaCurve(std::span<const glm::dvec3> pts, bool closed, int iterations = 30);
} // namespace lila::curve
