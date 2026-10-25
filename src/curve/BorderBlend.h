#pragma once

#include "core/Export.h"
#include "curve/Spline.h"
#include "geo/Mesh.h"
#include "geo/Topology.h"

#include <string>

/** Profile blends: a strip of quads bridging two mesh border loops, tangent to the faces on both sides (Alias Profile, Maya Round). */
namespace lila::curve
{
/** One side of a blend: a chain of mesh points with the direction leaving the mesh square to the chain at each. */
struct BorderSide
{
    std::vector<glm::dvec3> points;
    /** Unit, in the tangent plane of the faces along the chain, square to it and away from them. */
    std::vector<glm::dvec3> out;
    bool closed = false;
    /** The faces along the chain wind with it (point k to k + 1), so a strip continuing them must wind against it. */
    bool facesRunForward = false;
};

/**
 * A side from a chain of mesh points (in the mesh's space). Border edges leave away from their one face; for edges with
 * faces on both sides, toward the hint (where the other side is).
 */
LILA_SCENE_API BorderSide borderSide(const geo::Mesh& m, const geo::EdgePath& chain, const glm::dvec3& toward);

struct BorderBlendOptions
{
    /** Quads across the strip. */
    int segments = 6;
    /** Handle length on each side as a multiple of a third of the gap; 0 meets that side at an angle. */
    double tensionA = 1.0, tensionB = 1.0;
    bool flip = false;
};

/** Where a strip column meets a side: on chain point `segment`, or `t` of the way to the next one. */
struct SideStation
{
    size_t segment = 0;
    double t = 0.0;
};

struct BorderBlendResult
{
    geo::Mesh mesh;
    /** Strip points on side A and side B, one per column. */
    std::vector<geo::Index> rowA, rowB;
    /** Where each column meets each side (the side with fewer points gets in-between stations). */
    std::vector<SideStation> stationsA, stationsB;
    std::vector<std::string> warnings;
};

/**
 * Bridges the sides with quads: per column a cubic from side A to side B leaving each along its out direction (G1 with the
 * faces), lined up by arc length with side B turned and reversed to suit A. Profile curves drawn across the gap shape
 * the cross-section where they lie, blended between them along the strip and eased out at the sides so tangency holds.
 * Winds to continue side A's faces. Throws std::invalid_argument when the sides do not pair up (one closed, too short).
 */
LILA_SCENE_API BorderBlendResult borderBlend(const BorderSide& a, const BorderSide& b, const Splines& profiles, const BorderBlendOptions& o = {});
} // namespace lila::curve
