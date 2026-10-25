#pragma once

#include "core/Export.h"
#include "curve/Spline.h"
#include "geo/Mesh.h"

#include <string>

/** Curve network surfaces: a quad mesh through a grid of crossing curves (Rhino's NetworkSrf, Alias's curve network). */
namespace lila::curve
{
struct NetworkOptions
{
    /** Curves closer than this meet, cm. */
    double tolerance = 0.1;
    /** Quads along each U curve between neighbouring V curves, and along each V curve between neighbouring U curves. */
    int spansU = 4;
    int spansV = 4;
    /** Per curve: 0 puts it in the U family, 1 in V, anything else sorts it automatically. May be shorter than the curves. */
    std::vector<int> families;
    bool flip = false;
};

struct NetworkResult
{
    geo::Mesh mesh;
    /** Per input curve: 0 U, 1 V, -1 left out (it meets nothing, or is unusable). */
    std::vector<int> family;
    /** Curves in each family as used, in order (input indices; -1 is a pole where the other family's curves meet). */
    std::vector<int> uOrder, vOrder;
    std::vector<std::string> warnings;
};

/**
 * Sorts crossing curves into two families (curves of one family cross only curves of the other), orders and orients them,
 * and interpolates the grid with a Gordon surface: the curves are blended across with C2 cardinal splines, so the surface
 * passes through every curve and stays smooth across them, and every curve becomes an edge loop of the quads. Where one
 * family's curves all meet at a point (a three-sided patch) the missing curve is that point and its row is triangles.
 * Throws std::invalid_argument naming the curves (by index) when the network cannot be built.
 */
LILA_SCENE_API NetworkResult curveNetwork(const Splines& curves, const NetworkOptions& o = {});
} // namespace lila::curve
