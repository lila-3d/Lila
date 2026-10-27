#pragma once

#include "core/Export.h"

#include "geo/Conform.h"
#include "geo/Knife.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"

#include <cstdint>
#include <span>
#include <vector>

/** Edge loop tools that work along a selected loop: Offset Edge Loop and Edit Edge Flow. */
namespace lila::geo
{
enum class OffsetSide : uint8_t
{
    Both,
    Left,  // the side the faces wound along the chain's direction lie on
    Right,
};

enum class OffsetEnds : uint8_t
{
    Converge, // the new loops meet at an open chain's end point, splitting the faces before it
    Wrap,     // they carry on round the end point and meet on the edge continuing the loop
};

struct OffsetLoopOptions
{
    /** Fraction of each crossing edge's length, or with absolute a distance square to the chain. */
    double distance = 0.25;
    bool absolute = false;
    OffsetSide side = OffsetSide::Both;
    OffsetEnds ends = OffsetEnds::Converge;
    /** Images of the chains under the symmetry are cut too; chains wholly off the driving side are left to their images. */
    const SymmetryMap* symmetry = nullptr;
};

/**
 * Plan of Maya's Offset Edge Loop: a new loop either side of each chain of the given edges, through the crossing edges
 * at the distance. A side with no crossing edge at a point is crossed inside its face. Chains ending on a border
 * end there; chains ending inside the mesh end as opts.ends says.
 */
LILA_SCENE_API KnifePlan offsetEdgeLoopPlan(const Mesh& mesh, std::span<const Index> edges, const OffsetLoopOptions& opts = {});

/** Cuts offsetEdgeLoopPlan; with removeOriginal the chains' own edges are dissolved after. Returns the new edges. */
LILA_SCENE_API std::vector<Index> offsetEdgeLoops(Mesh& mesh, std::span<const Index> edges, const OffsetLoopOptions& opts = {},
                                                  bool removeOriginal = false);

struct EdgeFlowOptions
{
    /** 0 flat between the neighbours across the loop, 1 on the curve through them, above 1 exaggerated, negative inverted. */
    double amount = 1.0;
    /** The curve from the next points out across the loop (Maya's edge flow); off, from the surface normals there. */
    bool edgeFlow = true;
    /** Scales the bend down (toward 1, flatter) or up (negative, fuller). */
    double tension = 0.0;
    /** Also eases each loop along its own length toward a smooth curve, 0 to 1, keeping its spacing. */
    double loopSmooth = 0.0;
    /** Passes of loopSmooth. */
    int iterations = 1;
};

/**
 * Edit Edge Flow: each point of the edges' chains moves onto a curve through the nearest unselected points across its
 * loop: the blend of the two circular arcs through them and each point beyond, which gives back any circle exactly, so
 * the loop follows the surrounding curvature. Selected points between two anchors spread evenly by edge count, as
 * subdividing would place them. Points given loose (on no chain) are placed by both directions across them. Points with
 * nothing across on one side stay. Returns a move for every point considered, unmoved ones included.
 */
LILA_SCENE_API PointMoves editEdgeFlow(const Mesh& mesh, std::span<const Index> edges, std::span<const Index> points,
                                       const EdgeFlowOptions& opts = {});

/**
 * Splits a point selection into the edges of its chains (edges between selected points that each have at most two
 * selected neighbours) and loose points (the rest), the inputs editEdgeFlow takes.
 */
LILA_SCENE_API void flowSelectionOfPoints(const Mesh& mesh, std::span<const Index> points, std::vector<Index>& edges,
                                          std::vector<Index>& loose);
} // namespace lila::geo
