#pragma once

#include "core/Export.h"
#include "curve/Spline.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

/**
 * Grooming: sculpt brushes for curve nodes holding many curves (hair, fur, grass) and the scalp they grow from. A
 * curve's first point is its root; brushes keep roots where they are, on the scalp when there is one, except Slide
 * (moves them along it) and Add (plants new ones on it). Works on points, so poly, Bezier and NURBS curves all comb.
 */
namespace lila::curve::groom
{
using D3 = glm::dvec3;

enum class Brush : uint8_t
{
    Comb,
    Smooth,
    Length,
    Puff,
    Add,
    Delete,
    Density,
    Cut,
    Pinch,
    Slide,
    Count
};

LILA_SCENE_API const char* brushName(Brush b);
LILA_SCENE_API bool parseBrush(std::string_view name, Brush* out);

/** The scalp: a mesh in the curves' object space, with smooth normals and an optional density map (a vertex color set). */
class LILA_SCENE_API Surface
{
public:
    explicit Surface(geo::Mesh mesh, std::string densitySet = {});

    struct Hit
    {
        D3 point{0.0};
        D3 normal{0.0, 1.0, 0.0};
        geo::Index triangle = geo::kInvalid;
        D3 bary{0.0};
    };

    bool empty() const { return mBvh.empty(); }
    std::optional<Hit> closest(const D3& p, double maxDistance = 1e300) const;
    std::optional<Hit> raycast(const D3& origin, const D3& dir) const;
    /** The density map at a hit, 0 to 1 (luminance of the color set); 1 without one. */
    double density(const Hit& h) const;
    const geo::Mesh& mesh() const { return mMesh; }
    const geo::MeshBvh& bvh() const { return mBvh; }

private:
    Hit hitAt(geo::Index triangle, const D3& bary, const D3& point) const;

    geo::Mesh mMesh;
    geo::MeshBvh mBvh;
    std::vector<D3> mNormals;
    std::string mDensitySet;
};

/** One brush stamp, in the curves' object space. */
struct Dab
{
    D3 center{0.0};
    /** The surface normal under the brush. */
    D3 normal{0.0, 1.0, 0.0};
    double radius = 1.0;
    /** Pressure and opacity, 0 to 1. */
    double strength = 1.0;
    /** How far the cursor moved since the last dab (comb, slide). */
    D3 delta{0.0};
    /** Into the screen: the brush reaches through it as a cylinder; zero makes it a sphere. */
    D3 viewDir{0.0};
};

struct Settings
{
    Brush brush = Brush::Comb;
    /** Weight at t = distance / radius; null is smooth. */
    std::function<double(double)> falloff;
    double strength = 0.5;
    /** Shrink, flatten, spread, remove: the brush's opposite. */
    bool invert = false;
    /** Comb, smooth, puff and pinch keep each segment's length (follow the leader from the root). */
    bool keepLength = true;
    /** Skip points behind the surface under the brush (the far side of a head). */
    bool frontOnly = true;

    /** Add: curves per dab, their length (cm) and points; shape from the nearest curves when interpolate is on. */
    int addCount = 5;
    double addLength = 5.0;
    int addPoints = 8;
    bool interpolate = true;
    int neighbours = 3;
    /** Add and density: closest two roots may be, cm; 0 is no limit for add, and from density for density. */
    double minDistance = 0.0;
    /** Density: target curves per square cm (scaled by the surface's density map). */
    double density = 1.0;

    /** Per spline, 1 where the brush may change it; empty for all. Kept in step as curves come and go. */
    std::vector<char> mask;
};

struct Stats
{
    int changed = 0;
    int added = 0;
    int removed = 0;
    std::vector<std::string> warnings;
};

/** Applies one dab. rng drives Add, Delete and Density, so a replay with the same seed gives the same result. */
LILA_SCENE_API void applyDab(Splines& s, Settings& o, const Dab& d, const Surface* surface, std::mt19937& rng, Stats* stats = nullptr);

/** Moves each curve so its root sits on the surface (closest point). Returns how many moved. */
LILA_SCENE_API int attachRoots(Splines& s, const Surface& surface);

/** Guide curves grown straight out of a surface along its normals, spread by area and the density map. */
struct GuideOptions
{
    int count = 200;
    double length = 10.0;
    /** Lengths vary by up to this fraction. */
    double lengthJitter = 0.0;
    int points = 8;
    /** Closest two roots may be, cm; 0 picks one from the area and count. */
    double minDistance = 0.0;
    uint32_t seed = 1;
};

LILA_SCENE_API Splines guides(const Surface& surface, const GuideOptions& o, std::vector<std::string>* warnings = nullptr);

/** Points of a curve resampled to n evenly spaced along its length (its control points for NURBS). */
LILA_SCENE_API std::vector<D3> resample(const std::vector<D3>& pts, int n);
} // namespace lila::curve::groom
