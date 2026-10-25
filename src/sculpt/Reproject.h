#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Symmetry.h"

#include <memory>
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
    /**
     * The furthest a cage point may move, as a multiple of the largest move of the subdivided surface it shapes; 0 is no
     * limit. Subdivision carries a point's move only part of the way to its surface, so an exact fit moves points up to
     * about 2.5 times the surface; a bump finer than the cage can drive plain least squares much further off.
     */
    double maxMoveRatio = 2.0;
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
LILA_SCENE_API std::vector<glm::dvec3> fitCage(const geo::Mesh& cage, std::span<const geo::PointStencil> stencils, std::span<const glm::dvec3> target,
                                const FitOptions& opts = {});

/**
 * fitCage with its normal equations factored once, for fitting the same cage topology to target after target (a live
 * preview of the fit while sculpting, or the project-and-solve rounds of fitCageToSurface). The stencils, and the
 * symmetry map in opts, must outlive it.
 */
class LILA_SCENE_API CageFitter
{
public:
    CageFitter(const geo::Mesh& cage, std::span<const geo::PointStencil> stencils, const FitOptions& opts = {});
    ~CageFitter();

    /** fitCage's result for a cage starting at `start` (per point capacity). */
    std::vector<glm::dvec3> solve(std::span<const glm::dvec3> start, std::span<const glm::dvec3> target) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

/**
 * fitCageToSurface's least squares rounds with a prepared fitter: subdivide `start` through stencils, project onto the
 * surface bvh holds, solve, repeat. Returns the cage positions.
 */
LILA_SCENE_API std::vector<glm::dvec3> fitToSurface(const CageFitter& fitter, std::span<const geo::PointStencil> stencils,
                                                    std::span<const glm::dvec3> start, const geo::MeshBvh& bvh, int iterations);

/**
 * Moves the cage onto a surface it has no point correspondence with (an imported high poly, a remeshed sculpt). With
 * LeastSquares it subdivides, projects the subdivided points onto the surface, solves, and repeats; Closest and Normal
 * project the cage points directly. surface must be the mesh bvh was built from, in the cage's space.
 */
LILA_SCENE_API geo::Mesh fitCageToSurface(const geo::Mesh& cage, const geo::Mesh& surface, const geo::MeshBvh& bvh, const FitOptions& opts = {});

/** Sets each pair to the average of itself and its partner's reflection, and on-plane points onto the plane. */
LILA_SCENE_API void symmetrizePositions(std::span<glm::dvec3> positions, const geo::SymmetryMap& map);
} // namespace lila::sculpt
