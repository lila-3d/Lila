#pragma once

#include "core/Export.h"
#include "curve/Spline.h"
#include "geo/CrossSection.h"

#include <span>
#include <string>

/** Slicing meshes into curves (Rhino's Contour and Section). */
namespace lila::curve
{
struct SliceOptions
{
    /** Largest distance from the cut a simplified point may leave, cm; 0 keeps every crossing. */
    double tolerance = 0.01;
    /** Fit smooth Beziers (corners kept) instead of poly lines. */
    bool smooth = false;
    /** Close open cuts (open meshes, gaps between meshes) across the gap, as crossSections does; off keeps them open. */
    bool bridge = false;
};

/**
 * One plane's cut as curves: loops wind counter-clockwise about the normal and start at their point furthest along the plane's
 * first in-plane axis, open chains run from the end lower along that axis, so stacked sections line up for lofting.
 * Consecutive duplicate points and loops too small to survive simplification are dropped.
 */
LILA_SCENE_API Splines sectionCurves(std::span<const geo::SectionMesh> meshes, const glm::dvec3& origin, const glm::dvec3& normal, const SliceOptions& o = {});

struct ContourOptions
{
    SliceOptions slice;
    /** Plane normal: the contours step along it. */
    glm::dvec3 axis{0.0, 1.0, 0.0};
    /** A point every plane is a whole number of spacings from (spacing mode). */
    glm::dvec3 origin{0.0};
    /** cm between planes, used when count is 0. */
    double spacing = 10.0;
    /** Planes spread evenly through the meshes' extent along the axis, its two ends left out. */
    int count = 0;
    /** More planes than this are refused with a warning (a spacing far smaller than the meshes). */
    int maxPlanes = 2000;
};

struct ContourResult
{
    Splines curves;
    /** Where each plane crosses the axis (signed distance from the world origin along it), one per plane cut. */
    std::vector<double> planes;
    std::vector<std::string> warnings;
};

/** Contours of the meshes: sectionCurves at evenly spaced parallel planes. */
LILA_SCENE_API ContourResult contour(std::span<const geo::SectionMesh> meshes, const ContourOptions& o);
} // namespace lila::curve
