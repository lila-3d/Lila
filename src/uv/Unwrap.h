#pragma once

#include "uv/Chart.h"

#include <span>

namespace lila::uv
{
enum class UnwrapMethod : uint8_t
{
    Conformal, // LSCM only: angle preserving, fast, area can drift on curved islands
    Slim,      // LSCM start refined by SLIM (symmetric Dirichlet): low area and angle distortion, no flips
    Bff,       // Boundary First Flattening: conformal with boundary lengths kept; SLIM when it folds or the chart is pinned
};

struct UnwrapOptions
{
    UnwrapMethod method = UnwrapMethod::Slim;
    /** SLIM iterations; it stops early once the energy settles. */
    int iterations = 12;
    /** Keep pinned vertices where they are. Two or more pins also fix the island's scale and placement. */
    bool usePins = true;
};

struct UnwrapResult
{
    bool ok = false;
    bool usedTutte = false; // LSCM folded, so SLIM started from a Tutte embedding
    int iterations = 0;
    /** Symmetric Dirichlet energy per area minus its isometric minimum (0 = no distortion). */
    double distortion = 0.0;
};

/**
 * Flattens one chart, which must be a disk (one boundary loop). Without pins the uvs come out at 3D
 * scale (uv area equals surface area) with the bounding box's lower corner at the origin.
 */
UnwrapResult unwrapChart(Chart& chart, const UnwrapOptions& opts = {});

/** All charts, in parallel. Returns how many succeeded. */
size_t unwrapCharts(std::span<Chart> charts, const UnwrapOptions& opts = {});

/**
 * Least squares conformal map. Uses the chart's pinned vertices when there are two or more, else pins
 * the two vertices farthest apart. False when the system is singular.
 */
bool lscm(Chart& chart, bool usePins = true);

/** Tutte embedding: the outer boundary on a circle, interior at neighbour averages. Never folds. */
bool tutte(Chart& chart);

/**
 * Scalable Locally Injective Maps (Rabinovich et al. 2017) minimising symmetric Dirichlet energy from a
 * fold-free start. Returns the iterations run, or -1 when the start folds.
 */
int slim(Chart& chart, int iterations, bool usePins = true);

/** Symmetric Dirichlet distortion of the chart's current uvs after normalising scale (0 = isometric). */
double chartDistortion(const Chart& chart);

/** Triangles whose uv winding disagrees with the chart's overall orientation, or are degenerate. */
size_t foldedTriangles(const Chart& chart);

/** Uniform scale about the origin so uv area equals 3D area. */
void matchSurfaceArea(Chart& chart);
} // namespace lila::uv
