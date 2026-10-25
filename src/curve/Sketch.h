#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Sketch constraints: geometric constraints and dimensions between the splines of one curve node, solved in a plane.
 *
 * The solver sees each spline as points, or as one circle or arc when it is an exact rational quadratic NURBS that is
 * one. Points are poly vertices, Bezier anchors (their free handles ride along as offsets) and NURBS control points;
 * lines are the legs between consecutive points. Constraints refer to them by spline and point index, so they survive
 * edits that keep the point count. Everything solves in the plane's 2D coordinates; a point's height off the plane is
 * kept as it was.
 */
namespace lila::curve::sketch
{
using json = nlohmann::json;
using D2 = glm::dvec2;
using D3 = glm::dvec3;

/** What a constraint refers to. */
enum class RefKind : uint8_t
{
    Point,   // a point; index -1 is the spline's last point
    Line,    // the leg from point index to the next
    Curve,   // a whole spline: a circle or arc, or any curve for point on curve
    Center,  // a circle's or arc's centre
    HandleL, // a Bezier anchor's incoming handle
    HandleR, // its outgoing handle
};

struct LILA_SCENE_API Ref
{
    RefKind kind = RefKind::Point;
    int spline = 0;
    int index = 0;

    bool operator==(const Ref&) const = default;
    bool isPointLike() const { return kind == RefKind::Point || kind == RefKind::Center || kind == RefKind::HandleL || kind == RefKind::HandleR; }
};

/** "p0.3" point 3 of spline 0 ("p0.end" its last), "l0.3" the line from point 3, "c1" spline 1, "o1" its centre, "hl0.3"/"hr0.3" handles. */
LILA_SCENE_API std::string refName(const Ref& r);
LILA_SCENE_API bool parseRef(std::string_view text, Ref* out);

enum class Type : uint8_t
{
    Coincident,
    Horizontal,
    Vertical,
    Parallel,
    Perpendicular,
    Tangent,
    Equal,
    Concentric,
    Midpoint,
    Symmetric,
    Fixed,
    PointOnCurve,
    Collinear,
    // Dimensions: they carry a value.
    Distance,
    HorizontalDistance,
    VerticalDistance,
    Length,
    Radius,
    Diameter,
    Angle,
    Count
};

LILA_SCENE_API const char* typeName(Type t);
LILA_SCENE_API bool parseType(std::string_view name, Type* out);
inline bool isDimension(Type t) { return t >= Type::Distance && t < Type::Count; }

struct LILA_SCENE_API Constraint
{
    uint32_t id = 0;
    Type type = Type::Coincident;
    std::vector<Ref> refs;
    /** Dimensions: cm, or degrees for Angle. */
    double value = 0.0;
    /** Dimensions: an expression for the value (other dimensions by name, units, + - * / ^, sin, sqrt...); empty uses value. */
    std::string expr;
    /** Dimensions: the name expressions use, d1, d2... */
    std::string name;
    /** Which side or turn the geometry had when it was made: the solver keeps it there. */
    double sign = 1.0;
    /** Fixed: where the referenced points (circles: centre, then a point on the rim for the radius) are held, object space. */
    std::vector<D3> anchors;
    /** Tangent: which ends of the two curves meet (0 start, 1 end), -1 when they need not. */
    int endA = -1, endB = -1;

    json toJson() const;
    /** Throws std::invalid_argument on a malformed constraint. */
    static Constraint fromJson(const json& j);
};

/** The sketch plane: origin and two unit axes, horizontal (u) and vertical (v). */
struct LILA_SCENE_API Plane
{
    D3 origin{0.0};
    D3 u{1.0, 0.0, 0.0};
    D3 v{0.0, 0.0, -1.0};

    D3 normal() const { return glm::normalize(glm::cross(u, v)); }
    D2 to2d(const D3& p) const { return {glm::dot(p - origin, u), glm::dot(p - origin, v)}; }
    double height(const D3& p) const { return glm::dot(p - origin, normal()); }
    D3 to3d(const D2& p, double h = 0.0) const { return origin + u * p.x + v * p.y + normal() * h; }
};

/** A plane with this normal through origin, its u axis as close to world X (or Z when the normal is X) as it can be. */
LILA_SCENE_API Plane planeFrom(const D3& origin, const D3& normal);
/** The best-fit plane of the splines' points; fallbackNormal when they are collinear or empty. */
LILA_SCENE_API Plane fitPlane(const Splines& s, const D3& fallbackNormal);

/** Everything a curve node stores about its sketch. */
struct LILA_SCENE_API Sketch
{
    Plane plane;
    std::vector<Constraint> constraints;
    uint32_t nextId = 1;
    /** The next free dimension name. */
    int nextName = 1;

    bool empty() const { return constraints.empty(); }
    const Constraint* find(uint32_t id) const;
    /** Gives c an id (and a dimension a name) and appends it. Returns the id. */
    uint32_t add(Constraint c);
    bool remove(uint32_t id);

