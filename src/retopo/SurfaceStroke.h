#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

/**
 * Strokes drawn on a surface: the shared path every topology sketching tool's input goes through, from raw surface hits
 * to an even, smoothed polyline with its corners found.
 */
namespace lila::retopo
{
struct StrokeOptions
{
    /** Distance between output points; zero or negative keeps a fiftieth of the stroke's length. */
    double spacing = 0.0;
    /** Smoothing passes over the resampled points (corners and ends stay). */
    int smoothing = 2;
    /** Points turning sharper than this many degrees over the corner window are corners; negative finds none. */
    double cornerAngle = 55.0;
    /** How far either side of a point the turn is measured, in spacings. */
    int cornerWindow = 3;
    /** Close the stroke when it ends within this distance of its start; zero or negative never closes. */
    double closeDistance = 0.0;
};

/** Length of a polyline, including the closing segment when closed. */
LILA_SCENE_API double polylineLength(std::span<const glm::dvec3> points, bool closed = false);

struct SurfaceStroke
{
    std::vector<glm::dvec3> points;
    /** Indices into points, ascending; a closed stroke's first point is a corner only when it really turns there. */
    std::vector<int> corners;
    bool closed = false;

    double length() const { return polylineLength(points, closed); }
    bool empty() const { return points.size() < 2; }
};

/**
 * Points evenly spaced by arc length along a polyline: segments + 1 of them for an open one (both ends kept), segments
 * for a closed one (starting at its first point). With a surface each lands back on it.
 */
LILA_SCENE_API std::vector<glm::dvec3> resamplePolyline(std::span<const glm::dvec3> points, int segments, bool closed = false,
                                                        const geo::MeshBvh* surface = nullptr);

/** The point at a fraction of the way along a polyline by arc length, 0 to 1. */
LILA_SCENE_API glm::dvec3 pointAlong(std::span<const glm::dvec3> points, double fraction, bool closed = false);

/**
 * Points where the polyline turns sharper than angleDegrees, measured between the points `window` steps either side, one
 * per sharp bend (the sharpest point of each run). Ends of an open polyline are never corners.
 */
LILA_SCENE_API std::vector<int> findCorners(std::span<const glm::dvec3> points, bool closed, double angleDegrees, int window = 3);

/**
 * Turns raw surface hits (in drawing order) into an even stroke on the surface: duplicate and backtracking samples
 * dropped, resampled by arc length, smoothed with corners and ends held, projected back onto the surface, corners found,
 * closed when it ends near its start.
 */
LILA_SCENE_API SurfaceStroke processStroke(std::span<const glm::dvec3> raw, const geo::MeshBvh* surface, const StrokeOptions& opts = {});

/** Index of the polyline point nearest p. */
LILA_SCENE_API int nearestPointIndex(std::span<const glm::dvec3> points, const glm::dvec3& p);

/** The part of a polyline from index a to index b inclusive; for a closed one it runs forward and wraps past the end. */
LILA_SCENE_API std::vector<glm::dvec3> polylineSpan(std::span<const glm::dvec3> points, int a, int b, bool closed);
} // namespace lila::retopo
