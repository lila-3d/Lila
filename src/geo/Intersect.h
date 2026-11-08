#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Heightfield.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <vector>

namespace lila::geo
{
/** A chain of intersection segments. Segment k runs from points[k] to points[k + 1] (wrapping when closed). */
struct IntersectionPolyline
{
    std::vector<glm::dvec3> points;
    /** Per segment: the face of each side it lies on (for a heightfield, the cell half: (z * (nx - 1) + x) * 2 + half). */
    std::vector<Index> facesA;
    std::vector<Index> facesB;
    bool closed = false;
};

struct MeshIntersection
{
    std::vector<IntersectionPolyline> lines;
    /** Triangle pairs that cross, before chaining. */
    size_t segments = 0;
    /** Triangle pairs lying in one plane: touching, not crossing, so they give no segment. */
    size_t coplanarPairs = 0;
};

/**
 * The segment where two triangles cross. False when they don't, or when they are coplanar (coplanar is then set).
 * A triangle touching the other at a single point gives no segment.
 */
LILA_SCENE_API bool triangleIntersection(const glm::dvec3 p[3], const glm::dvec3 q[3], glm::dvec3& s0, glm::dvec3& s1,
                                         bool* coplanar = nullptr);

/**
 * Where the surfaces of a and b cross, as chained polylines in a's space. bToA places b in a's space, so one tree
 * serves every instance of a mesh. Endpoints closer than weld join; deterministic at any thread count.
 */
LILA_SCENE_API MeshIntersection intersectMeshes(const MeshBvh& a, const MeshBvh& b, const glm::dmat4& bToA,
                                                double weld = 1e-4);

struct IntersectionInput
{
    const MeshBvh* bvh = nullptr;
    glm::dmat4 world{1.0};
    /** Inputs sharing an object never intersect each other (pieces of one object, or deliberate overlaps). */
    int object = 0;
};

struct ObjectIntersection
{
    /** Indices into the inputs, a < b. */
    size_t a = 0;
    size_t b = 0;
    /** In world space. */
    MeshIntersection result;
};

/** Every pair of inputs from different objects whose surfaces cross, in world space, ordered by (a, b). */
LILA_SCENE_API std::vector<ObjectIntersection> intersectObjects(std::span<const IntersectionInput> inputs,
                                                                double weld = 1e-4);

/**
 * Where a mesh crosses the ground, in the heightfield's space (meshToHeightfield places the mesh there). Cells are
 * split along their (x, z) to (x + 1, z + 1) diagonal; cells with a holes mask above 0.5 at a corner are open.
 * Nothing outside the grid counts.
 */
LILA_SCENE_API MeshIntersection intersectHeightfield(const MeshBvh& mesh, const glm::dmat4& meshToHeightfield,
                                                     const Heightfield& ground, double weld = 1e-4,
                                                     const std::string& holes = "holes");
} // namespace lila::geo
