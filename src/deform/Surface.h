#pragma once

#include "core/Export.h"

#include "deform/Deform.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

/**
 * Surface-aware deformer math: shrinkwrap, surface and point deform, proximity wrap, collision, rigidify, skin slide
 * and surface pins. Points are world space; every function gives the same result on any thread count.
 */
namespace lila::deform
{
// ------------------------------------------------------------------------------------------------ rigid fit

/** A rotation about one centroid followed by a move to another. */
struct RigidFit
{
    glm::dmat3 rotation{1.0};
    glm::dvec3 from{0.0}, to{0.0};
    glm::dvec3 apply(const glm::dvec3& p) const { return rotation * (p - from) + to; }
};

/**
 * Best-fit rotation and translation taking rest onto deformed, least squares (Kabsch). Solved as Horn's quaternion
 * eigenproblem, so the result is always a proper rotation, reflections never. ids picks a subset; empty uses all.
 */
LILA_SCENE_API RigidFit rigidFit(std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed, std::span<const uint32_t> ids = {});

// ------------------------------------------------------------------------------------------------ point lookup

/** Nearest-point and radius queries over a fixed point set (uniform grid). */
class LILA_SCENE_API PointGrid
{
public:
    PointGrid() = default;
    explicit PointGrid(std::span<const glm::dvec3> points) { build(points); }

    void build(std::span<const glm::dvec3> points);
    bool empty() const { return mPoints.empty(); }

    /** The k nearest points as (distance, id), nearest first, ties by id. */
    void nearest(const glm::dvec3& p, int k, std::vector<std::pair<double, uint32_t>>& out) const;
    /** Ids of the points within r, ascending. */
    void within(const glm::dvec3& p, double r, std::vector<uint32_t>& out) const;

private:
    glm::ivec3 cellOf(const glm::dvec3& p) const;
    template <class F> void visitRing(const glm::ivec3& c, int ring, F&& fn) const;

    std::vector<glm::dvec3> mPoints;
    glm::dvec3 mOrigin{0.0};
    double mCell = 1.0;
    glm::ivec3 mDims{1};
    std::vector<uint32_t> mStart; // per cell into mIds, plus one
    std::vector<uint32_t> mIds;
};

// ------------------------------------------------------------------------------------------------ surfaces

/** Area-weighted point normals of a mesh with its points moved to `points`. */
LILA_SCENE_API std::vector<glm::dvec3> pointNormals(const geo::Mesh& topology, std::span<const glm::dvec3> points);

/** A world-space mesh prepared for closest point, ray and inside queries. */
class LILA_SCENE_API SurfaceTarget
{
public:
    SurfaceTarget() = default;
    explicit SurfaceTarget(const geo::Mesh& worldMesh) { build(worldMesh); }

    void build(const geo::Mesh& worldMesh);
    bool empty() const { return mBvh.empty(); }
    const geo::Mesh& mesh() const { return mMesh; }
    const geo::MeshBvh& bvh() const { return mBvh; }
    const PointGrid& points() const { return mGrid; }
    std::span<const glm::dvec3> normals() const { return mNormals; }
    /** Every edge has two faces: inside is decided by ray parity, else by the side of the nearest surface. */
    bool closed() const { return mClosed; }

    uint32_t triPoint(geo::Index triangle, int k) const;
    glm::dvec3 faceNormal(geo::Index triangle) const;
    /** Point normals blended at bary. */
    glm::dvec3 smoothNormal(geo::Index triangle, const glm::dvec3& bary) const;

