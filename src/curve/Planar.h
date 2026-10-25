#pragma once

#include "core/Export.h"
#include "curve/Spline.h"
#include "curve/Tessellate.h"

#include <span>
#include <string>

/** Planar regions from closed curves: the plane they lie in, their loops in 2D, and how the loops nest into outers and holes. */
namespace lila::curve
{
/** A plane: origin, in-plane axes and normal (x cross y). */
struct LILA_SCENE_API PlaneFrame
{
    glm::dvec3 origin{0.0};
    glm::dvec3 x{1.0, 0.0, 0.0};
    glm::dvec3 y{0.0, 1.0, 0.0};
    glm::dvec3 normal{0.0, 0.0, 1.0};

    glm::dvec2 to2(const glm::dvec3& p) const { return {glm::dot(p - origin, x), glm::dot(p - origin, y)}; }
    glm::dvec3 to3(const glm::dvec2& q, double height = 0.0) const { return origin + x * q.x + y * q.y + normal * height; }
    double height(const glm::dvec3& p) const { return glm::dot(p - origin, normal); }
};

/** A frame through origin with this normal; the in-plane axes are picked deterministically from it. */
LILA_SCENE_API PlaneFrame planeFrame(const glm::dvec3& origin, const glm::dvec3& normal);

/**
 * The plane closed curves lie in: the Newell normals of every loop turned to agree and summed, through the centroid of the
 * tessellated points. A non-zero normal given wins. The normal's largest component comes out positive (so curves on the
 * ground face up). deviation gets how far the farthest point lies off the plane.
 */
LILA_SCENE_API PlaneFrame fitPlane(std::span<const Spline> curves, const glm::dvec3& normal = glm::dvec3(0.0), double* deviation = nullptr);

/** A closed loop in a plane's 2D coordinates, tessellated from one spline. */
struct Loop2
{
    std::vector<glm::dvec2> points; // not repeated at the end
    std::vector<double> params;     // span parameter of each point on the source spline
    std::vector<uint8_t> kink;      // tangent breaks
    int source = -1;                // index of the spline it came from
};
using Loops2 = std::vector<Loop2>;

/** Positive for counter-clockwise. */
LILA_SCENE_API double signedArea(std::span<const glm::dvec2> pts);
LILA_SCENE_API bool pointInPolygon(const glm::dvec2& p, std::span<const glm::dvec2> poly);
/** Reverses a loop's direction, keeping its first point first. */
LILA_SCENE_API void reverseLoop(Loop2& l);

/**
 * Closed splines as 2D loops on the plane, repeated points dropped. Open, too short or zero area splines are skipped and
 * described in warnings.
 */
LILA_SCENE_API Loops2 loopsOf(std::span<const Spline> curves, const PlaneFrame& plane, const TessOptions& tess,
                              std::vector<std::string>* warnings = nullptr);

/** One outer loop (counter-clockwise) and the holes directly inside it (clockwise). */
struct Region2
{
    Loop2 outer;
    std::vector<Loop2> holes;
};

/**
 * Nests loops by containment: loops at even depth are outers, odd ones holes of the loop directly around them (an island
 * in a hole is an outer again). Orientations are fixed to match. The loops must not cross each other.
 */
LILA_SCENE_API std::vector<Region2> nestLoops(Loops2 loops);

/** Bezier handles of straight spans set to vector (the span stays a line when its points move). */
LILA_SCENE_API void vectorHandlesOnLines(Spline& s);

/** True when any two loops cross or touch, so they need a boolean union before they can be nested. */
LILA_SCENE_API bool loopsCross(const Loops2& loops);
} // namespace lila::curve
