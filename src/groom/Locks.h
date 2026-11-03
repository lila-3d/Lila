#pragma once

#include "core/Export.h"
#include "geo/Mesh.h"
#include "groom/Groom.h"

#include <functional>
#include <string>
#include <vector>

/**
 * Locks: a mesh swept along each guide with a cross-section profile, width, thickness and twist ramps from root to
 * tip, lying flat to the scalp near it. The stylized hair building block; strays and sub-locks are locks too.
 */
namespace lila::groom
{
enum class LockProfile : uint8_t
{
    Tube,     // round
    Ribbon,   // flat ellipse
    Crescent, // bowed band, thin at its edges
    Teardrop, // round on one side, pointed on the other
    Custom,   // LockOptions::customProfile
    Count
};

LILA_SCENE_API const char* profileName(LockProfile p);
LILA_SCENE_API bool parseProfile(std::string_view name, LockProfile* out);
/** Closed cross-section, counter-clockwise, x along the lock's width and y through its thickness, both within -1..1. */
LILA_SCENE_API std::vector<glm::dvec2> profilePoints(LockProfile p, int sides, const std::vector<glm::dvec2>& custom = {});

using Ramp = std::function<double(double)>;

struct LockOptions
{
    /** Width at full ramp, cm, times each curve's width attr and per-point radius. */
    double width = 2.0;
    /** Thickness as a fraction of width (1 is round), times each curve's thickness attr. */
    double thickness = 0.35;
    LockProfile profile = LockProfile::Ribbon;
    std::vector<glm::dvec2> customProfile;
    /** t 0 at the root to 1 at the tip; null is 1 everywhere. */
    Ramp widthRamp;
    Ramp thicknessRamp;
    /** Twist from root to tip, radians, shaped by twistRamp (null: linear); each point's tilt adds to it. */
    double twist = 0.0;
    Ramp twistRamp;
    /** How strongly a lock's flat side turns to face the scalp where it lies close to it, 0 to 1. */
    double flatten = 1.0;
    /** Push points out so the lock rests on the scalp instead of sinking into it; lift adds a gap, cm. */
    bool rest = true;
    double lift = 0.0;
    /** Width of strays (curves with the stray attr), cm. */
    double strayWidth = 0.08;
    /** Rings along each lock; 0 follows the curve's length (one ring per segmentLength). */
    int segments = 0;
    double segmentLength = 0.75;
    /** Points around the profile. */
    int sides = 10;
    bool caps = true;
    /** Atlas cells, picked per lock by its id. */
    int columns = 1;
    int rows = 1;
    uint32_t seed = 1;
};

/** Frames along a resolved lock: tangent, side (across the width) and the thickness axis, right handed (side, thick, tangent). */
struct Frames
{
    std::vector<D3> tangent, side, thick;
};

/** Rotation minimising frames, turned to lie flat on the scalp near it by flatten and twisted by the curve's tilt. */
LILA_SCENE_API Frames lockFrames(const Spline& s, const Surface* scalp, double flatten, double twist = 0.0, const Ramp& twistRamp = {});

/**
 * The locks the curves sweep: resampled polys with halfWidth, halfThick, side and v per point (groom/Attrs.h), roots on
 * the scalp, resting on it. What the lock mesh, the shell and the settle solver's collision radii all read.
 */
LILA_SCENE_API Splines resolveLocks(const Splines& curves, const Surface* scalp, const LockOptions& o, std::vector<std::string>* warnings = nullptr);

/** Collision radius per point of each curve (half its thickness, strays their own width), for the settle solver. */
LILA_SCENE_API std::vector<std::vector<double>> collisionRadii(const Splines& curves, const LockOptions& o);

struct LockMesh
{
    geo::Mesh mesh;
    int locks = 0;
};

/** The swept mesh of resolved locks: quads, caps as n-gons, uv0 (u around, v root to tip in the lock's atlas cell), face attr lockId. */
LILA_SCENE_API LockMesh lockMesh(const Splines& resolved, const LockOptions& o);
} // namespace lila::groom
