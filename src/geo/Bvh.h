#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"
#include "geo/Triangulate.h"

#include <glm/glm.hpp>

#include <limits>
#include <span>
#include <vector>

namespace lila::geo
{
struct BvhHit
{
    double t = std::numeric_limits<double>::max();
    Index triangle = kInvalid;
    Index face = kInvalid;
    /** Weights of the triangle's three corners at the hit. */
    glm::dvec3 bary{0.0};
    glm::dvec3 point{0.0};
    bool frontFacing = true;
};

struct BvhClosest
{
    double distance = std::numeric_limits<double>::max();
    Index triangle = kInvalid;
    Index face = kInvalid;
    glm::dvec3 bary{0.0};
    glm::dvec3 point{0.0};
};

/** A MeshBvh as flat arrays a GPU traversal uploads as they are (std430). */
struct FlatBvh
{
    /** 32 bytes. Interior nodes (count 0) have children first and first + 1; leaves hold count slots from first. */
    struct Node
    {
        float min[3];
        uint32_t first;
        float max[3];
        uint32_t count;
    };
    std::vector<Node> nodes;
    /** Three corners per leaf slot, in leaf order. w of the first corner is the slot's triangle index (bit cast), else 0. */
    std::vector<glm::vec4> corners;
};

/**
 * Bounding volume hierarchy over a mesh's triangles: binned SAH, built in parallel. Holds a copy of
 * the positions, so rebuild (or refit) after the mesh changes.
 */
class LILA_SCENE_API MeshBvh
{
public:
    MeshBvh() = default;
    explicit MeshBvh(const Mesh& mesh) { build(mesh); }

    void build(const Mesh& mesh);
    void build(const Mesh& mesh, Triangulation tris);
    /**
     * Builds over a triangle soup, three positions per triangle, without a Mesh: triangle t is face t with corners
     * 3t, 3t + 1, 3t + 2. refitPoints is not available on a soup tree.
     */
    void buildSoup(std::span<const glm::vec3> corners);
    /** Updates bounds after points moved, keeping the tree. Positions must have the same capacity. */
    void refit(const Mesh& mesh);
    /**
     * Updates only the given points and the nodes above their triangles, for small edits of a large mesh (sculpt
     * dabs). Topology must not have changed since build; the first call builds the lookup tables it needs.
     */
    void refitPoints(const Mesh& mesh, std::span<const Index> points);

    bool empty() const { return mNodes.empty(); }
    Bounds bounds() const;
    Index triangleCount() const { return Index(mTris.faces.size()); }
    const Triangulation& triangles() const { return mTris; }
    /** Corners of a triangle in the positions the tree was built or refit from. */
    void trianglePoints(Index t, glm::dvec3& a, glm::dvec3& b, glm::dvec3& c) const { triPoints(t, a, b, c); }

    /** Nearest hit along origin + t * dir for t in [0, tMax]. dir need not be unit length. */
    bool raycast(const glm::dvec3& origin, const glm::dvec3& dir, BvhHit& hit,
                 double tMax = std::numeric_limits<double>::max(), bool backfaces = true) const;
    /** True if anything lies along the segment; cheaper than raycast. */
    bool occluded(const glm::dvec3& origin, const glm::dvec3& dir, double tMax) const;
    /** Closest surface point within maxDistance. */
    bool closestPoint(const glm::dvec3& p, BvhClosest& out,
                      double maxDistance = std::numeric_limits<double>::max()) const;
    /** Triangles whose bounds overlap the box. */
    std::vector<Index> overlapping(const Bounds& box) const;
    /** The tree and its triangles in leaf order, positions as floats. Triangle indices are this BVH's (see triangles()). */
    FlatBvh exportFlat() const;

    /** Blend of a corner attribute at a hit, e.g. the uv under the cursor. */
    template <class T> T interpolate(std::span<const T> cornerValues, Index triangle, const glm::dvec3& bary) const
    {
        const Index* c = &mTris.corners[size_t(triangle) * 3];
        return cornerValues[c[0]] * float(bary.x) + cornerValues[c[1]] * float(bary.y) + cornerValues[c[2]] * float(bary.z);
    }

private:
    struct Node
    {
        float min[3];
        uint32_t first; // first child, or first triangle of a leaf
        float max[3];
        uint32_t count; // 0 for interior nodes
    };

    void buildTree();
    void triPoints(Index t, glm::dvec3& a, glm::dvec3& b, glm::dvec3& c) const;

    Triangulation mTris;
    std::vector<Index> mTriPoints; // 3 point ids per triangle
    std::vector<glm::dvec3> mPositions;
    std::vector<Node> mNodes;
    std::vector<Index> mOrder; // triangle ids in leaf order
    // Built on the first refitPoints call.
    std::vector<uint32_t> mParent;
    std::vector<uint32_t> mTriLeaf;
    std::vector<uint32_t> mPointTriOffsets;
    std::vector<Index> mPointTris;
    std::vector<uint8_t> mNodeDirty;
};
} // namespace lila::geo