    json toJson() const;
    /** What the curve's sketch attr stores. */
    std::string toText() const;
    /** Throws std::invalid_argument on malformed input. Empty text is an empty sketch. */
    static Sketch fromJson(const json& j);
    static Sketch fromText(std::string_view text);
};

/** How the solver sees one spline. */
enum class Shape : uint8_t
{
    Points,
    Circle,
    Arc,
};

/** A circle or arc found in a spline, in plane coordinates. Angles in radians from u toward v; sweep is signed. */
struct CircleFit
{
    Shape shape = Shape::Points;
    D2 center{0.0};
    double radius = 0.0;
    double start = 0.0;
    double sweep = 0.0;
    double height = 0.0;
};

/** The circle or arc a spline is (an exact rational quadratic NURBS lying in a plane parallel to pl), Points otherwise. */
LILA_SCENE_API CircleFit circleOf(const Spline& s, const Plane& pl);

/** True when the ref names something the splines have. why says what is wrong. */
LILA_SCENE_API bool refValid(const Splines& s, const Plane& pl, const Ref& r, std::string* why = nullptr);
/** The ref with index -1 resolved to the last point, and the last point of a closed spline's wrap folded in. */
LILA_SCENE_API Ref normalized(const Splines& s, const Ref& r);
/** Where a point-like ref is, object space. Lines give their midpoint, curves a point on them. */
LILA_SCENE_API D3 refPosition(const Splines& s, const Plane& pl, const Ref& r);

/** Checks the refs fit the type (counts and kinds) and fills what the constraint keeps from the geometry: sign, anchors, ends. */
LILA_SCENE_API void prepare(Constraint& c, const Splines& s, const Plane& pl);
/** The current value a dimension would measure (cm, or degrees). */
LILA_SCENE_API double measure(const Constraint& c, const Splines& s, const Plane& pl);

/** Evaluates a dimension expression. names are other dimensions' values. nullopt with why on a syntax error or unknown name. */
LILA_SCENE_API std::optional<double> evalExpression(std::string_view text, const std::map<std::string, double>& names, std::string* why = nullptr);
/** Every dimension's value with expressions resolved in dependency order. Throws std::invalid_argument on a cycle or bad expression. */
LILA_SCENE_API std::map<uint32_t, double> dimensionValues(const Sketch& k);

/** A point dragged to a target while the solve keeps the constraints. A circle or arc (Curve) dragged takes its radius to it. */
struct Drag
{
    Ref point;
    D3 to{0.0}; // object space
};

struct SolveOptions
{
    std::vector<Drag> drags;
    int maxIterations = 60;
    /** Look for which constraints conflict when the solve fails (a solve per constraint in the failing group). */
    bool findConflicts = true;
};

struct LILA_SCENE_API SolveResult
{
    bool ok = true;
    /** Largest constraint error left, cm. */
    double error = 0.0;
    int iterations = 0;
    /** Constraints that cannot all hold: removing any one of these lets the rest solve. */
    std::vector<uint32_t> conflicting;
    /** Constraints referring to geometry the splines no longer have; skipped. */
    std::vector<uint32_t> invalid;
    std::vector<std::string> warnings;
};

/** Solves the splines in place so every constraint holds, moving them as little as it can. */
LILA_SCENE_API SolveResult solve(Splines& s, const Sketch& k, const SolveOptions& o = {});

/** Degrees of freedom: what the constraints pin down. */
struct LILA_SCENE_API Status
{
    int dof = 0;
    /** Per spline, per point: the point (and its handles) cannot move without breaking a constraint. */
    std::vector<std::vector<bool>> pointFixed;
    /** Per spline: every part of it is pinned (a circle's centre and radius, an arc's ends too). */
    std::vector<bool> splineFixed;
    /** Constraints already implied by the others (harmless while consistent). */
    std::vector<uint32_t> redundant;

    bool lineFixed(size_t spline, size_t index) const;
    bool pointIsFixed(size_t spline, size_t index) const;
};

LILA_SCENE_API Status analyze(const Splines& s, const Sketch& k);

/** What adding c would do: solve result, and whether it adds nothing (already implied). */
struct AddCheck
{
    SolveResult result;
    bool redundant = false;
};

/** Solves s with c added (c prepared, id set), without changing s. */
LILA_SCENE_API AddCheck checkAdd(const Splines& s, const Sketch& k, const Constraint& c);

/**
 * Auto constraints for a point being drawn at `at` (plane coordinates). It snaps to existing points and circle centres
 * (coincident), onto lines and curves (point on curve), and with a previous point to horizontal and vertical.
 * Constraint refs to the point being drawn use spline kNew, index 0; to the segment from `previous` to it, spline kNew,
 * kind Line.
 */
inline constexpr int kNew = -100;

struct Inference
{
    D2 at{0.0};
    std::vector<Constraint> constraints;
};

LILA_SCENE_API Inference infer(const Splines& s, const Plane& pl, D2 at, const std::optional<D2>& previous, double tolerance,
                               const std::vector<Ref>& ignore = {});
} // namespace lila::curve::sketch
