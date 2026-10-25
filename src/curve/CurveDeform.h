#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <span>

/**
 * Deformers that reshape curves themselves: bend, curl, scale curvature, smooth, and even spacing. A mask (one value per
 * point, empty for all) limits the edit to some points; a Bezier point's handles move with it.
 */
namespace lila::curve
{
struct BendCurveOptions
{
    /** Total bend over the curve's chord, radians; positive bends toward the side the axis turns the chord. */
    double angle = 1.5707963267948966;
    /** Bend axis; zero takes the curve's plane normal (toward +Y), or the up direction across the chord for a straight one. */
    glm::dvec3 axis{0.0};
    /** Bend both halves about the chord's middle instead of from the start. */
    bool fromCenter = false;
};
/** Bends the curve around an axis like a bend deformer laid along its chord (start to end; closed curves their longest extent). */
LILA_SCENE_API Spline bendCurve(const Spline& s, const BendCurveOptions& o, std::span<const uint8_t> mask = {});

struct CurlCurveOptions
{
    /** Radius of the curl, cm. */
    double amplitude = 2.0;
    /** Turns over the whole curve. */
    double turns = 6.0;
    /** Ease the curl in and out over the first and last tenth so the ends stay put. */
    bool keepEnds = true;
    /** Resample (points per turn) so the coils are really there; off moves the existing points only. A mask turns it off. */
    int samplesPerTurn = 12;
};
/** Coils the curve around itself, a helix about its rotation-minimising frame. */
LILA_SCENE_API Spline curlCurve(const Spline& s, const CurlCurveOptions& o, std::span<const uint8_t> mask = {});

/**
 * Multiplies the curve's curvature: below 1 flattens it toward a straight line along its start tangent, above 1 bends it
 * tighter. Lengths along it stay; the start stays put. Points off the curve (control points) follow the frame there.
 */
LILA_SCENE_API Spline scaleCurvature(const Spline& s, double factor, std::span<const uint8_t> mask = {});

struct SmoothCurveOptions
{
    /** How far each pass moves a point toward its neighbours' average, 0 to 1. */
    double strength = 0.5;
    int iterations = 1;
    /** Open curves keep their end points. */
    bool keepEnds = true;
};
/** Laplacian smoothing of the control points (Bezier anchors, carrying their handles). */
LILA_SCENE_API Spline smoothCurve(const Spline& s, const SmoothCurveOptions& o, std::span<const uint8_t> mask = {});

/**
 * The same kind and point count, the points spread evenly by arc length. With keepCorners, tangent breaks stay where they
 * are and the points in between spread along each run. Poly and Bezier shapes stay within a small fraction of a span;
 * NURBS are refitted and lose their corners and weights.
 */
LILA_SCENE_API Spline evenSpacing(const Spline& s, bool keepCorners = true);

/** Largest distance from samples of a to the curve b, cm: how far an edit moved the shape. */
LILA_SCENE_API double shapeDeviation(const Spline& a, const Spline& b, int samplesPerSpan = 16);
} // namespace lila::curve
