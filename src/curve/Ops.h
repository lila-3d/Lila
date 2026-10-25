#pragma once

#include "core/Export.h"
#include "curve/Spline.h"
#include "curve/Tessellate.h"

#include <optional>

namespace lila::geo
{
class Mesh;
class MeshBvh;
} // namespace lila::geo

/** Curve to curve operations: corners, offsets, projection onto meshes, and joining, splitting, opening and closing. */
namespace lila::curve
{
enum class CornerMode : uint8_t
{
    Arc,     // circular arc, exact (a rational quadratic) between straight neighbours
    Chamfer, // straight cut
    G2,      // curvature-continuous quintic blend; radius is its tightest radius
};
LILA_SCENE_API const char* cornerModeName(CornerMode m);
LILA_SCENE_API bool parseCornerMode(std::string_view name, CornerMode* out);

struct FilletOptions
{
    CornerMode mode = CornerMode::Arc;
    /** Radius for corners without their own cornerRadius (cm). 0 leaves them. */
    double radius = 1.0;
    /** Use each point's cornerRadius where it is set (above 0). */
    bool perPoint = true;
    /** Only these corners (point indices of poly/Bezier anchors); empty is every corner. */
    std::vector<size_t> only;
    /** Corners turning less than this (radians) are left alone. */
    double minAngle = 0.0175;
    /** Arcs as rational quadratics; false approximates them with cubics so a Bezier stays a Bezier. */
    bool exactArcs = true;
};
/** Rounds, cuts or blends the curve's corners (tangent breaks). Radii larger than the neighbouring spans allow are clamped. */
LILA_SCENE_API Spline fillet(const Spline& s, const FilletOptions& o);

enum class JoinType : uint8_t
{
    Round,
    Miter,
    Square,
};
LILA_SCENE_API const char* joinTypeName(JoinType j);
LILA_SCENE_API bool parseJoinType(std::string_view name, JoinType* out);

struct OffsetOptions
{
    /** cm; positive is outward for closed curves (counter-clockwise about the plane normal) and to the left for open ones. */
    double distance = 1.0;
    JoinType join = JoinType::Round;
    double miterLimit = 4.0;
    /** The offset plane's normal; zero takes the curve's own plane, or up when it has none. */
    glm::dvec3 normal{0.0};
    /** Fitting tolerance for smooth curves, cm. */
    double tolerance = 0.01;
};
/** Offsets a curve within its plane. Loops and cusps the offset makes are trimmed, so the result may be several curves. */
LILA_SCENE_API Splines offset(const Spline& s, const OffsetOptions& o);

struct ProjectOptions
{
    /** Along direction (a view direction); zero projects each point to the closest surface point. */
    glm::dvec3 direction{0.0};
    /** Lift off the surface along its normal, cm. */
    double offset = 0.0;
    /** Sampling and refit tolerance, cm. */
    double tolerance = 0.01;
};
/**
 * Projects a curve onto a mesh (both in the same space; bvh built from mesh). Where projection misses the surface the curve
 * breaks, so the result may be several curves. Poly curves stay poly; others come back as Beziers (NURBS as NURBS).
 */
LILA_SCENE_API Splines project(const Spline& s, const geo::Mesh& mesh, const geo::MeshBvh& bvh, const ProjectOptions& o);

/** Which ends attach meets: a's end or start to b's start or end; Nearest picks the closest pair. */
enum class AttachEnds : uint8_t
{
    Nearest,
    EndStart,
};
/**
 * Joins b onto a. Ends closer than tolerance merge; others are joined by a straight span, or with blend by a tangent
 * continuous cubic. The result keeps the kind both share (else NURBS).
 */
LILA_SCENE_API Spline attach(const Spline& a, const Spline& b, bool blend, AttachEnds ends = AttachEnds::Nearest, double tolerance = 1e-4);
/** Splits at span parameter u: an open curve into two, a closed one into one open curve starting and ending there. */
LILA_SCENE_API Splines detach(const Spline& s, double u);
/** Closes an open curve: the ends merge when they meet, else a straight span (or with blend a smooth one) joins them. */
LILA_SCENE_API Spline closeCurve(const Spline& s, bool blend);
/** Opens a closed curve at span parameter u (default its start). Same shape. */
LILA_SCENE_API Spline openCurve(const Spline& s, double u = 0.0);
/** Inserts a point at span parameter u without changing the shape (a Bezier anchor, poly vertex or NURBS knot). Returns its point index. */
LILA_SCENE_API size_t insertPointAt(Spline& s, double u);
/** Moves each end of an open curve to `to` (nullopt leaves it), carrying its handle or neighbouring control point along. */
LILA_SCENE_API void moveEnds(Spline& s, const std::optional<glm::dvec3>& start, const std::optional<glm::dvec3>& end);
} // namespace lila::curve
