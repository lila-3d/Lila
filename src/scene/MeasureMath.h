#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

/** Geometry behind the measure nodes and measureSelection. Everything is in scene units (cm) unless noted. */
namespace lila::scene::measure
{
struct Plane
{
    glm::dvec3 origin{0.0};
    glm::dvec3 normal{0, 1, 0};
    glm::dvec3 u{1, 0, 0}, v{0, 0, 1}; // in-plane basis, u along the points' longest spread
    double maxDeviation = 0.0;          // largest distance of a point from the plane
};

/** Least-squares plane (smallest principal axis). Throws std::invalid_argument for fewer than 3 or collinear points. */
LILA_SCENE_API Plane fitPlane(std::span<const glm::dvec3> pts);

struct Circle
{
    glm::dvec3 center{0.0};
    glm::dvec3 normal{0, 1, 0};
    double radius = 0.0;
    double rms = 0.0;        // root-mean-square radial residual
    double planarity = 0.0;  // largest distance of a point from the circle's plane
    double arcDegrees = 0.0; // 360 minus the largest angular gap between neighbouring points
};

/**
 * Best-fit circle through 3+ points: least-squares plane, then an algebraic (Kasa) fit in that plane refined by
 * Gauss-Newton on the geometric distance, so partial arcs fit as well as full rings. Throws std::invalid_argument for
 * fewer than 3 or collinear points.
 */
LILA_SCENE_API Circle fitCircle(std::span<const glm::dvec3> pts);

/** Angle at `vertex` between the arms to a and b, degrees. Throws std::invalid_argument for a zero-length arm. */
LILA_SCENE_API double angleDegrees(const glm::dvec3& a, const glm::dvec3& vertex, const glm::dvec3& b);

/** Index of the corner opposite the longest side: the angle's vertex when three points come in no particular order. */
LILA_SCENE_API int cornerOf(const std::array<glm::dvec3, 3>& pts);

/** Area of a planar or near-planar polygon (Newell). */
LILA_SCENE_API double polygonArea(std::span<const glm::dvec3> pts);
LILA_SCENE_API double polylineLength(std::span<const glm::dvec3> pts, bool closed);

/**
 * Display units for lengths. The general pref /units/linear picks one of mm, cm, m, km, in, ft, yd; scene units are
 * always cm.
 */
LILA_SCENE_API std::string displayUnit();
/** Scene units (cm) to one unit of `unit`, e.g. 0.01 for m. Unknown units give 1. */
LILA_SCENE_API double unitFactor(const std::string& unit);
/** Decimal places from the general pref /measure/precision. */
LILA_SCENE_API int displayPrecision();
/** "12.50 cm" in the display unit, or the bare number when withUnit is false. */
LILA_SCENE_API std::string formatLength(double cm, bool withUnit = true);
LILA_SCENE_API std::string formatArea(double cm2, bool withUnit = true);
LILA_SCENE_API std::string formatAngle(double degrees, bool withUnit = true);
} // namespace lila::scene::measure
