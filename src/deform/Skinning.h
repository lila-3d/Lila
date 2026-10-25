#pragma once

#include "core/Export.h"

#include "deform/Deform.h"

#include <glm/glm.hpp>

#include <array>
#include <span>
#include <vector>

/**
 * Skinning methods beyond linear blend and dual quaternion, free of the scene graph. Weights are CSR like
 * scene::SkinWeights; the palette holds each influence id's skinning matrix (identity where an influence is missing).
 */
namespace lila::deform
{
struct WeightsView
{
    std::span<const uint32_t> offsets; // point p's entries are [offsets[p], offsets[p + 1])
    std::span<const uint32_t> influences;
    std::span<const float> weights;

    size_t points() const { return offsets.empty() ? 0 : offsets.size() - 1; }
};

/**
 * Optimized centres of rotation (Le and Hodgins 2016): each point's centre is the area-weighted average of the
 * triangle centroids whose weights resemble its own. Points with one influence have no centre (they skin rigidly).
 */
struct CorBinding
{
    std::vector<glm::dvec3> centers;
    std::vector<char> valid;
};

/** sigma is the paper's similarity width (0.1). Triangles as point ids, 3 per triangle. */
LILA_SCENE_API CorBinding corBind(std::span<const glm::dvec3> rest, std::span<const uint32_t> triangles, const WeightsView& w, double sigma);

/**
 * Skins with the bound centres: rotation from the blended quaternions, translation from linear skinning of the centre.
 * Writes positions and, when rotations is given, each point's rotation for its normals.
 */
LILA_SCENE_API void corSkin(std::span<const glm::dvec3> rest, const CorBinding& bind, const WeightsView& w, std::span<const glm::dmat4> palette,
                            std::vector<glm::dvec3>& out, std::vector<glm::dmat3>* rotations);

/**
 * Direct Delta Mush (Le and Lewis 2019, variant 0): per point and influence, the smoothed moment matrix
 * Omega = sum_k S_ik w_kj [u u^T, u; u^T, 1] of the rest points, packed as the six unique entries of u u^T, then u,
 * then the weight. S is Lila's delta mush smoothing (iterations of Jacobi Laplacian steps).
 */
struct DdmBinding
{
    std::vector<uint32_t> offsets{0};
    std::vector<uint32_t> influences;
    std::vector<std::array<double, 10>> omega;
};

LILA_SCENE_API DdmBinding ddmBind(std::span<const glm::dvec3> rest, const Adjacency& adj, const WeightsView& w, int iterations, double step,
                                  bool pinBoundary);

/** Skins with the bound moments: each point's rotation is the polar part of its blended moment, falling back to linear skinning when degenerate. */
LILA_SCENE_API void ddmSkin(std::span<const glm::dvec3> rest, const DdmBinding& bind, const WeightsView& w, std::span<const glm::dmat4> palette,
                            std::vector<glm::dvec3>& out, std::vector<glm::dmat3>* rotations);

/** Linear blend skinning with normalized weights, the reference the methods above fall back to. */
LILA_SCENE_API glm::dmat4 blendMatrix(const WeightsView& w, size_t p, std::span<const glm::dmat4> palette);
} // namespace lila::deform
