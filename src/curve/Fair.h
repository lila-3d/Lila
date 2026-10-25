#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

/** Fairing: evens out a curve's curvature (a smoother comb) without moving it further than a tolerance. */
namespace lila::curve
{
struct FairOptions
{
    /** Largest distance any part of the curve may move, cm. */
    double tolerance = 0.1;
    /** Keep the end tangents' directions (the second control point slides along them). Ends always stay put. */
    bool keepTangents = true;
    /** Points where the curve turns more than cornerAngle (radians) stay sharp and in place. */
    bool keepCorners = true;
    double cornerAngle = 0.5236;
    int iterations = 200;
};

struct FairResult
{
    Spline curve;
    /** Largest distance between the faired curve and the original, cm. */
    double maxDeviation = 0.0;
    /** Total curvature change along the curve (sum of |delta curvature| over even samples), before and after. */
    double variationBefore = 0.0, variationAfter = 0.0;
    int iterations = 0;
};

/**
 * Fairs the curve by smoothing its control polygon's third differences (the jerk that makes a comb ragged), each control
 * point kept within tolerance of where it was so the curve cannot move further than that. Iterates until nothing moves.
 * Same kind and point count; Bezier handles of smooth points come out aligned.
 */
LILA_SCENE_API FairResult fair(const Spline& s, const FairOptions& o = {});
/** The variation measure FairResult reports. */
LILA_SCENE_API double curvatureVariation(const Spline& s, int samples = 256);
} // namespace lila::curve
