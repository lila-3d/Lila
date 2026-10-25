#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/** Handle deformation on a mesh's surface: Laplacian editing, as-rigid-as-possible, and bounded biharmonic weights. */
namespace lila::deform
{
/** Cotangent weights per edge (clamped positive) and lumped (Voronoi-free, a third of each triangle) point areas. */
struct CotanLaplacian
{
    std::vector<uint32_t> edgeA, edgeB;
    std::vector<double> weight;
    std::vector<double> area; // per point
};

LILA_SCENE_API CotanLaplacian cotanLaplacian(const geo::Mesh& topology, std::span<const glm::dvec3> points);

/**
 * A prefactored handle solve on one mesh. Points are held in groups (handle regions and anchors alike), each group
 * moved by its own transform, and the rest follow:
 *   Laplacian: the displacement is biharmonic (smooth, C1 at the groups), so detail translates but does not rotate.
 *   Arap: as-rigid-as-possible (Sorkine and Alexa 2007), detail rotates with the bend. It starts from the groups'
 *   transforms blended by biharmonic weights, so a rigid motion of every group is exact from the first pass.
 * Factoring is the costly part and depends only on the topology, the rest shape and the groups.
 */
class LILA_SCENE_API HandleSolver
{
public:
    enum class Mode : uint8_t
    {
        Laplacian,
        Arap,
    };

    HandleSolver();
    ~HandleSolver();
    HandleSolver(HandleSolver&&) noexcept;
    HandleSolver& operator=(HandleSolver&&) noexcept;

    /**
     * groups has one entry per point: -1 for a free point, else the group holding it. Invalid points, and every point of
     * a connected piece no group reaches, stay where the input puts them. False when there are no free points.
     */
    bool prepare(const geo::Mesh& topology, std::span<const glm::dvec3> rest, std::span<const int> groups, Mode mode);
    bool ready() const;

    /**
     * The deformed points: input is the undeformed shape now (its edge vectors are the detail kept); group g's points
     * move by transforms[g] (the identity past the end). Deterministic for any thread count.
     */
    std::vector<glm::dvec3> solve(std::span<const glm::dvec3> input, std::span<const glm::dmat4> transforms, int iterations) const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

/**
 * Bounded biharmonic weights (Jacobson et al. 2011) on the surface: one weight per point per handle, 1 on the handle's
 * own points, 0 on the other handles', each in [0, 1] (active set over the bounds) and normalised to sum to 1.
 * Row-major, points x handles. A point no handle reaches gets equal weights.
 */
LILA_SCENE_API std::vector<float> boundedBiharmonicWeights(const geo::Mesh& topology, std::span<const glm::dvec3> points,
                                                           const std::vector<std::vector<uint32_t>>& handles);
} // namespace lila::deform
