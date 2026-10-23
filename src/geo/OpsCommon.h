#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <span>
#include <vector>

namespace lila::geo::detail
{
/**
 * addFace that also copies attributes: face from srcFace, corner i from srcCorners[i], and the edge
 * from point i to i+1 from srcEdges[i] when that edge is new. Any source may be kInvalid or empty.
 */
LILA_SCENE_API Index addFaceFrom(Mesh& mesh, std::span<const Index> points, Index srcFace, std::span<const Index> srcCorners,
                  std::span<const Index> srcEdges = {});

/** A live face with exactly these points in any rotation and direction, or kInvalid. */
LILA_SCENE_API Index findFaceWithPoints(const Mesh& mesh, std::span<const Index> points);

/** Removes faces, then edges and points they leave loose. */
LILA_SCENE_API void removeFacesClean(Mesh& mesh, std::span<const Index> faces);

/**
 * Corners around p in order, each next one across the previous one's outgoing edge. Open fans start
 * at the boundary. False when p is non-manifold (several fans or a non-manifold edge inside).
 */
LILA_SCENE_API bool pointFan(const Mesh& mesh, Index p, std::vector<Index>& corners, bool& closed);

/** Corner of the other face on c's edge at the same point as c, for a manifold edge; kInvalid otherwise. */
LILA_SCENE_API Index acrossAtPoint(const Mesh& mesh, Index c, Index p);

/** Mix of corner attributes into dst from corners with weights. */
LILA_SCENE_API void mixCorners(Mesh& mesh, Index dst, std::span<const Index> srcs, std::span<const float> weights);

/** Snapshot of a face before it is removed: points and corners in order. */
struct FaceCopy
{
    Index face;
    std::vector<Index> points;
    std::vector<Index> corners;
    std::vector<Index> edges;
};
LILA_SCENE_API FaceCopy copyFace(const Mesh& mesh, Index f);
} // namespace lila::geo::detail
