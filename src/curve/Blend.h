#pragma once

#include "core/Export.h"
#include "curve/Spline.h"
#include "geo/Attribute.h"

#include <optional>

namespace lila::geo
{
class Mesh;
}

/**
 * Blend and match: a new curve bridging two curve ends (or picked points, or a mesh border), and moving a curve's end
 * onto another, each with exact positional (G0), tangent (G1), curvature (G2) or curvature-rate (G3) continuity.
 */
namespace lila::curve
{
enum class Continuity : uint8_t
{
    G0, // position
    G1, // tangent
    G2, // curvature
    G3, // rate of change of curvature
};
LILA_SCENE_API const char* continuityName(Continuity c);
LILA_SCENE_API bool parseContinuity(std::string_view name, Continuity* out);

/**
 * A curve's geometry where a blend or match meets it, by arc length heading away from it: the point, the unit tangent,
 * the curvature vector (second derivative, toward the centre of curvature) and its rate (third derivative).
 */
struct Geometry
{
    glm::dvec3 p{0.0};
    glm::dvec3 t{0.0};
    glm::dvec3 k{0.0};
    glm::dvec3 j{0.0};
};

/** Geometry at span parameter u heading toward larger u (forward) or smaller u. Zero tangent where the curve stalls. */
LILA_SCENE_API Geometry geometryAt(const Spline& s, double u, bool forward);
/** Geometry leaving an end of an open curve outward (atEnd false: its start). */
LILA_SCENE_API Geometry endGeometry(const Spline& s, bool atEnd);
/**
 * Geometry leaving a mesh across a border edge at fraction t along it: the tangent lies in the border face's plane,
 * square to the edge and away from the face; curvature follows the edge rings running into the mesh. Nullopt when the
 * edge is not a border.
 */
LILA_SCENE_API std::optional<Geometry> borderGeometry(const geo::Mesh& m, geo::Index edge, double t);

struct BlendEnd
{
    Geometry g;
    Continuity continuity = Continuity::G2;
    /** How far the blend runs along the tangent before turning, as a multiple of the gap; 1 is a balanced curve. */
    double bulge = 1.0;
};

/**
 * The blend from a to b: it leaves a.g.p along a.g.t and arrives at b.g.p heading against b.g.t, matching each end to its
 * continuity exactly. One Bezier span of degree ca + cb + 1 (a line for G0 at both, a Bezier for G1 at both, else NURBS).
 * Throws std::invalid_argument when the ends coincide or a needed tangent is missing.
 */
LILA_SCENE_API Spline blendCurve(const BlendEnd& a, const BlendEnd& b);

/** The same geometry travelled the other way. */
LILA_SCENE_API Geometry reversed(const Geometry& g);
/** g or reversed(g), whichever tangent is closer to dir. */
LILA_SCENE_API Geometry facing(const Geometry& g, const glm::dvec3& dir);

struct MatchOptions
{
    /** G0 to G2. */
    Continuity continuity = Continuity::G1;
    /** Leave the other end's position, tangent and curvature alone (spans are added when the curve is too short for that). */
    bool preserveOther = true;
    /** Meet the target heading the other way along it. */
    bool flip = false;
};

/**
 * Moves an end of an open curve onto target, changing only the control points that end's continuity needs. target is
 * the geometry the end runs on into: the curve leaves its end along target.t (onto another curve's end, that curve's
 * geometry heading into it). Poly curves become Beziers for G1 and up. Throws std::invalid_argument on a closed or
 * degenerate curve.
 */
LILA_SCENE_API Spline matchEnd(const Spline& s, bool atEnd, const Geometry& target, const MatchOptions& o = {});
/** Where two ends (each leaving its curve outward) would meet halfway: the average point, tangent and curvature, heading out of a. */
LILA_SCENE_API Geometry averageGeometry(const Geometry& a, const Geometry& b);
} // namespace lila::curve
