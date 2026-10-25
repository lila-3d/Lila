#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <optional>

/**
 * Curve to curve intersections and what is built on them: trim (delete the stretch between two intersections), split
 * at every intersection, and extend an end until it meets another curve.
 */
namespace lila::curve
{
struct IntersectOptions
{
    /** Curves closer than this meet, cm. */
    double tolerance = 0.01;
    /** Nonzero: intersect as seen along this direction (a view), so curves that only cross on screen meet. */
    glm::dvec3 direction{0.0};
};

/** Where two curves meet: span parameters on each and the points there (they differ along direction when projected). */
struct CurveHit
{
    double ua = 0.0, ub = 0.0;
    glm::dvec3 pa{0.0}, pb{0.0};
};

/** Every place a meets b, sorted along a. Touching and crossing both count; a stretch where they run together gives its ends. */
LILA_SCENE_API std::vector<CurveHit> intersect(const Spline& a, const Spline& b, const IntersectOptions& o = {});
/** Span parameters of s where it meets any of the others, sorted and without repeats. */
LILA_SCENE_API std::vector<double> intersectionParams(const Spline& s, const Splines& others, const IntersectOptions& o = {});

/** The part of s from u0 to u1 (span parameters). On a closed curve u1 below u0 wraps through the start. Same kind where it can. */
LILA_SCENE_API Spline subCurve(const Spline& s, double u0, double u1);

/** The stretch of s between two cuts, as subCurve takes it: from u0 to u1, wrapping on a closed curve when u1 <= u0. */
struct CurveInterval
{
    double u0 = 0.0, u1 = 0.0;
};
/** The stretch holding u between the cuts around it (the curve's ends count as cuts on an open curve). Nullopt for a closed curve with fewer than two cuts. */
LILA_SCENE_API std::optional<CurveInterval> intervalAt(const Spline& s, const std::vector<double>& cuts, double u);
/** What is left of s after deleting the stretch holding u: up to two curves (one for a closed curve). */
LILA_SCENE_API Splines trimAt(const Spline& s, const std::vector<double>& cuts, double u);
/** s cut at every cut: open curves into cuts + 1 pieces, closed ones into as many pieces as cuts. */
LILA_SCENE_API Splines splitAt(const Spline& s, std::vector<double> cuts);

enum class ExtendMode : uint8_t
{
    Line,   // straight on along the end tangent
    Arc,    // a circular arc with the end's curvature
    Smooth, // the end span carried on (its polynomial continued)
};
LILA_SCENE_API const char* extendModeName(ExtendMode m);
LILA_SCENE_API bool parseExtendMode(std::string_view name, ExtendMode* out);

/** The extension leaving an end of an open curve (atEnd false: its start), length cm long, starting exactly on the end. */
LILA_SCENE_API std::optional<Spline> extension(const Spline& s, bool atEnd, ExtendMode mode, double length);

struct ExtendOptions
{
    ExtendMode mode = ExtendMode::Smooth;
    /** How far to look for a boundary, cm; 0 is ten times the size of the curves involved. */
    double maxLength = 0.0;
    IntersectOptions intersect;
};

struct ExtendResult
{
    Spline curve;
    /** False when no boundary was in reach (curve is then s unchanged). */
    bool hit = false;
    /** Length added, cm. */
    double added = 0.0;
};
/** Extends an end of an open curve to the nearest boundary curve its extension meets. */
LILA_SCENE_API ExtendResult extendTo(const Spline& s, bool atEnd, const Splines& boundaries, const ExtendOptions& o = {});
/** Extends an end by an exact length. */
LILA_SCENE_API Spline extendBy(const Spline& s, bool atEnd, ExtendMode mode, double length);
} // namespace lila::curve
