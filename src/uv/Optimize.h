#pragma once

#include "uv/Chart.h"

#include <span>
#include <string_view>

namespace lila::uv
{
/** Distortion of a chart's current uvs, independent of its uv scale. */
struct ChartStats
{
    /** Area weighted mean of |log2| of each triangle's uv-to-3D area ratio over the chart's (0 = even). */
    double area = 0.0;
    double maxArea = 0.0;
    /** Area weighted mean anisotropy: largest over smallest stretch, minus 1 (0 = conformal). */
    double angle = 0.0;
    double maxAngle = 0.0;
    size_t folded = 0;
};

ChartStats chartStats(const Chart& chart);

enum class OptimizeMode : uint8_t
{
    Conformal, // keep angles: each triangle pulled towards its nearest similarity
    Authalic,  // keep areas: each triangle pulled towards its nearest area-preserving map
    Blend,     // per triangle target between the two, by OptimizeOptions::mix
    Isometric, // symmetric Dirichlet (SLIM): both, with no folds
};

struct OptimizeOptions
{
    OptimizeMode mode = OptimizeMode::Blend;
    /** Blend only: 0 keeps angles, 1 keeps areas. */
    double mix = 0.5;
    int iterations = 20;
    /** Pinned vertices stay put; two or more also hold the island's scale and placement. */
    bool usePins = true;
};

struct OptimizeResult
{
    bool ok = false;
    int iterations = 0;
    ChartStats before;
    ChartStats after;
};

/**
 * Improves a chart's current uvs in place (local/global: per-triangle targets, then one Poisson solve
 * on a factorisation reused every iteration). Keeps the island's uv area and centroid. Steps never flip
 * a triangle when the start has none folded.
 */
OptimizeResult optimizeChart(Chart& chart, const OptimizeOptions& opts = {});

struct MosaicOptions
{
    /** Islands whose symmetric Dirichlet excess (chartDistortion) is above this are split. */
    double threshold = 0.08;
    /** Prefer cuts that are hidden: occluded, concave and downward facing. */
    bool visibility = true;
    OptimizeOptions optimize;
};

struct MosaicResult
{
    size_t islandsBefore = 0;
    size_t islandsAfter = 0;
    size_t split = 0; // islands that were cut up
};

/**
 * RizomUV-style mosaic: islands of the faces (from the uv set) that cannot flatten under the threshold are
 * split along short, hidden, creased cuts (the auto seams costs) until every piece can, then every piece
 * is re-solved with the optimise mode. Pieces of a split island keep its uv scale; the largest stays where
 * the island was and the rest are lined up beside it. Seams are written to match.
 */
MosaicResult mosaicFaces(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const MosaicOptions& opts = {});
} // namespace lila::uv
