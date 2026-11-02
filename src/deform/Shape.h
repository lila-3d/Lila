#pragma once

#include "core/Export.h"

#include "deform/Deform.h"
#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/** Shape deformer math: Kelvinlets, cage coordinates beyond mean value, and the simple deformers (shear, cast, smooth, noise, tension). */
namespace lila::deform
{
// ------------------------------------------------------------------------------------------------ kelvinlets

/**
 * Regularized Kelvinlets (de Goes and James 2017): the closed-form displacement of an infinite elastic medium under a
 * load smeared over radius eps, with shear modulus 1 and Poisson ratio nu. r is the point relative to the load centre.
 */
LILA_SCENE_API glm::dvec3 kelvinletGrab(const glm::dvec3& r, const glm::dvec3& force, double eps, double nu);
/** The affine load F (twist when skew, scale when isotropic, pinch when symmetric and traceless). */
LILA_SCENE_API glm::dvec3 kelvinletAffine(const glm::dvec3& r, const glm::dmat3& F, double eps, double nu);

/** How fast a brush fades: one radius decays as 1/r, two (bi-scale) as 1/r^3, three (tri-scale) as 1/r^5. */
struct KelvinletBrush
{
    double radius = 1.0;
    double poisson = 0.25; // 0.5 keeps volume
    int scales = 1;        // 1 to 3
};

/** Grab brush: the centre moves by exactly displacement. */
LILA_SCENE_API glm::dvec3 kelvinletGrabBrush(const glm::dvec3& r, const glm::dvec3& displacement, const KelvinletBrush& b);
/**
 * Twist brush: the centre turns by the rotation vector (axis times radians), fading with distance. Points rotate rather
 * than shear, so large angles keep lengths; small angles match the closed form.
 */
LILA_SCENE_API glm::dvec3 kelvinletTwistBrush(const glm::dvec3& r, const glm::dvec3& rotation, const KelvinletBrush& b);
/** Scale brush: the centre scales by 1 + amount. */
LILA_SCENE_API glm::dvec3 kelvinletScaleBrush(const glm::dvec3& r, double amount, const KelvinletBrush& b);
/** Pinch brush: G (symmetric, traceless) is the displacement gradient at the centre. */
LILA_SCENE_API glm::dvec3 kelvinletPinchBrush(const glm::dvec3& r, const glm::dmat3& G, const KelvinletBrush& b);

/**
 * All four from one affine handle motion: x -> M x + t about the centre. M is split by polar decomposition into a
 * rotation (twist), its isotropic stretch (scale) and the traceless rest (pinch); t is the grab. Each part is scaled by
 * its weight.
 */
struct KelvinletWeights
{
    double grab = 1.0, twist = 1.0, scale = 1.0, pinch = 1.0;
};
struct KelvinletMotion
{
    glm::dvec3 grab{0.0};
    glm::dvec3 twist{0.0}; // rotation vector, radians
    double scale = 0.0;
    glm::dmat3 pinch{0.0};
};
LILA_SCENE_API KelvinletMotion kelvinletMotion(const glm::dmat3& M, const glm::dvec3& t, const KelvinletWeights& w);
LILA_SCENE_API glm::dvec3 kelvinletDisplacement(const glm::dvec3& r, const KelvinletMotion& m, const KelvinletBrush& b);

/** Rotation and symmetric stretch of M = R S (R a proper rotation when det M > 0). */
LILA_SCENE_API void polarDecompose(const glm::dmat3& M, glm::dmat3& R, glm::dmat3& S);

// ------------------------------------------------------------------------------------------------ cage coordinates

/**
 * Green coordinates (Lipman, Levin and Cohen-Or 2008) of x against a closed triangle cage with outward normals:
 * x = sum phi_i v_i + sum psi_j n_j. Deformed: x' = sum phi_i v'_i + sum psi_j s_j n'_j, with s_j the face's stretch.
 */
LILA_SCENE_API void greenCoordinates(std::span<const glm::dvec3> cage, std::span<const uint32_t> triangles, const glm::dvec3& x, std::span<double> phi,
                                     std::span<double> psi);

/** Per face: s_j n'_j - n_j, the face term of a Green coordinates displacement (s_j the deformed face's stretch). */
LILA_SCENE_API std::vector<glm::dvec3> greenFaceTerms(std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed,
                                                      std::span<const uint32_t> triangles);

/**
 * Somigliana coordinates (after Chen, de Goes and Desbrun 2023): the Somigliana identity of linear elasticity over the
 * cage, a double layer (traction kernel) over the cage vertices' linear hat functions and a single layer (Kelvin kernel)
 * over each face, as 3x3 matrices per vertex and per face, row-major. The vertex matrices are normalised to sum to the
 * identity so translations are exact.
 */
LILA_SCENE_API void somiglianaCoordinates(std::span<const glm::dvec3> cage, std::span<const uint32_t> triangles, const glm::dvec3& x, double nu,
                                          std::span<double> vertexCoords, std::span<double> faceCoords);

struct SomiglianaParams
{
    double poisson = 0.3;
    double bulge = 0.0; // 0: stretch across a face follows the cage around it; 1 adds the face's own area loss as stretch along its normal
};

/**
 * Per-face tractions of a deformed cage in the cage's best-fit rigid frame (rot, trans: deformed ~ rot * rest + trans),
 * from each face's corotated strain; the stretch across the face comes from an affine fit of the cage around it, so
 * at bulge 0 a cage under a linear stretch gives the exact elastic tractions. Zero at rest and under rigid motion.
 */
LILA_SCENE_API std::vector<glm::dvec3> somiglianaTractions(std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed,
                                                           std::span<const uint32_t> triangles, const glm::dmat3& rot, const glm::dvec3& trans,
                                                           const SomiglianaParams& p);

/**
 * Harmonic coordinates (Joshi et al. 2007) of points inside a closed triangle cage: per cage vertex, the solution of
 * Laplace's equation on a grid over the cage's interior with the vertex's hat function on the boundary, read back
 * trilinearly. Rows sum to 1. resolution is grid cells across the cage's longest side; points outside take the nearest
 * interior values. Row-major, points x cage points.
 */
LILA_SCENE_API std::vector<float> harmonicCoordinates(std::span<const glm::dvec3> cage, std::span<const uint32_t> triangles,
                                                      std::span<const glm::dvec3> points, int resolution);

// ------------------------------------------------------------------------------------------------ simple set

/** Shear in handle space: x and z move by shear * y, y clamped to [low, high] so the ends stay rigid. */
struct ShearParams
{
    double shearX = 0.5, shearZ = 0.0;
    double low = -1.0, high = 1.0;
};
LILA_SCENE_API glm::dvec3 shear(const glm::dvec3& p, const ShearParams& o);

enum class CastShape : uint8_t
{
    Sphere,
    Cylinder, // around Y
    Box,
};
/** Pulls points toward a sphere, cylinder or box around the origin by factor (0 none, 1 onto it). radius 0: the mean size. */
LILA_SCENE_API void cast(std::vector<glm::dvec3>& points, CastShape shape, double factor, double radius);

/** Taubin smoothing (lambda then mu each pass, mu < -lambda), which smooths without shrinking. */
LILA_SCENE_API void smoothTaubin(std::vector<glm::dvec3>& points, const Adjacency& adj, int iterations, double lambda, double passBand, bool pinBoundary);

/** Seeded gradient (Perlin) noise in about -1..1. */
LILA_SCENE_API double noise3(const glm::dvec3& p, uint32_t seed);
/** Fractal sum of octaves, each at twice the frequency and half the amplitude. */
LILA_SCENE_API double fractalNoise(const glm::dvec3& p, uint32_t seed, int octaves);
/** Three decorrelated noise channels, for displacing along a vector. */
LILA_SCENE_API glm::dvec3 fractalNoise3(const glm::dvec3& p, uint32_t seed, int octaves);

// ------------------------------------------------------------------------------------------------ tension

/** Per point: mean of (length / rest length - 1) over its edges. Positive stretched, negative compressed, 0 at rest. */
LILA_SCENE_API std::vector<float> tension(const geo::Mesh& topology, std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed);

/**
 * Per point: strain along the uv set's U and V directions (length along that direction / rest length - 1), from each
 * face's deformation gradient, averaged over the faces round the point by area. Negative is compression: folds run
 * across the compressed direction. Zeros without the uv set.
 */
LILA_SCENE_API std::vector<glm::vec2> directionalStrain(const geo::Mesh& topology, std::span<const glm::dvec3> rest,
                                                        std::span<const glm::dvec3> deformed, std::string_view uvSet);
} // namespace lila::deform
