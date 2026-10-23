#pragma once

#include "core/Export.h"

#include "uv/Chart.h"

#include <glm/glm.hpp>

#include <memory>
#include <span>
#include <vector>

/**
 * Boundary First Flattening (Sawhney and Crane 2017): conformal flattening where the boundary is chosen
 * first, either by its scale (lengths) or its curvature (shape), and the interior follows by a harmonic
 * extension. Every step is a solve against one of two Cholesky factorisations made per chart.
 */
namespace lila::uv
{
enum class BffBoundary : uint8_t
{
    MinimalArea,  // scale 0 along the boundary: boundary lengths kept, least area distortion
    Disk,         // boundary on a circle
    Polygon,      // straight sides between corners turning equally: four corners give a rectangle
    Curvature,    // target turning angle per boundary vertex (BffOptions::boundaryValues)
    ScaleFactors, // log scale factor per boundary vertex (BffOptions::boundaryValues)
};

struct BffOptions
{
    BffBoundary boundary = BffBoundary::MinimalArea;
    /** Polygon: chart vertices on the boundary to turn at. Fewer than three picks the four sharpest. */
    std::vector<Index> corners;
    /** Curvature or ScaleFactors values, one per vertex of BffSolver::boundary(), in its order. */
    std::vector<double> boundaryValues;
    /** Cones placed greedily where the scale distortion is largest. MinimalArea boundary only. */
    int autoCones = 0;
    /** Interior chart vertices to make cones, as well. */
    std::vector<Index> cones;
    /** Close the boundary by bending it rather than stretching it, so its edge lengths come out exact. */
    bool exactLengths = false;
};

struct BffResult
{
    bool ok = false;
    /** Cone vertices of the (cut) chart. */
    std::vector<Index> cones;
    /** Chart edges cut open from the cones to the boundary. */
    size_t cutEdges = 0;
};

/** The cached operators of one disk chart, which must outlive it. */
class LILA_SCENE_API BffSolver
{
public:
    explicit BffSolver(const Chart& chart);
    ~BffSolver();
    BffSolver(BffSolver&&) noexcept;
    BffSolver& operator=(BffSolver&&) noexcept;

    /** False unless the chart is a disk (one boundary loop, Euler characteristic 1) and the systems factor. */
    bool ok() const;
    /** The boundary loop, counter-clockwise relative to the triangles. */
    const std::vector<Index>& boundary() const;
    /** Per chart vertex: angle defect inside (2 pi minus the angle sum), turning angle on the boundary (pi minus it). */
    const std::vector<double>& curvature() const;

    /** Target turning angles along the boundary for log scale factors u along it (flat interior). */
    std::vector<double> dirichletToNeumann(std::span<const double> u) const;
    /** Log scale factors along the boundary that give target turning angles k (which must sum to 2 pi). */
    std::vector<double> neumannToDirichlet(std::span<const double> k) const;
    /** Lays out the boundary from its scale u and turning k, closes it and extends it harmonically inside. */
    bool flatten(std::span<const double> u, std::span<const double> k, bool exactLengths, std::vector<glm::dvec2>& uv) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

/**
 * Flattens a disk chart in place. Without pins it comes out at 3D scale with its lower corner at the
 * origin. Cones cut the chart open along shortest paths to the boundary, which changes its vertices
 * (cornerVertex follows).
 */
LILA_SCENE_API BffResult bff(Chart& chart, const BffOptions& opts = {});

/** Greedy cone placement: repeatedly the interior vertex with the largest |log scale| given the cones so far. */
LILA_SCENE_API std::vector<Index> placeCones(const Chart& chart, int count, std::span<const Index> fixedCones = {});

/** Cuts the chart open along the given chart edges (pairs of vertices). Returns the source vertex per vertex. */
LILA_SCENE_API std::vector<Index> cutChart(Chart& chart, std::span<const std::pair<Index, Index>> edges);
} // namespace lila::uv
