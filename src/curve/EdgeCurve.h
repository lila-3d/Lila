#pragma once

#include "core/Export.h"
#include "curve/Spline.h"
#include "geo/Attribute.h"

#include <span>

namespace lila::geo
{
class Mesh;
}

/** Mesh edges to curves: chains of edges become splines through their points. */
namespace lila::curve
{
struct EdgeCurveOptions
{
    Kind kind = Kind::Bezier;
    int degree = 3; // NURBS
    /** Fit within tolerance instead of one point per vertex. */
    bool smooth = false;
    double tolerance = 0.05; // cm
    /** Points turning more than this (radians) stay sharp: vector handles, a run break for fits. */
    double cornerAngle = 0.6981317; // 40 degrees
};

/** Indices of the points turning more than angle radians. Ends of an open chain never count. */
LILA_SCENE_API std::vector<size_t> turnCorners(std::span<const glm::dvec3> pts, bool closed, double angle);

/** A spline of the given kind through the points of an edge chain, corners kept. */
LILA_SCENE_API Spline splineThrough(std::span<const glm::dvec3> pts, bool closed, const EdgeCurveOptions& o);

/**
 * One spline per chain of the given edges (ids that are not valid edges are skipped): branching points end chains and
 * closed loops become closed splines. Points go through toWorld, then have origin subtracted.
 */
LILA_SCENE_API Splines edgeCurves(const geo::Mesh& mesh, const glm::dmat4& toWorld, std::span<const geo::Index> edges, const EdgeCurveOptions& o,
                                  const glm::dvec3& origin = glm::dvec3(0.0));
} // namespace lila::curve
