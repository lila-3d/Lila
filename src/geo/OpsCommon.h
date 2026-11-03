#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace lila::geo::detail
{
/** Offsets and thicknesses clamped where points would cross stop this share short of the crossing. */
inline constexpr double kClampMargin = 0.02;
/** Points a clamped offset brings closer than this share of the offset are welded. */
inline constexpr double kWeldShare = 1e-7;

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

/** weldPoints, also giving each face it rebuilt its new id, or kInvalid when the face collapsed or doubled another. */
LILA_SCENE_API void weldPoints(Mesh& mesh, std::span<const Index> target, std::unordered_map<Index, Index>* rebuilt);

/** No two edges of the closed polygon cross or touch, apart from neighbours sharing their corner. */
LILA_SCENE_API bool polygonIsSimple(std::span<const glm::dvec2> p);

/** q lies inside the closed polygon, by the even-odd rule. */
inline bool insidePolygon(std::span<const glm::dvec2> poly, const glm::dvec2& q)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        const glm::dvec2 &a = poly[i], &b = poly[j];
        if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}
} // namespace lila::geo::detail
