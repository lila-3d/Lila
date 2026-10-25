#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

/**
 * Quad-dominant fills of planar regions with holes, keeping the boundary points as given: a ring of quads along the
 * boundary (the straight skeleton's wavefront a little way in), a square grid inside it, a constrained Delaunay
 * triangulation joining the two, triangle pairs merged into quads where they make good ones, and a relax of the interior.
 */
namespace lila::curve
{
struct CapFillOptions
{
    /** Grid spacing inside, in the loops' units; 0 takes the boundary's median edge length. */
    double spacing = 0.0;
    /** Merge triangle pairs into quads. */
    bool quads = true;
    /** A ring of quads following the boundary this many spacings wide goes in before the grid; 0 for none. */
    double borderRing = 0.75;
    /** Interior points stay at least this many spacings off the boundary. */
    double margin = 0.6;
    int relaxIterations = 8;
    /** Pairs whose quad has a corner further than this from 90 degrees stay triangles. */
    double maxQuadSkew = 60.0;
};

struct CapFillResult
{
    /** The boundary points, every loop in order, then the interior points. */
    std::vector<glm::dvec2> points;
    /** Counter-clockwise faces (quads and triangles), as indices into points. */
    std::vector<std::vector<int>> faces;
    std::string error;

    bool ok() const { return error.empty(); }
};

/** Fills the region the loops bound (outer loops counter-clockwise, holes clockwise; they must not cross). */
LILA_SCENE_API CapFillResult capFill(const std::vector<std::vector<glm::dvec2>>& loops, const CapFillOptions& o = {});
} // namespace lila::curve
