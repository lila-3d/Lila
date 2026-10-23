#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <span>
#include <utility>
#include <vector>

/**
 * Deformer math, free of the scene graph: every function takes plain points and returns new ones, runs in parallel
 * where it pays and gives the same result for the same input on any thread count.
 */
namespace lila::deform
{
using geo::Index;

/** Point neighbours over edges, CSR: point p's neighbours are neighbors[offsets[p] .. offsets[p + 1]). */
struct Adjacency
{
    std::vector<uint32_t> offsets{0};
    std::vector<uint32_t> neighbors;
    std::vector<char> boundary; // per point: on an open border
};

LILA_SCENE_API Adjacency pointAdjacency(const geo::Mesh& mesh);

/** Uniform Laplacian smoothing, Jacobi style so the order of points never matters. Boundary points stay put when pinned. */
LILA_SCENE_API void smooth(std::vector<glm::dvec3>& points, const Adjacency& adj, int iterations, double step, bool pinBoundary);

// ------------------------------------------------------------------------------------------------ falloff

/** 1 inside radius * (1 - softness), easing to 0 at radius. A radius of 0 or less means no falloff (always 1). */
LILA_SCENE_API double sphericalFalloff(double distance, double radius, double softness);

// ------------------------------------------------------------------------------------------------ nonlinear

/** Parameters of the nonlinear deformers, in handle space: the deformation axis is +Y, bounds are along it. */
struct BendParams
{
    double angle = 90.0; // degrees bent across [low, high]
    double low = -1.0, high = 1.0;
};
struct TwistParams
{
    double startAngle = 0.0, endAngle = 90.0; // degrees at low and high
    double low = -1.0, high = 1.0;
};
struct TaperParams
{
    double startScale = 1.0, endScale = 0.5; // at low and high
    double curve = 0.0;                      // bulge added in the middle, scale units
    double low = -1.0, high = 1.0;
};
struct SquashParams
{
    double factor = -0.5; // stretch along Y: -0.5 halves the length
    double expand = 1.0;  // how much the sides keep the volume, 0 none
    double maxExpandPos = 0.5;
    double low = -1.0, high = 1.0;
};
struct SineParams
{
    double amplitude = 0.2, wavelength = 1.0, offset = 0.0, dropoff = 0.0;
    double low = -1.0, high = 1.0;
};
struct WaveParams
{
    double amplitude = 0.1, wavelength = 0.5, offset = 0.0, dropoff = 0.0;
    double minRadius = 0.0, maxRadius = 1.0;
};

LILA_SCENE_API glm::dvec3 bend(const glm::dvec3& p, const BendParams& o);
LILA_SCENE_API glm::dvec3 twist(const glm::dvec3& p, const TwistParams& o);
LILA_SCENE_API glm::dvec3 taper(const glm::dvec3& p, const TaperParams& o);
LILA_SCENE_API glm::dvec3 squash(const glm::dvec3& p, const SquashParams& o);
LILA_SCENE_API glm::dvec3 sine(const glm::dvec3& p, const SineParams& o);
LILA_SCENE_API glm::dvec3 wave(const glm::dvec3& p, const WaveParams& o);

// ------------------------------------------------------------------------------------------------ lattice

enum class FfdBasis : uint8_t
{
    Trilinear,
    Bernstein, // Sederberg-Parry: global, every point moves everything
    BSpline,   // cubic, local: a control point reaches two cells each way
};

/** Lattice point (i, j, k) is index i + n.x * (j + n.y * k); at rest it sits at -0.5 + i / (n.x - 1) and so on. */
inline size_t latticeIndex(const glm::ivec3& n, int i, int j, int k) { return size_t(i) + size_t(n.x) * (size_t(j) + size_t(n.y) * size_t(k)); }
LILA_SCENE_API glm::dvec3 latticeRest(const glm::ivec3& n, int i, int j, int k);

/** The lattice as a mesh: points in latticeIndex order, a quad for every cell face. Divisions are points per axis, at least 2. */
LILA_SCENE_API geo::Mesh latticeMesh(const glm::ivec3& divisions);

/**
 * Displacement at lattice coordinates stu (each 0..1) from per-point displacements (current - rest). Displacement form
 * keeps the rest lattice an exact identity for every basis.
 */
LILA_SCENE_API glm::dvec3 ffdDisplacement(const glm::ivec3& n, std::span<const glm::dvec3> displacement, const glm::dvec3& stu, FfdBasis basis);

/** Weights of one axis: (point index, weight) pairs summing to 1. */
LILA_SCENE_API void ffdAxisWeights(int n, double s, FfdBasis basis, std::vector<std::pair<int, double>>& out);

// ------------------------------------------------------------------------------------------------ delta mush

/** Rest deltas of every point in its smoothed rest frame. */
struct DeltaMushBind
{
    int iterations = 0;
    double step = 0.0;
    bool pinBoundary = true;
    std::vector<glm::dvec3> local;
};

LILA_SCENE_API DeltaMushBind deltaMushBind(const geo::Mesh& topology, std::span<const glm::dvec3> rest, const Adjacency& adj, int iterations, double step,
                            bool pinBoundary);

/** Smooths the deformed points and puts the rest detail back on in the smoothed frames, scaled by displacement. */
LILA_SCENE_API std::vector<glm::dvec3> deltaMushApply(const geo::Mesh& topology, std::span<const glm::dvec3> deformed, const Adjacency& adj,
                                       const DeltaMushBind& bind, double displacement);

/** Orthonormal frame per point from smoothed points: x toward the first neighbour, z along the area-weighted normal. */
LILA_SCENE_API std::vector<glm::dmat3> pointFrames(const geo::Mesh& topology, std::span<const glm::dvec3> points, const Adjacency& adj);

// ------------------------------------------------------------------------------------------------ wrap

/** How one target point follows a driver: up to k driver triangles, each with a frame offset and a weight. */
struct WrapBinding
{
    struct Influence
    {
        uint32_t tri[3];
        glm::dvec3 bary;
        glm::dvec3 offset; // in the triangle's frame
        double weight;
    };
    std::vector<uint32_t> offsets{0};
    std::vector<Influence> influences;
    uint32_t driverPoints = 0;
};

/**
 * Binds targets to the driver's k nearest triangles. Triangles within falloff times the nearest distance count, weighted
 * by inverse squared distance. Both point sets in the same space.
 */
LILA_SCENE_API WrapBinding wrapBind(const geo::Mesh& driver, std::span<const glm::dvec3> targets, int maxDrivers, double falloff);
/** Target points for the driver's current points (same topology as at bind). */
LILA_SCENE_API std::vector<glm::dvec3> wrapApply(const WrapBinding& bind, std::span<const glm::dvec3> driverPoints, std::span<const glm::dvec3> targets);

// ------------------------------------------------------------------------------------------------ cage

/** Mean value coordinates of x against a closed triangle cage (Ju, Schaefer and Warren 2005). Sums to 1. */
LILA_SCENE_API void meanValueCoordinates(std::span<const glm::dvec3> cage, std::span<const uint32_t> triangles, const glm::dvec3& x, std::span<double> out);

/** Dense coordinates for many points, row per point. Parallel. */
LILA_SCENE_API std::vector<float> meanValueCoordinates(std::span<const glm::dvec3> cage, std::span<const uint32_t> triangles, std::span<const glm::dvec3> points);

/** The triangles of a mesh as point ids, 3 per triangle, for the cage functions. */
LILA_SCENE_API std::vector<uint32_t> trianglePoints(const geo::Mesh& mesh);
} // namespace lila::deform
