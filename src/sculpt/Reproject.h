#pragma once

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Symmetry.h"

#include <span>
#include <vector>

/**
 * Low-poly cage reprojection: moving a cage so its subdivided surface matches a sculpted or high-resolution one.
 * Only positions change, so UVs, normals and every other attribute of the cage stay as they were.
 */
namespace lila::sculpt
{
enum class FitMethod : uint8_t
{
    /** Least squares through the subdivision stencils: the cage whose subdivision best matches the target. */
    LeastSquares,
    /** Each cage point to the closest point on the target. */
    Closest,
    /** Each cage point along its normal (either way) onto the target. */
    Normal,
};

struct FitOptions
{
    FitMethod method = FitMethod::LeastSquares;
    /** Subdivision the fit goes through (LeastSquares). */
    geo::SubdivideOptions subdivide{.levels = 2};
    /**
     * Pull toward the starting cage, relative to the fit's own weights. Keeps points the target barely constrains in
     * place; a cage that already fits is returned unchanged whatever the value.
     */
    double regularization = 1e-6;
    /** Project-and-solve rounds when fitting to a surface without point correspondence. */
    int iterations = 3;
    /** Per point capacity; nonzero points stay put. Optional. */
    std::vector<uint8_t> pinned;
    /** Mirror pairs of the cage: the result is made exactly symmetric. Optional. */
    const geo::SymmetryMap* symmetry = nullptr;
};

/**
 * Cage positions (per point capacity) whose subdivision through stencils best matches target, which holds one
 * position per subdivided point in subdivide()'s order. Solved as sparse normal equations with Eigen's LDLT.
 */
std::vector<glm::dvec3> fitCage(const geo::Mesh& cage, std::span<const geo::PointStencil> stencils, std::span<const glm::dvec3> target,
                                const FitOptions& opts = {});

/**
 * Moves the cage onto a surface it has no point correspondence with (an imported high poly, a remeshed sculpt). With
 * LeastSquares it subdivides, projects the subdivided points onto the surface, solves, and repeats; Closest and Normal
 * project the cage points directly. surface must be the mesh bvh was built from, in the cage's space.
 */
geo::Mesh fitCageToSurface(const geo::Mesh& cage, const geo::Mesh& surface, const geo::MeshBvh& bvh, const FitOptions& opts = {});

/** Sets each pair to the average of itself and its partner's reflection, and on-plane points onto the plane. */
void symmetrizePositions(std::span<glm::dvec3> positions, const geo::SymmetryMap& map);
} // namespace lila::sculpt
