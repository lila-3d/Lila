#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "geo/Triangulate.h"

#include <span>
#include <vector>

/**
 * Modeling operations. Each works on a component selection and returns the new selection. Ids of
 * untouched elements stay stable; removed elements are only marked deleted, so call Mesh::compact()
 * when convenient. Attributes are carried along: corner uvs and colours are copied or interpolated,
 * edge "hard"/"seam"/"crease" follow the edges they came from, face attributes follow the face.
 *
 * Ops taking a SymmetryMap run on the selection plus its mirror and keep on-plane points on the plane.
 */
namespace lila::geo
{
// Delete. Leftover loose edges and points are removed too.
LILA_SCENE_API void deleteFaces(Mesh& mesh, std::span<const Index> faces, const SymmetryMap* symmetry = nullptr);
/** Removes the edges and their faces. */
LILA_SCENE_API void deleteEdges(Mesh& mesh, std::span<const Index> edges, const SymmetryMap* symmetry = nullptr);
/** Removes the points with their edges and faces. */
LILA_SCENE_API void deletePoints(Mesh& mesh, std::span<const Index> points, const SymmetryMap* symmetry = nullptr);

struct ExtrudeOptions
{
    /** Distance along the averaged face normals. */
    double offset = 0.0;
    /** Each face separately instead of as connected regions. */
    bool individual = false;
    const SymmetryMap* symmetry = nullptr;
    /** Inward distance of the extruded cap's border in its plane (Maya's offset); negative grows it. */
    double inset = 0.0;
};

struct ExtrudeResult
{
    std::vector<Index> faces; // the extruded faces, same ids as the input
    std::vector<Index> sides; // new side faces
};

LILA_SCENE_API ExtrudeResult extrudeFaces(Mesh& mesh, std::span<const Index> faces, const ExtrudeOptions& opts = {});

struct InsetOptions
{
    /** Inward distance in the face plane. */
    double thickness = 10.0;
    /** Distance along the normals after the inset. */
    double depth = 0.0;
    bool individual = false;
    const SymmetryMap* symmetry = nullptr;
};

/** Returns the inner faces (same ids as the input) and the new rim faces as sides. */
LILA_SCENE_API ExtrudeResult insetFaces(Mesh& mesh, std::span<const Index> faces, const InsetOptions& opts = {});

struct BevelOptions
{
    /** Width from the original edge to each new edge, measured perpendicular in the faces. */
    double offset = 5.0;
    int segments = 1;
    /** 0.5 round, 0.25 straight, towards 1 square. */
    double profile = 0.5;
    /** Keep each offset under half of the shortest neighbouring edge. */
    bool clamp = true;
    /** New edges sharper than this are marked hard; negative leaves them soft. */
    double smoothingAngle = 30.0;
    const SymmetryMap* symmetry = nullptr;
};

/** Bevels manifold edges. Returns the new bevel and corner faces. */
LILA_SCENE_API std::vector<Index> bevelEdges(Mesh& mesh, std::span<const Index> edges, const BevelOptions& opts = {});

struct EdgeLoopOptions
{
    /** Where along each ring edge the first loop goes, 0 to 1, measured from the start edge's first point side. */
    double fraction = 0.5;
    /** More than one inserts evenly spaced loops and ignores fraction. */
    int count = 1;
};

/** Inserts loops across the edge ring through ringEdge. Returns the new loop edges. */
LILA_SCENE_API std::vector<Index> insertEdgeLoop(Mesh& mesh, Index ringEdge, const EdgeLoopOptions& opts = {});

/**
 * Splits faces between selected points that are consecutive (among the selected) around a face but not already
 * joined by an edge, Maya's Connect. Returns the new edges.
 */
LILA_SCENE_API std::vector<Index> connectPoints(Mesh& mesh, std::span<const Index> points);

/** Splits each edge into count+1 pieces. Returns the new points. */
LILA_SCENE_API std::vector<Index> subdivideEdges(Mesh& mesh, std::span<const Index> edges, int count = 1);

struct MergeOptions
{
    double distance = 0.01;
    /** Merged points move to their average instead of staying at the lowest id. */
    bool average = false;
    const SymmetryMap* symmetry = nullptr;
};

/** Welds points closer than the distance (all points when the selection is empty). Returns survivors. */
LILA_SCENE_API std::vector<Index> mergeByDistance(Mesh& mesh, std::span<const Index> points, const MergeOptions& opts = {});
/** Welds all given points into one at their centre. */
LILA_SCENE_API Index mergeToCenter(Mesh& mesh, std::span<const Index> points);
/** Welds the given points into target, keeping its position. */
LILA_SCENE_API Index mergeToTarget(Mesh& mesh, std::span<const Index> points, Index target);
/**
 * Welds points: target[p] is the point p merges into (or p / kInvalid to keep it). Survivors keep
 * their positions. Faces that collapse below three points are removed.
 */
LILA_SCENE_API void weldPoints(Mesh& mesh, std::span<const Index> target);
/** Collapses each connected group of edges to its centre. Returns the resulting points. */
LILA_SCENE_API std::vector<Index> collapseEdges(Mesh& mesh, std::span<const Index> edges);

/** Removes edges by merging their faces; with clean, endpoints left with two edges on the same faces go too (Maya). */
LILA_SCENE_API std::vector<Index> dissolveEdges(Mesh& mesh, std::span<const Index> edges, bool clean = true);
/** Removes points by merging the faces around them. Returns the merged faces. */
LILA_SCENE_API std::vector<Index> dissolvePoints(Mesh& mesh, std::span<const Index> points);

struct BridgeOptions
{
    /** Extra rows of faces between the loops. */
    int divisions = 0;
};

/** Joins two boundary loops (or open boundary chains) of equal length with quads. Returns the new faces. */
LILA_SCENE_API std::vector<Index> bridgeEdgeLoops(Mesh& mesh, std::span<const Index> edges, const BridgeOptions& opts = {});

/** Splits each face into a fan of triangles around a new centre point. Returns the new faces. */
LILA_SCENE_API std::vector<Index> pokeFaces(Mesh& mesh, std::span<const Index> faces);
/** Turns each edge one step within its two faces. Returns the new edges. */
LILA_SCENE_API std::vector<Index> spinEdges(Mesh& mesh, std::span<const Index> edges);
/** Caps the boundary loops through the given edges (all holes when empty). Returns the new faces. */
LILA_SCENE_API std::vector<Index> fillHoles(Mesh& mesh, std::span<const Index> edges);
LILA_SCENE_API void flipNormals(Mesh& mesh, std::span<const Index> faces);
/** Makes winding consistent within each connected part; closed parts face outward. All faces when empty. */
LILA_SCENE_API void conformNormals(Mesh& mesh, std::span<const Index> faces = {});
LILA_SCENE_API std::vector<Index> triangulateFaces(Mesh& mesh, std::span<const Index> faces, const TriangulateOptions& opts = {});

struct QuadrangulateOptions
{
    /** Largest angle between the two triangle normals. */
    double maxFaceAngle = 40.0;
    bool keepHardEdges = true;
    bool keepSeams = true;
    bool keepMaterials = true;
};

/** Joins pairs of neighbouring triangles into quads, best shaped first. Returns the resulting faces. */
LILA_SCENE_API std::vector<Index> quadrangulateFaces(Mesh& mesh, std::span<const Index> faces, const QuadrangulateOptions& opts = {});

struct SubdivideOptions
{
    int levels = 1;
    /** Boundary edges stay sharp (Maya's default). */
    bool sharpBoundary = true;
};

/**
 * Catmull-Clark subdivision. Edge "crease" of 1 or more is sharp (and drops by one per level),
 * fractions blend between smooth and sharp. Corner uvs and colours are interpolated linearly per face.
 */
LILA_SCENE_API Mesh subdivide(const Mesh& mesh, const SubdivideOptions& opts = {});

/** Sparse linear map between point sets: out row i = sum of weights[k] * in point cols[k], k in [offsets[i], offsets[i+1]). */
struct LILA_SCENE_API PointStencil
{
    std::vector<Index> offsets{0};
    std::vector<Index> cols;
    std::vector<double> weights;
    /** Point capacity of the input. */
    Index inputs = 0;

