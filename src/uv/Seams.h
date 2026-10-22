#pragma once

#include "uv/Chart.h"

#include <span>
#include <vector>

namespace lila::uv
{
enum class SeamMethod : uint8_t
{
    Split,   // hard edges, cut each island open into a disk, split where it stretches, merge back where it does not
    OptCuts, // joint seam and distortion optimisation (Li et al. 2018): grow seams where they relieve the most stretch, zip back the rest
};

struct AutoSeamOptions
{
    SeamMethod method = SeamMethod::Split;
    /** Edges sharper than this (degrees) are always seams; islands never merge across them. */
    double hardAngle = 60.0;
    /** Edges marked "hard" are always seams too. */
    bool useHardEdges = true;
    /** Existing seams stay. */
    bool keepSeams = false;
    /** Islands whose flattened distortion (symmetric Dirichlet excess) is above this are split. */
    double maxDistortion = 0.08;
    /** Islands this small are never split for distortion. */
    int minSplitFaces = 8;
    /** Prefer cuts where they are least visible: occluded, concave and downward facing. */
    bool visibility = true;
    /** Merge neighbouring islands back together while the result stays below maxDistortion. */
    bool merge = true;
    /** OptCuts: symmetric Dirichlet excess over all the islands together to get under. */
    double distortionBound = 0.05;
    /** OptCuts: price of seam length against distortion; higher keeps seams shorter and more hidden. */
    double seamWeight = 0.5;
};

struct AutoSeamResult
{
    /** Final islands as face lists, with the uvs of a quick flatten in charts. */
    std::vector<Chart> charts;
    Index seamEdges = 0;
};

/**
 * Seams for game assets: hard and sharp edges first, then each island is cut open into a disk along the
 * cheapest path (short, along creases, out of sight) and split where it cannot flatten without stretch.
 * Writes "seam" on the faces' edges (all live faces when empty).
 */
AutoSeamResult autoSeams(geo::Mesh& mesh, std::span<const Index> faces = {}, const AutoSeamOptions& opts = {});

/**
 * Adds the fewest seams that turn each island of the faces (from current seams) into a disk: closed parts
 * are split in two, tubes and holed islands are cut along the cheapest path. Returns the seams added.
 */
Index cutToDisks(geo::Mesh& mesh, std::span<const Index> faces = {});

/** Per edge capacity: the price of cutting each edge of the faces (its length, less where hidden, creased or concave). */
std::vector<double> seamCosts(const geo::Mesh& mesh, std::span<const Index> faces, bool visibility = true);

/** Per point capacity: how open each point is to the sky, 0 (enclosed) to 1. Ray traced, parallel. */
std::vector<float> pointVisibility(const geo::Mesh& mesh, int rays = 16);
} // namespace lila::uv
