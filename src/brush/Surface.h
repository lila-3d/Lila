#pragma once

#include "core/Export.h"

#include "brush/Brush.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"

#include <utility>
#include <vector>

namespace lila::brush
{
/**
 * Points a dab reaches walking along edges from the triangle under it, so it never jumps a gap to a nearby part. Each
 * point's weight is the tip's at its distance along the edges. Keeps its scratch space between dabs.
 */
class LILA_SCENE_API SurfaceWalk
{
public:
    /** triangle is a triangle of bvh (the hit's); d.center the point on it. Ascending by point. */
    std::vector<std::pair<geo::Index, float>> gather(const geo::Mesh& mesh, const geo::MeshBvh& bvh, geo::Index triangle, const Dab& d,
                                                     const Falloff& falloff);

    /** Moves a dab that is off the surface (a radial copy) onto the closest point within its radius; false when none is. */
    static bool snap(const geo::MeshBvh& bvh, Dab& d, geo::Index& triangle);

private:
    std::vector<double> mDist;
    FalloffTable mTable;
    Falloff mTableFor;
    bool mHaveTable = false;
};
} // namespace lila::brush
