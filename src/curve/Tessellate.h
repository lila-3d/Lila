#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <functional>
#include <span>

namespace lila::curve
{
enum class TessMode : uint8_t
{
    Fixed,    // perSpan segments on every curved span, one on straight ones
    Adaptive, // subdivides until within chordError and maxAngle
    Game,     // one segment per straight span; curved spans get segments by turning angle, spaced by arc length
};

LILA_SCENE_API const char* tessModeName(TessMode m);
LILA_SCENE_API bool parseTessMode(std::string_view name, TessMode* out);

struct TessOptions
{
    TessMode mode = TessMode::Adaptive;
    int perSpan = 8;
    double chordError = 0.02;   // cm
    double maxAngle = 0.1396;   // radians between neighbouring segments (8 degrees)
    int maxPerSpan = 512;
};

/**
 * A curve as points. Span boundaries where the tangent breaks are always points, flagged as kinks. Closed curves don't
 * repeat the start; length includes the closing segment.
 */
struct LILA_SCENE_API Polyline
{
    std::vector<glm::dvec3> points;
    std::vector<glm::dvec3> tangentIn;  // unit, arriving at the point
    std::vector<glm::dvec3> tangentOut; // unit, leaving it; differs from tangentIn only at kinks
    std::vector<double> params;         // span parameter u of each point
    std::vector<double> arcLength;      // from the start
    std::vector<uint8_t> kink;
    bool closed = false;
    double length = 0.0;

    size_t size() const { return points.size(); }
    /** Average of the in and out tangents (the miter direction at a kink). */
    glm::dvec3 tangent(size_t i) const;
    /** Point at arc length s (wrapped on closed curves, clamped on open ones). */
    glm::dvec3 pointAtLength(double s) const;
    /** Index of the segment holding arc length s and the fraction along it. */
    size_t segmentAt(double s, double* frac) const;
};

/** Tessellates one spline. */
LILA_SCENE_API Polyline tessellate(const Spline& s, const TessOptions& o = {});
/** A polyline straight from points (tangents from the neighbours, every corner sharper than kinkAngle flagged). */
LILA_SCENE_API Polyline polylineFrom(std::span<const glm::dvec3> pts, bool closed, double kinkAngle = 0.5236);

/** Arc length of a spline, by adaptive quadrature over its pieces. */
LILA_SCENE_API double length(const Spline& s);
/** Span parameter at arc length s along the spline. */
LILA_SCENE_API double paramAtLength(const Spline& s, double len);
/** The span parameter of the curve point nearest p, and that point. */
LILA_SCENE_API double closestParam(const Spline& s, const glm::dvec3& p, glm::dvec3* at = nullptr);

// Frames

enum class FrameMode : uint8_t
{
    Rmf,           // rotation minimising (no twist), closed loops close up
    FixedUp,       // normal toward a fixed up vector
    SurfaceNormal, // normal from a surface under the curve
    Frenet,        // principal normal; where the curve is straight it carries the last one on
};

LILA_SCENE_API const char* frameModeName(FrameMode m);
LILA_SCENE_API bool parseFrameMode(std::string_view name, FrameMode* out);

/** Right handed: binormal = tangent x normal. */
struct Frame
{
    glm::dvec3 t{0.0, 0.0, 1.0};
    glm::dvec3 n{1.0, 0.0, 0.0};
    glm::dvec3 b{0.0, 1.0, 0.0};
};

struct FrameOptions
{
    FrameMode mode = FrameMode::Rmf;
    glm::dvec3 up{0.0, 1.0, 0.0};
    /** SurfaceNormal: unit surface normal at a world point (zero for none). */
    std::function<glm::dvec3(const glm::dvec3&)> surfaceNormal;
    /** First frame's normal; zero picks the curve's plane normal for planar curves, else up. */
    glm::dvec3 startNormal{0.0};
    /** Closed loops: spread the twist RMF leaves at the seam along the loop so the frames close up. */
    bool closeLoops = true;
    /** Extra whole turns around a closed loop (added to the closing twist). */
    int extraTurns = 0;
};

/** Frames at the polyline points, rotated by tilt (radians per point, may be empty). Uses the miter tangent at kinks. */
LILA_SCENE_API std::vector<Frame> frames(const Polyline& pl, const FrameOptions& o = {}, std::span<const double> tilt = {});
/** Tilt per polyline point, from the spline's tilt values. */
LILA_SCENE_API std::vector<double> tiltAlong(const Spline& s, const Polyline& pl);
/** Plane normal of a planar point set (unit), or zero when the points don't lie in one plane within tolerance. */
LILA_SCENE_API glm::dvec3 planeNormal(std::span<const glm::dvec3> pts, double tolerance = 1e-6);

// Curvature comb

struct CombTooth
{
    glm::dvec3 base{0.0};
    glm::dvec3 tip{0.0};
    double curvature = 0.0;
};
/** Curvature comb: per span `samples` teeth along the curvature normal, length curvature x scale. */
LILA_SCENE_API std::vector<CombTooth> curvatureComb(const Spline& s, int samples, double scale);
} // namespace lila::curve
