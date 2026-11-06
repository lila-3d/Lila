// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <vector>

#include "groomlab/math.h"

namespace groomlab {

// The point at t along a polyline of n points (t in [0, 1] by point index, not
// arc length). Outside [0, 1] it holds the end point, or with `extrapolate`
// carries on along the first or last segment.
inline Vec3f pointAlong(const Vec3f* pts, std::size_t n, float t, bool extrapolate = false) {
    if (n == 0) return {};
    if (n == 1) return pts[0];
    if (!extrapolate) t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float x = t * static_cast<float>(n - 1);
    const std::size_t i = x <= 0.0f ? 0 : (static_cast<std::size_t>(x) < n - 1 ? static_cast<std::size_t>(x) : n - 2);
    const float f = x - static_cast<float>(i);
    return pts[i] * (1.0f - f) + pts[i + 1] * f;
}

// Total polyline length.
float arcLength(const Vec3f* points, std::size_t count);
inline float arcLength(const std::vector<Vec3f>& points) { return arcLength(points.data(), points.size()); }

// The arc length from the first point to each point (out[0] = 0; count values).
void cumulativeArcLength(const Vec3f* points, std::size_t count, float* out);

// Each point's arc-length position along the polyline, 0 at the first point to
// 1 at the last (count values); by point index when it has no length.
void arcParams(const Vec3f* points, std::size_t count, float* out);

// Values placed at the points of a polyline (one per point; `valueCount` may
// be short, the last one then holding on) resampled to `outCount` values
// evenly spaced by arc length, as resampleByArcLength spaces the points:
// a per-point layer following its curve's resampling.
void resampleValuesByArcLength(const Vec3f* points, std::size_t count, const float* values, std::size_t valueCount,
                               float* out, std::size_t outCount);

// Resamples a polyline to `outCount` points evenly spaced by arc length,
// covering the fraction [0, lengthFraction] of the curve (1.0 = whole curve).
// A single-point input repeats that point; outCount 0 returns empty.
std::vector<Vec3f> resampleByArcLength(const Vec3f* points, std::size_t count, std::size_t outCount,
                                       float lengthFraction = 1.0f);

// Same, writing into `out` (must hold outCount points). Avoids allocation in
// per-strand loops.
void resampleByArcLengthInto(const Vec3f* points, std::size_t count, Vec3f* out, std::size_t outCount,
                             float lengthFraction = 1.0f);

// A smooth curve through every point (centripetal Catmull-Rom: no loops or
// overshoot on uneven spacing), `subdivisions` pieces per segment: point i of
// the input is point i * subdivisions of the result. Below 2 returns the input.
std::vector<Vec3f> smoothPolyline(const Vec3f* points, std::size_t count, int subdivisions);

// Resamples a curve to `outCount` points evenly spaced by arc length along the smooth curve
// through its points (smoothPolyline), not along the straight spans between them: more points
// keep a bend round, as the viewport draws it (Resample Guides). Fewer than 3 points: as
// resampleByArcLength.
std::vector<Vec3f> resampleSmoothCurve(const Vec3f* points, std::size_t count, std::size_t outCount);

}  // namespace groomlab
