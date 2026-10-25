#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <array>
#include <limits>
#include <string>
#include <vector>

/**
 * Straight skeletons of polygons with holes (Aichholzer et al.), simulated as a shrinking wavefront: edge events where a
 * wavefront edge shrinks away, split events where a reflex vertex runs into an edge, and vertex events where several meet
 * at once, resolved together at the point. Optional per edge speeds give a weighted skeleton (0 makes a gable).
 */
namespace lila::curve
{
struct SkeletonArc
{
    int a = -1, b = -1;
    /** Input edges whose faces lie either side; -1 for the front at maxTime. */
    int faceA = -1, faceB = -1;
};

struct StraightSkeleton
{
    /** Node positions and the time (offset distance) the wavefront reached them. The input vertices come first, in loop order. */
    std::vector<glm::dvec2> nodes;
    std::vector<double> times;
    std::vector<SkeletonArc> arcs;
    /** Per input edge (edge i of a loop runs from its point i to i + 1): its start and end node, and its loop. */
    std::vector<std::array<int, 2>> edges;
    std::vector<int> edgeLoop;
    /**
     * Per input edge, the region of the plane its wavefront swept: node indices counter-clockwise, starting with the edge's
     * start and end node. Empty when it could not be traced.
     */
    std::vector<std::vector<int>> faces;
    /** When stopped at maxTime: the wavefront then, as loops of node indices with the remaining region on their left. */
    std::vector<std::vector<int>> front;
    /** The time the last event happened (the full skeleton) or maxTime. */
    double endTime = 0.0;
    std::string error;

    bool ok() const { return error.empty(); }
};

struct SkeletonOptions
{
    /** Stop the wavefront here (an offset distance); the faces are then cut off along the front. */
    double maxTime = std::numeric_limits<double>::infinity();
    /** Per loop, per edge speed (empty for 1 everywhere). 0 keeps an edge still: a vertical gable on a roof. */
    std::vector<std::vector<double>> weights;
};

/**
 * Skeleton of the region the loops bound: outer loops counter-clockwise and holes clockwise (the region is on the left
 * of every edge). Repeated points must be removed first. Exact degeneracies (parallel edges meeting, several vertices
 * arriving at one point) are resolved at the point they share.
 */
LILA_SCENE_API StraightSkeleton straightSkeleton(const std::vector<std::vector<glm::dvec2>>& loops, const SkeletonOptions& o = {});

/** Removes repeated points, zero-length edges and spikes (edges doubling back on themselves). */
LILA_SCENE_API std::vector<glm::dvec2> cleanLoop(const std::vector<glm::dvec2>& loop, double tolerance);

/** Skeleton arcs between two interior nodes (the medial axis of the straight skeleton), chained into polylines. */
LILA_SCENE_API std::vector<std::vector<glm::dvec2>> skeletonCenterLines(const StraightSkeleton& s);
} // namespace lila::curve
