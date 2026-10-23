#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace lila::geo
{
enum class QuadSplit : uint8_t
{
    ShortestDiagonal, // order-independent, what MikkTSpace recommends
    FirstCorner,      // fan from the first corner
};

struct TriangulateOptions
{
    QuadSplit quads = QuadSplit::ShortestDiagonal;
    /** N-gons: flip diagonals toward the constrained Delaunay triangulation (largest smallest angle). */
    bool beauty = true;
};

/** Triangles of a whole mesh. Every live face of n corners gets exactly n-2 triangles. */
struct Triangulation
{
    std::vector<Index> corners;   // 3 corner ids per triangle, wound like the face
    std::vector<Index> faces;     // source face per triangle
    std::vector<Index> faceFirst; // per face capacity: first triangle of the face, kInvalid if deleted

    Index triangleCount() const { return Index(faces.size()); }
};

/**
 * Deterministic: the same face gives the same triangles, so display, export, tangents and picking all
 * agree. Faces are projected onto their Newell plane. Quads take opts.quads unless that diagonal folds,
 * larger faces are ear-clipped (collinear corners are never ear tips) and then optionally flipped
 * toward Delaunay. Never adds points. Parallel.
 */
LILA_SCENE_API Triangulation triangulate(const Mesh& mesh, const TriangulateOptions& opts = {});

/** Appends 3*(n-2) corner ids for face f. */
LILA_SCENE_API void triangulateFace(const Mesh& mesh, Index f, std::vector<Index>& out, const TriangulateOptions& opts = {});

/** Ear clipping of a simple polygon given in 2D, counter-clockwise. Appends 3*(n-2) local indices. */
LILA_SCENE_API void earClip(std::span<const glm::dvec2> poly, std::vector<Index>& out);
} // namespace lila::geo
