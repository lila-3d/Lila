#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <vector>

/**
 * Edge rules for automatic bevelling: which edges of a mesh are folds worth rounding off, and how to find an edge
 * again after the mesh around it was rebuilt.
 */
namespace lila::geo
{
enum class FoldSide : uint8_t
{
    Both,
    Outside, // convex
    Inside,  // concave
};

struct FoldEdgeOptions
{
    /** Range of the angle between the two face normals, degrees (0 is flat). */
    double minAngle = 30.0;
    double maxAngle = 180.0;
    FoldSide side = FoldSide::Both;
    /**
     * Thickness edges are skipped: shorter than this, or with a face narrower than this at the edge (distance from
     * the edge midpoint to the nearest face edge not touching it). 0 keeps everything.
     */
    double thin = 0.0;
    double minLength = 0.0;
    /** Where folds meet at an angle, follow quad loops from the corner until they reach another fold, and add them. */
    bool completeCorners = true;
};

/** Angle between the normals of the edge's two faces, degrees; negative unless the edge has exactly two faces. */
LILA_SCENE_API double edgeFoldAngle(const Mesh& mesh, Index e);
/** True for a convex edge (the faces fold away from their normals). */
LILA_SCENE_API bool edgeIsConvex(const Mesh& mesh, Index e);
/** The face's width across from edge e, as the thin test measures it. */
LILA_SCENE_API double faceWidthAt(const Mesh& mesh, Index f, Index e);

/** Fold edges by the rule. Sorted. */
LILA_SCENE_API std::vector<Index> foldEdges(const Mesh& mesh, const FoldEdgeOptions& opts = {});
/** Blender's bevel limit "Angle": two-face edges whose normals differ by more than the angle. Sorted. */
LILA_SCENE_API std::vector<Index> angleEdges(const Mesh& mesh, double degrees);
/** Edges whose float "bevelWeight" is above the threshold. Sorted. */
LILA_SCENE_API std::vector<Index> weightedEdges(const Mesh& mesh, double threshold = 0.0);

/** Remembers one edge well enough to find it again in a changed mesh. */
struct EdgeRef
{
    Index edge = kInvalid;
    Index p0 = kInvalid;
    Index p1 = kInvalid;
    glm::dvec3 a{0.0};
    glm::dvec3 b{0.0};
};

LILA_SCENE_API EdgeRef edgeRef(const Mesh& mesh, Index e);

enum class EdgeMatch : uint8_t
{
    Lost,
    Id,       // same id joining the same points (topology unchanged)
    Position, // same id or another one with the same endpoints
    Overlap,  // a collinear edge overlapping the old one, nearest its midpoint
};

/**
 * Finds remembered edges in a mesh: by id when the topology is unchanged, else by exact endpoints, else by the
 * collinear overlapping edge nearest the old midpoint. Spatial grids are built on first use, so lookups stay
 * near constant time on big meshes.
 */
class LILA_SCENE_API EdgeMatcher
{
public:
    explicit EdgeMatcher(const Mesh& mesh);
    ~EdgeMatcher();
    EdgeMatcher(const EdgeMatcher&) = delete;
    EdgeMatcher& operator=(const EdgeMatcher&) = delete;

    /** sameTopology: the mesh has the element counts the ref was taken at, so its ids still mean the same edges. */
    Index find(const EdgeRef& ref, bool sameTopology, EdgeMatch* how = nullptr) const;
    double tolerance() const { return mTol; }

private:
    struct Grids;
    Index byEndpoints(const glm::dvec3& a, const glm::dvec3& b) const;
    Index byOverlap(const glm::dvec3& a, const glm::dvec3& b) const;
    Grids& grids() const;

    const Mesh& mMesh;
    double mTol = 1e-6;
    mutable Grids* mGrids = nullptr;
};
} // namespace lila::geo
