#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <cstdint>

/** Procedural texture kernels shared by the texture nodes, written to match Blender's. */
namespace lila::gn::tex
{
/** Blender's Jenkins lookup3 hashes of 1 to 4 words (BLI_hash). */
LILA_SCENE_API uint32_t hashU(uint32_t kx);
LILA_SCENE_API uint32_t hashU(uint32_t kx, uint32_t ky);
LILA_SCENE_API uint32_t hashU(uint32_t kx, uint32_t ky, uint32_t kz);
LILA_SCENE_API uint32_t hashU(uint32_t kx, uint32_t ky, uint32_t kz, uint32_t kw);
/** Hash of the coordinates' float bits mapped to 0..1. */
LILA_SCENE_API double hashToFloat(double x);
LILA_SCENE_API double hashToFloat(double x, double y);
LILA_SCENE_API double hashToFloat(double x, double y, double z);
LILA_SCENE_API double hashToFloat(double x, double y, double z, double w);

enum class VoronoiFeature : uint8_t
{
    F1,
    F2,
    SmoothF1,
    DistanceToEdge,
    NSphereRadius,
};

enum class VoronoiMetric : uint8_t
{
    Euclidean,
    Manhattan,
    Chebyshev,
    Minkowski,
};

struct VoronoiParams
{
    int dimensions = 3;
    VoronoiFeature feature = VoronoiFeature::F1;
    VoronoiMetric metric = VoronoiMetric::Euclidean;
    double scale = 5.0;
    double detail = 0.0;
    double roughness = 0.5;
    double lacunarity = 2.0;
    double smoothness = 1.0;
    double exponent = 0.5;
    double randomness = 1.0;
    bool normalize = false;
};

struct VoronoiResult
{
    double distance = 0.0;
    glm::dvec3 color{0.0};
    glm::dvec3 position{0.0};
    double w = 0.0;
    double radius = 0.0;
};

/** Samples at vector (and w for 1D and 4D), both multiplied by params.scale first, like the node. */
LILA_SCENE_API VoronoiResult voronoi(const VoronoiParams& params, const glm::dvec3& vector, double w = 0.0);

/** Gradient noise in -1..1 over four dimensions. */
LILA_SCENE_API double gradientNoise4(const glm::dvec4& p);
} // namespace lila::gn::tex
