#pragma once

#include <lila/Heightfield.h>
#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

/**
 * Seamwork: where objects meet (the ground, each other, tight creases), found exactly from intersections, as curves with
 * per-point facts, sampled points, distance fields and masks. Nothing here knows about the scene graph.
 */
namespace seamwork
{
using lila::geo::Heightfield;
using lila::geo::Mesh;
using lila::geo::MeshBvh;

inline constexpr double kPi = 3.14159265358979323846;

enum class Kind : uint8_t
{
    Ground,
    Mesh,
    Crease,
};
const char* kindName(Kind k);
bool parseKind(const std::string& name, Kind* out);

/** Object index of the terrain heightfield in a curve's b; kNone for a crease, which has no second object. */
inline constexpr int kTerrain = -1;
inline constexpr int kNone = -2;

/** One mesh in world space. Instances share mesh and bvh. */
struct Object
{
    std::string name;
    std::shared_ptr<const Mesh> mesh;
    std::shared_ptr<const MeshBvh> bvh;
    glm::dmat4 world{1.0};
    /** Pieces sharing a non-zero group never meet each other (one object split in parts, or meant to overlap); 0 is the object's own. */
    int group = 0;
    /** Ground: its joins with other objects are ground joins; two grounds never meet. */
    bool ground = false;
    bool ignore = false;
};

struct Ground
{
    std::shared_ptr<const Heightfield> field;
    glm::dmat4 world{1.0};
    std::string name;
};

/** Short-range occlusion rays for the crease mask: true when anything lies within tMax of origin along dir (unit, world). */
using OcclusionFn = std::function<bool(const glm::dvec3& origin, const glm::dvec3& dir, double tMax)>;

struct Options
{
    bool ground = true;
    bool mesh = true;
    /** Near contacts: surfaces within gap of each other without crossing (a wall standing on the ground). */
    bool nearContact = true;
    bool crease = false;
    /** cm. */
    double gap = 2.0;
    /** Curves shorter than this, cm, are dropped. */
    double minLength = 0.5;
    double weld = 1e-3;
    int exposureRays = 16;
    /** How far, cm, something blocks the sky for exposure. */
    double exposureRange = 2000.0;
    /** How far, cm, crease rays look. */
    double creaseRange = 20.0;
    /** Occlusion above which a point counts as in a crease, 0 to 1. */
    double creaseThreshold = 0.4;
    int creaseRays = 24;
    glm::dvec3 up{0.0, 1.0, 0.0};
    /** Object pairs (indices, either order) never joined. */
    std::vector<std::pair<int, int>> ignorePairs;
    /** Rays for the crease mask; null traces the objects' own trees on the CPU. */
    OcclusionFn occluded;
    /**
     * Where the crease mask is traced without occluded: auto (the bake's GPU AO when the renderer registered it, else
     * the CPU), gpu, cpu, or bakeCpu (the GPU path's samples through the bake's CPU kernel, its reference).
     */
    std::string creaseBackend = "auto";
};

/** A join between two surfaces as a polyline in world space, with one value per point in each array. */
struct Curve
{
    Kind kind = Kind::Mesh;
    int a = 0;
    /** Object index, kTerrain or kNone. */
    int b = kTerrain;
    bool closed = false;
    /** Found within the gap tolerance rather than from a crossing. */
    bool nearContact = false;
    std::vector<glm::dvec3> points;
    std::vector<glm::dvec3> normalA, normalB;
    /** Air wedge between the surfaces, degrees: 90 for a wall on flat ground, near 0 in a tight crease. */
    std::vector<double> angle;
    /** How far a goes into b below the point, cm. */
    std::vector<double> depth;
    /** Distance between the surfaces, cm; 0 where they cross. */
    std::vector<double> gap;
    /** How much the wedge opens upward, 0 to 1. */
    std::vector<double> upFacing;
    /** Sky visibility, 0 to 1. */
    std::vector<double> exposure;
    /** Index into Result::layers of the heaviest terrain layer, -1 for none. */
    std::vector<int> layer;

    double length() const;
};

struct PairStats
{
    int a = 0;
    int b = 0;
    Kind kind = Kind::Mesh;
    size_t curves = 0;
    double length = 0.0;
};

struct Result
{
    std::vector<Curve> curves;
    /** Terrain layers (layer_<name> masks), by index. */
    std::vector<std::string> layers;
    /** Per object, per point: crease occlusion 0 to 1 (empty unless crease was on). */
    std::vector<std::vector<float>> crease;
    /** rt or compute (the bake's GPU AO), cpu, or custom (the caller's occlusion function). */
    std::string creaseBackend;
    std::vector<std::string> warnings;
    size_t crossings = 0;

