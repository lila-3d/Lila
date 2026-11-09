#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

/**
 * Soft Net: a network of curves bound to a surface shapes it. Each curve sample is a blend of three surface points (a
 * barycentric soft constraint on the mesh as it is, never cut); moving the curves moves the surface by a smooth
 * displacement that meets the samples and fades with distance.
 */
namespace lila::deform::softnet
{
/** A curve sample: where it sits on the surface and how much each of its two knots carries it. */
struct Sample
{
    std::array<uint32_t, 3> points{};
    glm::dvec3 bary{0.0};
    std::array<int, 2> knot{-1, -1};
    std::array<double, 2> weight{0.0, 0.0};
};

/**
 * The prefactored solve for one mesh and one set of samples: the displacement minimising its bi-Laplacian (over the
 * mesh's edges), a screening term that makes it fade over reach, and the samples' misses weighted by stiffness.
 */
class LILA_SCENE_API Solver
{
public:
    Solver();
    ~Solver();
    Solver(Solver&&) noexcept;
    Solver& operator=(Solver&&) noexcept;

    /**
     * Factors the solve over topology's points and edges; edgeLength is the mean edge length at bind, which reach is
     * measured against. False when there is nothing to solve.
     */
    bool prepare(const geo::Mesh& topology, std::span<const Sample> samples, double edgeLength, double reach, double stiffness);
    bool ready() const;

    /** The displacement of every point that moves each sample by its delta (one per sample). */
    std::vector<glm::dvec3> solve(std::span<const glm::dvec3> deltas) const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

/** Where a sample sits on points. */
inline glm::dvec3 at(const Sample& s, std::span<const glm::dvec3> points)
{
    return points[s.points[0]] * s.bary.x + points[s.points[1]] * s.bary.y + points[s.points[2]] * s.bary.z;
}

/** A sample's arc-length fraction t between its knots as the two weights: smooth at both ends, flat at the knots. */
LILA_SCENE_API std::array<double, 2> knotWeights(double t);
} // namespace lila::deform::softnet