    struct Hit
    {
        glm::dvec3 point{0.0};
        glm::dvec3 normal{0.0}; // smooth
        geo::Index triangle = geo::kInvalid;
        glm::dvec3 bary{0.0};
        double distance = 0.0;
    };
    bool closest(const glm::dvec3& p, Hit& hit, double maxDistance = std::numeric_limits<double>::max()) const;
    /** Nearest hit along dir within limit, skipping faces facing (cullFront) or facing away from (cullBack) the ray. */
    bool ray(const glm::dvec3& origin, const glm::dvec3& dir, double limit, bool cullFront, bool cullBack, Hit& hit) const;
    bool inside(const glm::dvec3& p) const;

private:
    geo::Mesh mMesh;
    geo::MeshBvh mBvh;
    std::vector<glm::dvec3> mNormals;
    PointGrid mGrid;
    bool mClosed = false;
};

// ------------------------------------------------------------------------------------------------ shrinkwrap

enum class ShrinkwrapMode : uint8_t
{
    NearestSurface,
    Project,       // along an axis or each point's normal
    NearestVertex,
    TargetNormal,  // along the target's smooth normals: the surface point whose normal passes through the point
};

enum class ShrinkwrapPlace : uint8_t
{
    OnSurface,      // offset toward the side the point came from
    Inside,         // points outside move in; points deeper than offset stay
    Outside,        // points inside move out; points farther than offset stay
    OutsideSurface, // on the surface, offset always to the outside
    AboveSurface,   // offset along the surface normal
};

enum class CullFaces : uint8_t
{
    Off,
    Front,
    Back,
};

struct ShrinkwrapParams
{
    ShrinkwrapMode mode = ShrinkwrapMode::NearestSurface;
    ShrinkwrapPlace place = ShrinkwrapPlace::OnSurface;
    double offset = 0.0;
    glm::dvec3 axis{0.0}; // Project: world direction; zero projects along each point's normal
    bool positive = true, negative = false;
    CullFaces cull = CullFaces::Off;
    double limit = 0.0;   // Project: farthest a point travels, 0 no limit
    int smoothIterations = 0;
    double smoothStep = 0.5;
    bool keepDetail = false; // wrap the smoothed shape, then put the detail smoothing removed back on
};

LILA_SCENE_API std::vector<glm::dvec3> shrinkwrap(const geo::Mesh& topology, const Adjacency& adj, std::span<const glm::dvec3> points,
                                                  const SurfaceTarget& target, const ShrinkwrapParams& o);

// ------------------------------------------------------------------------------------------------ surface deform

/** Orthonormal frame of a polygon: z its Newell normal, x toward its first point from the centroid. */
LILA_SCENE_API glm::dmat3 polygonFrame(std::span<const glm::dvec3> polygon);
/** Mean value coordinates (Floater) of x projected onto the polygon's plane; reproduces any point of that plane. */
LILA_SCENE_API void polygonCoordinates(std::span<const glm::dvec3> polygon, const glm::dvec3& x, std::span<double> out);

/** Each target point rides up to maxFaces driver polygons: a mean value anchor on the polygon plus an offset in its frame. */
struct SurfaceBinding
{
    struct Influence
    {
        uint32_t first = 0, count = 0; // polygon points in ids and coords
        double weight = 0.0;
        glm::dvec3 offset{0.0};        // from the anchor, in polygonFrame
        glm::dvec2 uv{0.0};            // where the anchor sits in the driver's uvs
        glm::dvec3 uvOffset{0.0};      // the offset in the uv frame there, for rebinding by uv
    };
    std::vector<uint32_t> offsets{0};
    std::vector<Influence> influences;
    std::vector<uint32_t> ids;
    std::vector<double> coords;
    uint32_t driverPoints = 0;
    bool hasUv = false;
};

/** falloff is the exponent of the inverse distance weighting between a point's polygons. */
LILA_SCENE_API SurfaceBinding surfaceBind(const geo::Mesh& driver, std::span<const glm::dvec3> targets, int maxFaces, double falloff,
                                          std::string_view uvSet = "uv0");
LILA_SCENE_API std::vector<glm::dvec3> surfaceApply(const SurfaceBinding& bind, std::span<const glm::dvec3> driver, std::span<const glm::dvec3> targets);
/**
 * The binding carried onto a driver whose topology changed, through the uv of every influence: each point lands where
 * it would on the old driver at the new one's current shape. Influences whose uv no face covers are dropped.
 */
LILA_SCENE_API SurfaceBinding surfaceRebindByUv(const SurfaceBinding& bind, const geo::Mesh& driver, std::string_view uvSet = "uv0");

/** Houdini Point Deform: targets captured by nearby driver points, each moving them by its neighbourhood's rigid motion. */
struct PointCapture
{
    std::vector<uint32_t> offsets{0};
    std::vector<uint32_t> ids;
    std::vector<double> weights;
    std::vector<glm::dvec3> rest;           // driver points at capture
    std::vector<uint32_t> nbOffsets{0}, nb; // each driver point's neighbourhood
};

/** radius 0 sizes each point's kernel from its maxPoints nearest driver points. */
LILA_SCENE_API PointCapture pointCapture(std::span<const glm::dvec3> driverRest, std::span<const glm::dvec3> targets, int maxPoints, double radius,
                                         int neighbours);
LILA_SCENE_API std::vector<glm::dvec3> pointDeform(const PointCapture& c, std::span<const glm::dvec3> driver, std::span<const glm::dvec3> targets);

// ------------------------------------------------------------------------------------------------ proximity wrap

/** Wrap 2: each target point rides up to maxDrivers triangles across one or more drivers. */
struct ProximityBinding
{
    struct Influence
    {
        uint32_t driver = 0;
        uint32_t tri[3]{};
        glm::dvec3 bary{0.0};
        glm::dvec3 offset{0.0}; // in the triangle's frame
        double weight = 0.0;
        double restArea = 0.0;  // 0: unknown, no scale compensation
        glm::dvec3 anchor{0.0}; // the bound surface point at rest
    };
    std::vector<uint32_t> offsets{0};
    std::vector<Influence> influences;
    std::vector<uint32_t> driverPoints; // per driver
};

enum class WrapMode : uint8_t
{
    Offset,  // the offset turns with the surface
    Surface, // the surface's movement is added, the offset never turns
    Snap,    // onto the surface
};

struct ProximityParams
{
    int maxDrivers = 4;
    double falloff = 2.0;     // triangles within this many times the nearest distance count
    int smoothIterations = 0; // influence spreading over the target's edges
    double smoothAmount = 0.5;
};

/** Drivers and targets in the same space. targetAdjacency is needed only for smoothing. */
LILA_SCENE_API ProximityBinding proximityBind(std::span<const geo::Mesh* const> drivers, std::span<const glm::dvec3> targets, const ProximityParams& o,
                                              const Adjacency* targetAdjacency = nullptr);
LILA_SCENE_API std::vector<glm::dvec3> proximityApply(const ProximityBinding& b, std::span<const std::span<const glm::dvec3>> drivers,
                                                      std::span<const glm::dvec3> targets, WrapMode mode, double scaleCompensation);

// ------------------------------------------------------------------------------------------------ collision

/** The segment a..b swept by a sphere. */
struct Capsule
{
    glm::dvec3 a{0.0}, b{0.0};
    double radius = 0.0;
};

struct CollisionParams
{
    double offset = 0.0;      // kept clear of each collider
    double falloff = 0.0;     // points this much farther than offset ease out instead of stopping hard
    double volumeKeep = 0.0;  // 0 to 1: the pushed amount bulges out around the contact
    double bulgeRadius = 0.0; // 0: four times the deepest push
    bool keepInside = false;  // colliders hold points in instead of out
};

LILA_SCENE_API std::vector<glm::dvec3> collide(const geo::Mesh& topology, std::span<const glm::dvec3> points, std::span<const SurfaceTarget* const> meshes,
                                               std::span<const Capsule> capsules, const CollisionParams& o);

// ------------------------------------------------------------------------------------------------ rigidify

/** Region per point: connected components of the masked points over edges, -1 where unmasked. Empty mask: all points. */
LILA_SCENE_API std::vector<int32_t> connectedRegions(const Adjacency& adj, std::span<const uint8_t> mask = {});

/**
 * Each region (ids >= 0) moves by the best rigid fit of its rest points to their deformed ones. Points within
 * blendRings edge rings of a region ease from that rigid motion into their own deformation.
 */
LILA_SCENE_API std::vector<glm::dvec3> rigidify(const Adjacency& adj, std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed,
                                                std::span<const int32_t> region, int blendRings);

// ------------------------------------------------------------------------------------------------ skin slide

struct SlideParams
{
    int relaxIterations = 4;
    double relaxStep = 0.5;
    double normalAmount = 0.0; // how much of the movement off the rest surface is kept
};

/** restSurface is the rest shape (same topology as rest). The deformation slides along it and is relaxed there. */
LILA_SCENE_API std::vector<glm::dvec3> skinSlide(const Adjacency& adj, std::span<const glm::dvec3> rest, const SurfaceTarget& restSurface,
                                                 std::span<const glm::dvec3> deformed, const SlideParams& o);

// ------------------------------------------------------------------------------------------------ pins

/** A point on a mesh's surface: a triangle by its point ids and weights, with its corners' uvs when the mesh has them. */
struct SurfacePoint
{
    uint32_t points[3]{};
    glm::dvec3 bary{0.0};
    glm::dvec2 uv[3]{};
    bool hasUv = false;
};

/** The surface point at uv, if a triangle covers it (the nearest when several overlap, by triangle order). */
LILA_SCENE_API bool locateUv(const geo::Mesh& mesh, std::string_view uvSet, const glm::dvec2& uv, SurfacePoint& out);
/** The surface point nearest p. */
LILA_SCENE_API bool locateNearest(const geo::Mesh& mesh, const glm::dvec3& p, std::string_view uvSet, SurfacePoint& out);
/** Origin on the point, z along the triangle's normal, x along the direction u grows (else the triangle's first edge). */
LILA_SCENE_API glm::dmat4 surfaceFrame(std::span<const glm::dvec3> points, const SurfacePoint& sp);
} // namespace lila::deform
