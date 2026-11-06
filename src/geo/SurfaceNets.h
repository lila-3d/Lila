#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <climits>
#include <span>
#include <vector>

namespace lila::geo
{
/**
 * A dense grid of signed values, solid where negative: value (i, j, k) is values[i + nx * (j + ny * k)]. Grid points are
 * the samples; cell (i, j, k) is the cube between points (i, j, k) and (i + 1, j + 1, k + 1).
 */
struct SurfaceNetsGrid
{
    glm::ivec3 dims{0};
    std::span<const float> values;

    float at(int i, int j, int k) const { return values[size_t(i) + size_t(dims.x) * (size_t(j) + size_t(dims.y) * size_t(k))]; }
};

struct SurfaceNetsOptions
{
    /**
     * Grid points whose +X, +Y and +Z edges may make quads: faceMin up to, not including, faceMax. An edge also needs the
     * four cells around it inside the grid. Limiting it meshes one block of a larger grid, with a one-point apron around
     * the block so its border cells still get their points.
     */
    glm::ivec3 faceMin{0};
    glm::ivec3 faceMax{INT_MAX};
};

/** Surface nets in index space: one point per cell the surface crosses, joined by a quad across every crossing edge. */
struct SurfaceNets
{
    /** Each point's cell and its place inside it (0 to 1 a side): the mean of the cell's edge crossings. */
    std::vector<glm::ivec3> cells;
    std::vector<glm::dvec3> offsets;
    /** Four points per quad, facing from solid to empty. */
    std::vector<Index> quads;
    /** Per quad: the grid point its crossing edge starts at (x, y, z) and the edge's axis (w: 0 X, 1 Y, 2 Z). */
    std::vector<glm::ivec4> edges;

    size_t numQuads() const { return edges.size(); }
    glm::dvec3 point(size_t p) const { return glm::dvec3(cells[p]) + offsets[p]; }
};

/**
 * Surface nets of the grid. Only points some quad uses are kept, ordered by cell; the result is the same at any thread
 * count, and a cell's point is the same bits whichever block of a larger grid it was computed in.
 */
LILA_SCENE_API SurfaceNets surfaceNets(const SurfaceNetsGrid& grid, const SurfaceNetsOptions& opts = {});

/** surfaceNets as a quad mesh, grid point (i, j, k) placed at origin + (i, j, k) * voxel. */
LILA_SCENE_API Mesh surfaceNetsMesh(const SurfaceNetsGrid& grid, const glm::dvec3& origin, double voxel, const SurfaceNetsOptions& opts = {});
} // namespace lila::geo