    double totalLength() const;
    std::vector<PairStats> pairs() const;
};

/**
 * Crease occlusion (0 open, 1 enclosed) at each given point (world position and unit normal per object, empty lists
 * skipped) through the bake backend's AO kernel, every object and the ground near them as occluders. Returns the
 * backend's mode (rt, compute) or an empty string, with why in note, when no GPU bake backend ran.
 */
std::string bakeCrease(std::span<const Object> objects, const Ground* ground, const std::vector<std::vector<glm::dvec3>>& points,
                       const std::vector<std::vector<glm::dvec3>>& normals, const Options& o, std::vector<std::vector<float>>& out, std::string& note,
                       const std::string& backend = "gpu");

/** Every join between the objects and with the ground (when given). Deterministic at any thread count. */
Result findSeams(std::span<const Object> objects, const Ground* ground, const Options& o);

/** Sky visibility at a point: the share of cosine-weighted rays over the up hemisphere that reach range unblocked. */
double exposureAt(std::span<const Object> objects, const Ground* ground, const glm::dvec3& p, const glm::dvec3& up, int rays, double range);

// Bands: widths in world cm with a falloff and world noise, used everywhere a seam band is.

enum class Falloff : uint8_t
{
    Linear,
    Smooth,
    Sharp,
    Root,
    Custom,
};
const std::vector<std::string>& falloffNames();

struct Band
{
    double width = 30.0;
    Falloff falloff = Falloff::Smooth;
    /** Custom profile "d:w d:w ...", d 0 at the join to 1 at the band's edge, w the weight there. */
    std::string curve;
    /** Width varies by up to this fraction with world noise. */
    double noise = 0.0;
    double noiseScale = 150.0;
    uint32_t seed = 0;

    /** 1 at the join falling to 0 at width (moved by noise at pos). */
    double weight(double distance, const glm::dvec3& pos) const;
};

/** Gradient noise in about -1 to 1. */
double noise3(const glm::dvec3& p, uint32_t seed);

/** Segments of a set of curves, for nearest-distance queries. */
class SegmentIndex
{
public:
    /** Curves passing filter (all when empty). */
    SegmentIndex(const Result& r, const std::function<bool(const Curve&)>& filter = {});
    explicit SegmentIndex(std::vector<std::pair<glm::dvec3, glm::dvec3>> segments);
    bool empty() const { return mSegs.empty(); }
    /** Distance to the nearest segment, capped at maxDistance (returned when none is closer). */
    double distance(const glm::dvec3& p, double maxDistance, glm::dvec3* closest = nullptr, size_t* segment = nullptr) const;
    const std::vector<std::pair<glm::dvec3, glm::dvec3>>& segments() const { return mSegs; }
    /** Curve and segment-in-curve of segment s. */
    std::pair<size_t, size_t> origin(size_t s) const { return mOrigin[s]; }

private:
    void build();
    std::vector<std::pair<glm::dvec3, glm::dvec3>> mSegs;
    std::vector<std::pair<size_t, size_t>> mOrigin;
    double mCell = 50.0;
    glm::dvec3 mMin{0.0};
    glm::ivec3 mDims{0};
    std::vector<uint32_t> mCellStart;
    std::vector<uint32_t> mCellSegs;
};

/** Per point of the object: distance to the nearest join it takes part in (world cm), capped at maxDistance. */
std::vector<float> distanceField(const Object& obj, int objectIndex, const Result& r, double maxDistance);

/** Writes the band weight around ground joins into the heightfield's mask (in the heightfield's own space via world). */
void contactMask(Heightfield& h, const glm::dmat4& world, const Result& r, const Band& band, const std::string& mask = "contact");

// Points along joins.

struct SeamPoint
{
    glm::dvec3 position{0.0};
    glm::dvec3 tangent{1.0, 0.0, 0.0};
    glm::dvec3 normalA{0.0, 1.0, 0.0};
    glm::dvec3 normalB{0.0, 1.0, 0.0};
    double angle = 90.0;
    double depth = 0.0;
    double gap = 0.0;
    double upFacing = 0.0;
    double exposure = 1.0;
    int layer = -1;
    int a = 0;
    int b = kTerrain;
    Kind kind = Kind::Mesh;
    size_t curve = 0;
    /** Distance along the curve, cm. */
    double along = 0.0;
};

/** Points every spacing cm along each curve, values interpolated between the curve's points. */
std::vector<SeamPoint> samplePoints(const Result& r, double spacing);

/** Bisector of the two normals: the direction out of the wedge. */
glm::dvec3 bisector(const SeamPoint& p, const glm::dvec3& up);

/** Iso-lines of a per-point scalar over a mesh's triangles at a level, world space, chained. */
std::vector<std::vector<glm::dvec3>> isoLines(const Mesh& m, const MeshBvh& bvh, const glm::dmat4& world, std::span<const float> values, double level,
                                              std::vector<uint8_t>* closed = nullptr);

/** Per-point normals of a mesh: area-weighted face normals, object space. */
std::vector<glm::dvec3> pointNormals(const Mesh& m);
} // namespace seamwork
