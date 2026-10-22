#pragma once

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace lila::retopo::detail
{
using geo::Index;
using geo::kInvalid;

inline uint64_t edgeKey(Index a, Index b)
{
    if (a > b)
        std::swap(a, b);
    return (uint64_t(a) << 32) | b;
}

enum class FeatureKind : uint8_t
{
    None,
    Line,
    Corner,
};

/** The triangle mesh the remesher works on, plus the adjacency and per-vertex data the fields need. */
struct Surface
{
    std::vector<glm::dvec3> pos;
    std::vector<std::array<Index, 3>> tris;
    std::vector<float> density;           // per vertex, 1 is neutral
    std::unordered_set<uint64_t> hard;    // vertex pairs of user feature edges

    // From buildTopology()
    std::vector<std::array<Index, 2>> edges; // sorted endpoints
    std::vector<std::array<Index, 2>> edgeTris;
    std::vector<uint8_t> edgeTriCount;    // 3 means three or more
    std::vector<Index> adjStart;          // CSR, size pos+1
    std::vector<Index> adj;
    std::vector<Index> adjEdge;

    // From buildGeometry()
    std::vector<glm::dvec3> triNormal;
    std::vector<double> triArea;
    std::vector<glm::dvec3> normal;
    std::vector<double> area;

    // From detectFeatures()
    std::vector<uint8_t> edgeFeature;
    std::vector<uint8_t> edgeBoundary;
    std::vector<FeatureKind> feature;
    std::vector<glm::dvec3> featureDir; // unit, in the vertex tangent plane, for Line
    std::vector<uint8_t> onBoundary;

    Index vertexCount() const { return Index(pos.size()); }
    void buildTopology();
    void buildGeometry();
    /**
     * Feature edges: borders, hard edges, and creases sharper than sharpAngleDeg that run at least minCreaseLength.
     * Also makes a corner of any point whose face angles fall short of (or exceed) a flat 360 by more than
     * cornerDeficitDeg, like a cone's tip.
     */
    void detectFeatures(double sharpAngleDeg, double cornerDeficitDeg = 75.0, double minCreaseLength = 0.0);
    /** Keeps only corners at least spacing apart (the most constrained first), so the lattice can honour them. */
    void thinCorners(const std::vector<double>& spacing);
    double totalArea() const;
    double meanEdgeLength() const;
};

/** Triangulates a polygon mesh into a surface; hard edges become feature pairs when asked. */
Surface surfaceFromMesh(const geo::Mesh& mesh, bool useHardEdges);
/** Splits edges longer than maxLength until none are. */
void refineSurface(Surface& s, double maxLength);
/** QEM simplification to about targetTris, keeping borders and user feature edges. */
void decimateSurface(Surface& s, Index targetTris);
/** Evens out triangle shapes by tangential relaxation projected onto the source; feature and border vertices stay. */
void smoothSurface(Surface& s, const geo::MeshBvh& source, int iterations);
/** Keeps the part with plane.distance >= 0, cutting triangles; cut points land exactly on the plane. */
void clipSurface(Surface& s, const geo::SymmetryPlane& plane, int axis);
/** Drops vertices no triangle uses. */
void compactSurface(Surface& s);

/** Smooth unit 4-RoSy field as one tangent direction per vertex. */
struct FieldOptions
{
    double scale = 1.0;           // target lattice spacing, for the smoothing length
    double curvatureWeight = 1.0; // alignment to principal directions
    std::vector<std::vector<glm::dvec3>> guides;
};
std::vector<glm::dvec3> solveCrossField(const Surface& s, const FieldOptions& opts);

/** Per-vertex principal curvature tensor in the vertex frame: direction angle doubled and magnitudes. */
struct Curvature
{
    std::vector<glm::dvec3> maxDir; // unit, tangent
    std::vector<double> anisotropy; // |k1 - k2|
    std::vector<double> magnitude;  // sqrt(k1^2 + k2^2)
};
Curvature estimateCurvature(const Surface& s);

/** Lattice origin per vertex, the lattice point nearest the vertex. */
std::vector<glm::dvec3> solvePositionField(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<double>& scale);

/** Lattice points of i and j (returned and outJ) that coincide best near the two vertices. */
glm::dvec3 compatLatticePair(const glm::dvec3& vi, const glm::dvec3& ni, const glm::dvec3& qi, const glm::dvec3& oi, double si,
                             const glm::dvec3& vj, const glm::dvec3& nj, const glm::dvec3& qj, const glm::dvec3& oj, double sj,
                             glm::dvec3& outJ);
/** q1 turned by a multiple of 90 degrees about n1 to lie closest to q0. */
glm::dvec3 matchQuarterRotation(const glm::dvec3& q0, const glm::dvec3& q1, const glm::dvec3& n1);

/** What extraction gives back: a polygon mesh plus, per point, the feature it came from. */
struct Extracted
{
    geo::Mesh mesh;
    std::vector<FeatureKind> feature; // per point
    std::vector<uint8_t> boundary;    // per point: came from a surface boundary
};
Extracted extractMesh(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<glm::dvec3>& o,
                      const std::vector<double>& scale);

/** Nearest-point queries against the surface's feature edges, for keeping vertices on creases. */
class FeatureLines
{
public:
    /** The surface's feature edges and corners, or with boundaryOnly just its open borders and the corners on them. */
    void build(const Surface& s, double cellSize, bool boundaryOnly = false);
    bool empty() const { return mSegs.empty(); }
    /** Closest point on any feature segment within maxDist; false if none. tangent is the segment direction. */
    bool closest(const glm::dvec3& p, double maxDist, glm::dvec3& out, glm::dvec3* tangent = nullptr) const;
    /** Corner points within maxDist; false if none. */
    bool closestCorner(const glm::dvec3& p, double maxDist, glm::dvec3& out) const;

private:
    std::array<int64_t, 3> cell(const glm::dvec3& p) const;
    uint64_t hash(const std::array<int64_t, 3>& c) const;

    std::vector<std::array<glm::dvec3, 2>> mSegs;
    std::vector<glm::dvec3> mCorners;
    double mCell = 1.0;
    std::vector<std::vector<Index>> mBuckets;      // segments per hashed cell
    std::vector<std::vector<Index>> mCornerBuckets;
};

/** Unit vector perpendicular to n, chosen deterministically. */
inline glm::dvec3 anyTangent(const glm::dvec3& n)
{
    glm::dvec3 a = std::abs(n.x) < 0.6 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    return glm::normalize(glm::cross(n, glm::cross(a, n)));
}
} // namespace lila::retopo::detail
