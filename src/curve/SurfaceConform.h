#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

namespace lila::geo
{
class Mesh;
class MeshBvh;
} // namespace lila::geo

/** Curves that live on a mesh surface (Maya's Make Live): their points held on it as they are drawn and edited. */
namespace lila::curve
{
/**
 * Moves every anchor or control point to the closest point on the surface (curve and mesh in the same space, bvh built from
 * mesh), lifted along the surface normal there by offset. Bezier handles keep their length but turn into the surface's
 * tangent plane at their anchor, so the curve leaves each point along the surface. Points with no surface near (an empty
 * BVH) stay where they are.
 */
LILA_SCENE_API void conformToSurface(Spline& s, const geo::Mesh& mesh, const geo::MeshBvh& bvh, double offset);
} // namespace lila::curve
