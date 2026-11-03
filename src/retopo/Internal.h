#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace lila::progress
{
class Scope;
}

namespace lila::retopo::detail
{
/** Why a remesh stopped early: it ran past its time budget, or was cancelled from the progress overlay. */
struct Interrupted : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/**
 * The running remesh's time budget on this thread. reached() records a finished stage, moves the progress bar and checks;
 * checkpoint() only checks. Both throw Interrupted past the deadline or once the progress root is cancelled, and do
 * nothing while no Budget is open on the thread.
 */
class LILA_SCENE_API Budget
{
public:
    /** seconds <= 0 is unlimited. scope may be null. */
    Budget(double seconds, progress::Scope* scope);
    ~Budget();
    Budget(const Budget&) = delete;
    Budget& operator=(const Budget&) = delete;

    static void reached(const char* stage, double fraction);
    static void checkpoint();
    /** Seconds left before the deadline; infinite with no Budget open or no limit. */
    static double remaining();
    /** Facts for the error message if it runs out ("24k working triangles"). */
    static void note(std::string fact);
    /** "after cross field (24k working triangles)", for the error. */
    std::string where() const;
    double seconds() const { return mSeconds; }

private:
    void check() const;

    double mSeconds;
    double mStart;
    progress::Scope* mScope;
    std::string mStage;
    std::vector<std::string> mNotes;
    Budget* mPrev;
};

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
struct LILA_SCENE_API Surface
{
    std::vector<glm::dvec3> pos;
    std::vector<std::array<Index, 3>> tris;
    std::vector<float> density;           // per vertex, 1 is neutral
    std::unordered_set<uint64_t> hard;    // vertex pairs of user feature edges
    std::vector<glm::dvec3> pins;         // positions that must stay result vertices; detectFeatures makes them corners

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
    std::vector<uint8_t> pinned; // per vertex: sits on one of pins

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
LILA_SCENE_API Surface surfaceFromMesh(const geo::Mesh& mesh, bool useHardEdges);
/** Splits edges longer than maxLength until none are. */
LILA_SCENE_API void refineSurface(Surface& s, double maxLength);
/** QEM simplification to about targetTris, keeping borders and user feature edges. */
LILA_SCENE_API void decimateSurface(Surface& s, Index targetTris);
/** Evens out triangle shapes by tangential relaxation projected onto the source; feature and border vertices stay. */
LILA_SCENE_API void smoothSurface(Surface& s, const geo::MeshBvh& source, int iterations);
/** Keeps the part with plane.distance >= 0, cutting triangles; cut points land exactly on the plane. */
LILA_SCENE_API void clipSurface(Surface& s, const geo::SymmetryPlane& plane, int axis);
/** Drops vertices no triangle uses. */
LILA_SCENE_API void compactSurface(Surface& s);

/** Smooth unit 4-RoSy field as one tangent direction per vertex. */
struct FieldOptions
{
    double scale = 1.0;           // target lattice spacing, for the smoothing length
    double curvatureWeight = 1.0; // alignment to principal directions
    double softCreaseAngle = 0.0; // edges bending at least this many degrees (short of features) pull the field along them; <= 0 off
    std::vector<std::vector<glm::dvec3>> guides;
    /** Per surface edge, an angle (in the field's fourth power) added to its transport; from prescribeSingularities. */
    std::vector<double> twist;
};
LILA_SCENE_API std::vector<glm::dvec3> solveCrossField(const Surface& s, const FieldOptions& opts);

/**
 * Per surface edge, the angle a direction at edges[e][0] turns by on its way to edges[e][1], in the field's fourth
 * power (a quarter turn is 2pi), measured in each vertex's own tangent frame.
 */
LILA_SCENE_API std::vector<double> transportAngles(const Surface& s);
/** Per vertex, the field's direction as a fourth-power angle in the vertex frame. */
LILA_SCENE_API std::vector<double> fieldAngles(const Surface& s, const std::vector<glm::dvec3>& q);
/** The inverse of fieldAngles. */
LILA_SCENE_API std::vector<glm::dvec3> fieldFromAngles(const Surface& s, const std::vector<double>& phi);

/** A cross field singularity: a triangle the field turns round in, by index quarter turns. */
struct FieldSingularity
{
    Index tri = kInvalid;
    /** +1 becomes a three-edge pole, -1 a five-edge pole. */
    int index = 0;
};

/** The singular triangles of a field, in triangle order. */
LILA_SCENE_API std::vector<FieldSingularity> fieldSingularities(const Surface& s, const std::vector<glm::dvec3>& q);

/**
 * The smallest twist (a trivial connection, Crane et al. 2010) whose smooth fields turn exactly at the given triangles.
 * Every closed shell's indices must add up to its total turning (shellIndexTotals); error says which doesn't.
 */
struct PrescribedTwist
{
    std::vector<double> twist;
    std::string error;
};
LILA_SCENE_API PrescribedTwist prescribeSingularities(const Surface& s, std::span<const FieldSingularity> singularities);

/** Per triangle, its shell (connected through edges); and per shell, the index total its singularities must reach, or INT_MIN when it has a border. */
struct Shells
{
    std::vector<Index> shellOfTri;
    std::vector<int> indexTotal;
};
LILA_SCENE_API Shells shellIndexTotals(const Surface& s);

/** A field parallel under the twisted transport, spread from one vertex per component and turned to fit the features best. Instant. */
LILA_SCENE_API std::vector<glm::dvec3> propagateField(const Surface& s, const std::vector<double>& twist);

/** Per-vertex principal curvature tensor in the vertex frame: direction angle doubled and magnitudes. */
struct Curvature
{
    std::vector<glm::dvec3> maxDir; // unit, tangent
    std::vector<double> anisotropy; // |k1 - k2|
    std::vector<double> magnitude;  // sqrt(k1^2 + k2^2)
    std::vector<double> coherence;  // 0 to 1: the share of the anisotropy left after averaging over coherenceRadius
};
LILA_SCENE_API Curvature estimateCurvature(const Surface& s, double coherenceRadius = 0.0);

/**
 * Lattice origin per vertex, the lattice point nearest the vertex. shift starts the lattice that fraction of a cell
 * along the field instead, for a differently placed lattice at the same spacing.
 */
LILA_SCENE_API std::vector<glm::dvec3> solvePositionField(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<double>& scale,
                                                          glm::dvec2 shift = glm::dvec2(0.0));

struct Extracted;

/**
 * The same, from one global seamless parametrization (mixed-integer quadrangulation, Bommes et al. 2009): the surface is
 * cut to a disk through the field's singularities, the parameter jumps across the cuts by quarter turns and whole
 * lattice steps, and singularities land on lattice points. So the lattice only turns at the singularities. Empty, with
 * error set, when it can't be solved. edgeSteps gets, per surface edge, the whole lattice step from its first vertex's
 * lattice point to its second's, for extractMesh. grid gets the quad mesh traced straight from the parametrization's
 * integer grid. foldRetries caps how often a rounding that folds the layout is undone and tried another way; folds gets
 * how many triangles stay folded.
 */
LILA_SCENE_API std::vector<glm::dvec3> solveSeamlessPositions(const Surface& s, const std::vector<glm::dvec3>& q,
                                                              const std::vector<double>& scale, std::string* error = nullptr,
                                                              std::vector<std::array<int, 2>>* edgeSteps = nullptr,
                                                              Extracted* grid = nullptr, int foldRetries = 0,
                                                              Index* folds = nullptr);

/** Lattice points of i and j (returned and outJ) that coincide best near the two vertices. */
LILA_SCENE_API glm::dvec3 compatLatticePair(const glm::dvec3& vi, const glm::dvec3& ni, const glm::dvec3& qi, const glm::dvec3& oi, double si,
                             const glm::dvec3& vj, const glm::dvec3& nj, const glm::dvec3& qj, const glm::dvec3& oj, double sj,
                             glm::dvec3& outJ);
/** q1 turned by a multiple of 90 degrees about n1 to lie closest to q0. */
LILA_SCENE_API glm::dvec3 matchQuarterRotation(const glm::dvec3& q0, const glm::dvec3& q1, const glm::dvec3& n1);

/** What extraction gives back: a polygon mesh plus, per point, the feature it came from. */
struct Extracted
{
    geo::Mesh mesh;
    std::vector<FeatureKind> feature; // per point
    std::vector<uint8_t> boundary;    // per point: came from a surface boundary
};
LILA_SCENE_API Extracted extractMesh(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<glm::dvec3>& o,
                      const std::vector<double>& scale, const std::vector<std::array<int, 2>>* edgeSteps = nullptr);

/** Nearest-point queries against the surface's feature edges, for keeping vertices on creases. */
class LILA_SCENE_API FeatureLines
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

/** The point nearest x on any of the segments; x itself when there are none. */
inline glm::dvec3 closestOnSegments(const glm::dvec3& x, std::span<const std::array<glm::dvec3, 2>> segs)
{
    glm::dvec3 best = x;
    double bestD = std::numeric_limits<double>::max();
    for (const auto& [a, b] : segs)
    {
        const glm::dvec3 ab = b - a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(x - a, ab) / len2, 0.0, 1.0) : 0.0;
        const glm::dvec3 q = a + ab * t;
        const double d = glm::dot(x - q, x - q);
        if (d < bestD)
        {
            bestD = d;
            best = q;
        }
    }
    return best;
}

/** Unit vector perpendicular to n, chosen deterministically. */
inline glm::dvec3 anyTangent(const glm::dvec3& n)
{
    glm::dvec3 a = std::abs(n.x) < 0.6 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    return glm::normalize(glm::cross(n, glm::cross(a, n)));
}
} // namespace lila::retopo::detail
