#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <span>
#include <string_view>
#include <vector>

namespace lila::geo
{
/**
 * Maya-style edge loop through e: crosses valence-4 points to the edge sharing no face with the
 * current one, follows borders wherever they do not branch, and runs along an n-gon's rim through
 * valence-3 points between quads. Stops at other poles. Ordered; closed loops start at e.
 */
LILA_SCENE_API std::vector<Index> edgeLoop(const Mesh& mesh, Index e);

/** Edges opposite e across quads, in order, both directions. */
LILA_SCENE_API std::vector<Index> edgeRing(const Mesh& mesh, Index e);

/** Next loop edge after e at point p, kInvalid where the loop stops. */
LILA_SCENE_API Index edgeLoopNext(const Mesh& mesh, Index e, Index p);

/** Edge opposite e in quad f, kInvalid if f is not a quad containing e. */
LILA_SCENE_API Index quadOppositeEdge(const Mesh& mesh, Index e, Index f);

struct EdgePath
{
    std::vector<Index> points; // ordered; for a closed path the first point is not repeated
    std::vector<Index> edges;  // edges[i] joins points[i] and points[i+1]
    bool closed = false;
};

/** Boundary edges (one face) chained into ordered loops, following face winding. */
LILA_SCENE_API std::vector<EdgePath> boundaryLoops(const Mesh& mesh);

/** Chains a set of edges into ordered paths. Branching points end paths. */
LILA_SCENE_API std::vector<EdgePath> chainEdges(const Mesh& mesh, std::span<const Index> edges);

/** Component id per face (kInvalid for deleted faces), connected through shared edges. Returns the count. */
LILA_SCENE_API Index faceComponents(const Mesh& mesh, std::vector<Index>& component, std::span<const uint8_t> blockEdge = {});

/** Edges with exactly one side in the face set (or on the mesh boundary). */
LILA_SCENE_API std::vector<Index> regionBoundaryEdges(const Mesh& mesh, std::span<const Index> faces);

/** Points, edges or faces touching the given ones, plus themselves. */
LILA_SCENE_API std::vector<Index> facesOfPoints(const Mesh& mesh, std::span<const Index> points);
LILA_SCENE_API std::vector<Index> facesOfEdges(const Mesh& mesh, std::span<const Index> edges);
LILA_SCENE_API std::vector<Index> pointsOfFaces(const Mesh& mesh, std::span<const Index> faces);
LILA_SCENE_API std::vector<Index> edgesOfFaces(const Mesh& mesh, std::span<const Index> faces);
LILA_SCENE_API std::vector<Index> pointsOfEdges(const Mesh& mesh, std::span<const Index> edges);

/** Sorted, unique copy. */
LILA_SCENE_API std::vector<Index> uniqueSorted(std::span<const Index> ids);

/** Byte mask of size n with the given ids set. */
LILA_SCENE_API std::vector<uint8_t> makeMask(size_t n, std::span<const Index> ids);

/**
 * The corners plus every corner sharing their uv around the same point without crossing a seam: whole uv vertices, so
 * moving them keeps islands whole and leaves islands that only touch across a cut behind. Sorted, unique.
 */
LILA_SCENE_API std::vector<Index> uvVertexCorners(const Mesh& mesh, std::string_view uvSet, std::span<const Index> corners);

/**
 * Corner id to its face-vertex row (faces in id order, corners around each face from faceCorner), kInvalid for dead
 * corners. Rows index the per face-vertex arrays Python gets, such as Node.uvs_array.
 */
LILA_SCENE_API std::vector<Index> faceVertexRows(const Mesh& mesh);
/** The inverse: one corner id per face-vertex row. */
LILA_SCENE_API std::vector<Index> faceVertexCorners(const Mesh& mesh);
} // namespace lila::geo
