#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * Curves: one spline type with a per-curve kind (poly, Bezier, NURBS), and the math every curve tool shares (evaluation,
 * tessellation, frames, fitting, corners, offsets, projection, sweeps). Nothing here knows about the scene or Qt.
 *
 * Every spline is parameterised by span: u in [0, spanCount], span floor(u), local t = u - span. Poly and Bezier spans
 * are their segments; NURBS spans are the non-empty knot intervals.
 */
namespace lila::curve
{
enum class Kind : uint8_t
{
    Poly,
    Bezier,
    Nurbs,
};

/** Bezier handle behaviour, per side of each point. */
enum class HandleType : uint8_t
{
    Auto,    // smooth, recomputed from the neighbours whenever they move
    Vector,  // points a third of the way to its neighbour: a straight segment and a corner
    Aligned, // keeps its length, stays opposite the other aligned handle
    Free,    // independent: breaks the tangent
};

LILA_SCENE_API const char* kindName(Kind k);
LILA_SCENE_API bool parseKind(std::string_view name, Kind* out);
LILA_SCENE_API const char* handleTypeName(HandleType t);
LILA_SCENE_API bool parseHandleType(std::string_view name, HandleType* out);

/**
 * One curve. Optional per-point arrays are either empty (their default everywhere) or one value per point.
 *
 * NURBS knots: open curves take the full vector (points + degree + 1 values, empty for clamped uniform); closed curves are
 * periodic and take points + 1 breakpoints, one per span start plus the end of the period (empty for uniform), where a
 * repeated breakpoint raises that knot's multiplicity.
 */
struct LILA_SCENE_API Spline
{
    Kind kind = Kind::Poly;
    bool closed = false;
    int degree = 3; // NURBS only, 1 to 7

    std::vector<glm::dvec3> points; // poly vertices, Bezier anchors or NURBS control points

    std::vector<glm::dvec3> handleL, handleR; // Bezier: absolute positions of each anchor's incoming and outgoing handle
    std::vector<HandleType> typeL, typeR;

    std::vector<double> weights; // NURBS
    std::vector<double> knots;   // NURBS

    std::vector<double> tilt;         // radians around the tangent, default 0
    std::vector<double> radius;       // sweep scale, default 1
    std::vector<double> cornerRadius; // fillet radius at the point, cm, default 0 (none)

    size_t size() const { return points.size(); }
    double weight(size_t i) const { return i < weights.size() ? weights[i] : 1.0; }
    bool rational() const;
    /** Fills Bezier handle arrays (vector handles toward the neighbours) and drops arrays of the wrong length. */
    void normalize();
    /** Removes point i and its per-point values. */
    void erasePoint(size_t i);
    /** Inserts a point before index i (i == size() appends), its per-point values copied from its neighbour. */
    void insertPoint(size_t i, const glm::dvec3& p);
};

using Splines = std::vector<Spline>;

/** Throws std::invalid_argument naming what is wrong: too few points, a bad degree, knots of the wrong length or decreasing. */
LILA_SCENE_API void validate(const Spline& s);
LILA_SCENE_API bool isValid(const Spline& s, std::string* why = nullptr);

/** Number of spans (see the namespace doc). 0 for a spline too short to evaluate. */
LILA_SCENE_API int spanCount(const Spline& s);

/** A point and its first two derivatives with respect to the span's local t. */
struct Sample
{
    glm::dvec3 p{0.0};
    glm::dvec3 d1{0.0};
    glm::dvec3 d2{0.0};
};

/** One span as a rational Bezier: homogeneous control points (w x, w y, w z, w). Degree is cv.size() - 1. */
struct Piece
{
    std::vector<glm::dvec4> cv;
    int span = 0;

