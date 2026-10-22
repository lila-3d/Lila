#pragma once

#include "uv/Chart.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string_view>
#include <vector>

namespace lila::uv
{
/** Per-face distortion relative to the face's island, so packing scale does not count. */
struct FaceDistortion
{
    /** log2 of the face's uv-to-3D area ratio over its island's: 0 even, +1 twice as big in uv. */
    float area = 0.0f;
    /** Anisotropy: largest over smallest stretch, minus 1. 0 for a conformal (shape kept) face. */
    float angle = 0.0f;
    bool folded = false; // wound against its island
};

struct DistortionSummary
{
    double meanArea = 0.0; // area weighted mean of |area|
    double maxArea = 0.0;
    double meanAngle = 0.0;
    double maxAngle = 0.0;
    size_t folded = 0;
};

/** Per face capacity; faces outside the set (or without uvs) stay zero. All live faces when empty. */
std::vector<FaceDistortion> faceDistortion(const geo::Mesh& mesh, std::string_view uvSet, std::span<const Index> faces = {});
DistortionSummary summarize(const geo::Mesh& mesh, std::span<const FaceDistortion> d, std::span<const Index> faces = {});

/** One number per face for display: 0 perfect, towards 1 badly stretched. */
inline float distortionValue(const FaceDistortion& d)
{
    float v = std::max(std::abs(d.area), d.angle);
    return d.folded ? 1.0f : 1.0f - std::exp(-2.0f * v);
}

/**
 * Pairs of triangles whose interiors overlap in uv. Triangles in the same group are never compared with
 * each other when sameGroupOk is set. Parallel, grid accelerated. Flags are per triangle.
 */
size_t triangleOverlaps(std::span<const std::array<glm::dvec2, 3>> tris, std::vector<uint8_t>& flags,
                        std::span<const Index> group = {}, bool sameGroupOk = false);

/** Faces (per face capacity) whose uvs overlap another face's or fold over. Returns how many. */
size_t overlappingFaces(const geo::Mesh& mesh, std::string_view uvSet, std::vector<uint8_t>& flags, std::span<const Index> faces = {});

/** True when the chart's uv triangles overlap one another anywhere. */
bool chartOverlaps(const Chart& chart);

/** px per 3D unit for a texture of the given resolution: sqrt(uv area / surface area) * resolution. */
double texelDensity(const geo::Mesh& mesh, std::string_view uvSet, std::span<const Index> faces, double resolution,
                    const glm::dmat3& linear = glm::dmat3(1.0));

/**
 * Scales each uv island (about its centre) so it has the given density, as Maya's Set Texel Density does.
 * linear maps object to world space so density counts world size.
 */
void setTexelDensity(geo::Mesh& mesh, std::string_view uvSet, std::span<const Index> faces, double resolution, double density,
                     const glm::dmat3& linear = glm::dmat3(1.0));
} // namespace lila::uv
