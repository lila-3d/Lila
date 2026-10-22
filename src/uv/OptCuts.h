#pragma once

#include "uv/Seams.h"

#include <span>

namespace lila::uv
{
struct OptCutsResult
{
    /** Islands with their SLIM uvs, and the seam count, as autoSeams returns them. */
    AutoSeamResult seams;
    /** Symmetric Dirichlet excess of all islands together (0 = isometric). */
    double distortion = 0.0;
    /** 3D length of the seams inside the faces. */
    double seamLength = 0.0;
    int splits = 0;
    int merges = 0;
    bool reachedBound = false;
    /** Islands that still overlapped themselves and were split by the classic splitter. */
    size_t overlapFixes = 0;
};

/**
 * Auto Seams 2, after OptCuts (Li et al. 2018): starts from the hard edges and the fewest cuts that
 * make disks, then alternates topology steps with SLIM. While the distortion is above
 * opts.distortionBound it extends a seam from an island border by the edge whose cut relieves the most
 * stretch for its price (a local re-solve per candidate; price from the auto seams costs: length,
 * visibility, creases), re-solving the island warm. Once under the bound, seam tips are zipped back,
 * most visible first, while the bound holds. Islands stay fold-free throughout. Writes "seam" like
 * autoSeams.
 */
OptCutsResult optCuts(geo::Mesh& mesh, std::span<const Index> faces, const AutoSeamOptions& opts);

/** Seam length (3D) inside the faces: seam edges with both sides in the set. All live faces when empty. */
double seamLength(const geo::Mesh& mesh, std::span<const Index> faces = {});
} // namespace lila::uv