    int degree() const { return int(cv.size()) - 1; }
    glm::dvec3 start() const { return glm::dvec3(cv.front()) / cv.front().w; }
    glm::dvec3 end() const { return glm::dvec3(cv.back()) / cv.back().w; }
    glm::dvec3 point(size_t i) const { return glm::dvec3(cv[i]) / cv[i].w; }
    bool rational() const;
};

/** Every span as a Bezier piece, in order. Spans of a closed spline include the one back to its start. */
LILA_SCENE_API std::vector<Piece> pieces(const Spline& s);

LILA_SCENE_API Sample evaluate(const Piece& p, double t);
LILA_SCENE_API Sample evaluate(const Spline& s, double u);
LILA_SCENE_API glm::dvec3 pointAt(const Spline& s, double u);
/** Unit tangent leaving the piece's start (end=false) or arriving at its end, robust to handles that sit on their anchor. */
LILA_SCENE_API glm::dvec3 endTangent(const Piece& p, bool end);
/** de Casteljau split at t. */
LILA_SCENE_API void split(const Piece& p, double t, Piece& left, Piece& right);
/** The part of the piece between t0 and t1. */
LILA_SCENE_API Piece subPiece(const Piece& p, double t0, double t1);
/** Same curve at a higher degree. */
LILA_SCENE_API Piece elevate(const Piece& p, int degree);
/** True when every control point lies on the chord, so the piece is a straight segment. */
LILA_SCENE_API bool isStraight(const Piece& p, double tolerance = 1e-7);
/** Curvature (1/cm) of a sample. */
LILA_SCENE_API double curvature(const Sample& s);

// NURBS

/** The full knot vector the spline evaluates with; for a closed spline the periodic one over its unrolled control points. */
LILA_SCENE_API std::vector<double> knotVector(const Spline& s);
/** Control points in homogeneous form, with a closed spline's first degree points repeated at the end. */
LILA_SCENE_API std::vector<glm::dvec4> controlPoints(const Spline& s);
/** Knot-vector parameter of span u, and back. */
LILA_SCENE_API double knotParam(const Spline& s, double u);
LILA_SCENE_API double spanParam(const Spline& s, double knot);
/** Edit points: where the curve passes each span boundary (u = 0, 1, ..., spanCount). Closed splines don't repeat the start. */
LILA_SCENE_API std::vector<glm::dvec3> editPoints(const Spline& s);

/** Inserts knot u (span parameter) `times` times without changing the shape (Boehm). NURBS only; others are converted first. */
LILA_SCENE_API void insertKnot(Spline& s, double u, int times = 1);

// Conversion

/** The same shape as a NURBS: poly as degree 1, Bezier as degree 3 with a triple knot at every anchor. Exact. */
LILA_SCENE_API Spline toNurbs(const Spline& s);
/** A Bezier through the same shape. Exact for poly and non-rational NURBS up to degree 3, fitted within tolerance otherwise. */
LILA_SCENE_API Spline toBezier(const Spline& s, double tolerance = 0.01);
/** Rebuilds a spline from Bezier pieces joined end to end. Pieces of lower degree are elevated. Kind preferred decides
 *  the result where the pieces allow it: Bezier when every piece is a non-rational cubic or less, poly when all are lines,
 *  otherwise NURBS with a full-multiplicity knot between pieces. */
LILA_SCENE_API Spline fromPieces(const std::vector<Piece>& pieces, bool closed, Kind preferred);
/** Same spline with a different kind (conversion as above; poly takes the tessellated points). */
LILA_SCENE_API Spline convert(const Spline& s, Kind kind, double tolerance = 0.01);

// Construction

/** Exact circle: a closed rational quadratic NURBS (eight points, double knots) in the plane through center with this normal. */
LILA_SCENE_API Spline circle(const glm::dvec3& center, const glm::dvec3& normal, double radius);
/** Exact arc from start around the axis through center by angle radians (rational quadratic, one piece per 90 degrees or less). */
LILA_SCENE_API Spline arc(const glm::dvec3& center, const glm::dvec3& axis, const glm::dvec3& start, double angle);
/** Arc pieces as rational quadratics (for building splines piece by piece). */
LILA_SCENE_API std::vector<Piece> arcPieces(const glm::dvec3& center, const glm::dvec3& axis, const glm::dvec3& start, double angle);
LILA_SCENE_API Piece linePiece(const glm::dvec3& a, const glm::dvec3& b);

// Bezier handles

/**
 * Recomputes Auto and Vector handles from the anchors and re-aligns Aligned pairs. edited lists handles moved by hand
 * (point index, side 0 left / 1 right): an edited Auto handle becomes Aligned and an edited Vector one Free, and its
 * Aligned partner turns to stay opposite it.
 */
LILA_SCENE_API void updateHandles(Spline& s, const std::vector<std::pair<size_t, int>>& edited = {});
/** Sets both handle types of point i and recomputes. */
LILA_SCENE_API void setHandleType(Spline& s, size_t i, HandleType t);

// Whole-spline edits

LILA_SCENE_API void transform(Spline& s, const glm::dmat4& m);
/** Same shape, traversed the other way. */
LILA_SCENE_API void reverse(Spline& s);
/** Bounding box of the control points (handles included). */
LILA_SCENE_API void bounds(const Spline& s, glm::dvec3& lo, glm::dvec3& hi);

/** Value of a per-point array at span parameter u, linear between the points it belongs to. def when the array is empty. */
LILA_SCENE_API double pointValueAt(const Spline& s, const std::vector<double>& values, double def, double u);
} // namespace lila::curve