    Index rows() const { return Index(offsets.size() - 1); }
    std::vector<glm::dvec3> apply(std::span<const glm::dvec3> in) const;
};

/**
 * One stencil per level of subdivide(mesh, opts): positions are linear in the cage, so subdivide's points equal the
 * stencils applied in order to the cage's positions. result, when given, receives the subdivided mesh.
 */
LILA_SCENE_API std::vector<PointStencil> subdivideStencils(const Mesh& mesh, const SubdivideOptions& opts = {}, Mesh* result = nullptr);

/** Which side of the plane a mirror keeps: Positive keeps the side the normal points to and rebuilds the other from it. */
enum class MirrorDirection
{
    Positive,
    Negative,
    Both,
};

struct MirrorOptions
{
    SymmetryPlane plane;
    /** Positive and Negative cut the mesh along the plane first; Both mirrors all of it. */
    MirrorDirection direction = MirrorDirection::Both;
    /** Maps mesh points into the space the plane is given in, such as the object's world matrix. */
    glm::dmat4 space{1.0};
    /** Merge points on the plane with their mirror images. */
    bool weld = true;
    /** Points this close to the plane, in plane space, count as on it. */
    double tolerance = 1e-3;
};

/**
 * Cuts every face crossing the plane along it, then removes the faces on the side opposite keepSign (+1 or -1) and
 * faces lying in the plane. Points within tolerance of the plane snap onto it.
 */
LILA_SCENE_API void cutAtPlane(Mesh& mesh, const SymmetryPlane& plane, int keepSign, double tolerance = 1e-3, const glm::dmat4& space = glm::dmat4(1.0));

/** Adds a mirrored copy, after cutting away the discarded side unless the direction is Both. Returns the new faces. */
LILA_SCENE_API std::vector<Index> mirrorMesh(Mesh& mesh, const MirrorOptions& opts = {});

/** A new mesh holding copies of the given faces with all attributes. */
LILA_SCENE_API Mesh extractFaces(const Mesh& mesh, std::span<const Index> faces);
LILA_SCENE_API Mesh combine(std::span<const Mesh* const> meshes);
/** One mesh per connected part. */
LILA_SCENE_API std::vector<Mesh> separate(const Mesh& mesh);
} // namespace lila::geo
