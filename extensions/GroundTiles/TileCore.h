#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * The Ground Tiles's scatter maths, free of the scene: the parameter schema and presets, the distributions,
 * placement planning and per-instance geometry. Everything here is deterministic for a given seed and settings.
 */
namespace groundtiles
{
using json = nlohmann::json;
namespace geo = lila::geo;

/** The schema and presets as one JSON document: {sections, parameters, presets: [[name, overrides], ...]}. */
const char* builtInTablesJson();

struct Param
{
    std::string name, label, type, section, help;
    json def;
    double min = 0.0, max = 0.0;
    std::vector<std::string> options;
    /** The parameter whose value decides whether this one shows, and the values that show it. */
    std::string showIf;
    json showValues = json::array();
};

const std::vector<Param>& parameters();
const Param* findParam(std::string_view name);
/** (id, label) in display order. */
const std::vector<std::pair<std::string, std::string>>& sections();
const std::vector<std::string>& builtInPresets();
const std::string& defaultPreset();
/** The built-in overrides of a preset, null when there is no such built-in. */
const json* builtInOverrides(const std::string& name);

json defaults();
/** A stored value forced back into the type the schema expects. */
json coerce(const Param& p, const json& value);
/** Partial or stale settings merged onto the defaults. */
json sanitize(const json& values);
/** Full settings for a preset, user saved ones winning over built-ins. */
json presetSettings(const std::string& name, const json& userPresets);
bool isVisible(const Param& p, const json& values);

double num(const json& values, const char* name);
int64_t whole(const json& values, const char* name);
bool flag(const json& values, const char* name);
std::string text(const json& values, const char* name);

/** Mersenne Twister with the uniform, random and randrange the planning reads; the same seed repeats exactly. */
class Random
{
public:
    explicit Random(uint64_t seed) : mGen(seed) {}
    double random() { return double(mGen() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double a, double b) { return a + (b - a) * random(); }
    int64_t randrange(int64_t n) { return n <= 1 ? 0 : std::min<int64_t>(n - 1, int64_t(random() * double(n))); }

private:
    std::mt19937_64 mGen;
};

/** Tile-local scatter positions for the configured distribution, before edge wrapping. */
std::vector<glm::dvec2> scatterPoints(const json& values, Random& rng);
/** The point plus the copies it needs on the opposite edges when it overhangs them by margin. */
std::vector<glm::dvec2> wrappedPositions(double x, double y, double half, double margin);
/** How many times over the cluster discs blanket the tile. */
double clusterCoverage(const json& values);
std::string describeClustering(const json& values);
/** The height the lowest instances sit at, ignoring bands nothing lands in: where the bake plane goes. */
double lowestHeight(const json& values);
/** Whether any option rebuilds the mesh, which rules out shared meshes. */
bool modifiesGeometry(const json& values);

/** "Reeds & River Grass" to ReedsRiverGrass, for file and material names. */
std::string presetStem(const std::string& preset);
/** "Reeds & River Grass" to Reeds_River_Grass, for node names. */
std::string nodeStem(const std::string& preset);

inline constexpr const char* kGroupSuffix = "_GroundTile_GRP";
inline constexpr const char* kBakePlaneSuffix = "_BakePlane";
inline constexpr const char* kGroundSuffix = "_Ground";
inline constexpr const char* kBakeMaterialSuffix = "_BakePlaneMtl";
inline constexpr const char* kColourSet = "terrainMasks";
inline constexpr const char* kIdMaterialPrefix = "terrainID_";
inline constexpr const char* kHighSuffix = "_Hi";
inline constexpr const char* kLowSuffix = "_Lo";
inline constexpr const char* kIdsSuffix = "_ids";
inline constexpr const char* kColoursSuffix = "_colours";
inline constexpr const char* kIdMapSuffix = "_id";
inline constexpr std::array<const char*, 3> kMaskSuffixes{"_maskR", "_maskG", "_maskB"};

/** Negative ids are the ground, which reads as empty tile: black. */
glm::dvec3 defaultIdColour(int64_t id);
glm::dvec3 idColour(int64_t id, const json& palette);
/** Node names cannot carry a minus sign, so negative ids get an n: terrainID_n1. */
std::string idMaterialName(int64_t id);
std::string toHex(const glm::dvec3& colour);
/** {"<id>": {"colour": [r, g, b], "sources": [names]}} over the enabled sources, plus the ground when it is on. */
json idLegend(const json& activeSources, const json& palette, const json& values);
/** The hand-editable layer palette per id, seeded from the default recipes: {"<id>": {base, root, ..., roughness}}. */
nlohmann::ordered_json defaultColourRecipes(const json& legend);

inline constexpr std::array<const char*, 3> kAxisNames{"X", "Y", "Z"};

/** Which local axis of a source is its length, width and thickness, and which end of the length is the base. */
struct Axes
{
    glm::dvec3 min{0.0}, max{0.0}, extents{0.0};
    int length = 0, width = 1, thickness = 2;
    /** +1 when the base is the low end of the length axis. */
    double baseSign = 1.0;
};

/** lengthAxis Auto|X|Y|Z, baseEnd Auto|Min|Max. */
Axes sourceAxes(const glm::dvec3& boxMin, const glm::dvec3& boxMax, const std::string& lengthAxis = "Auto",
                const std::string& baseEnd = "Auto");
/** "grows +X, thin on Z". */
std::string describeAxes(const Axes& axes);
/** Farthest any point of the source's box can sit from its pivot, in world units: what an instance overhangs by. */
double pivotReach(const glm::dvec3& boxMin, const glm::dvec3& boxMax, const glm::dvec3& pivot, const glm::dmat4& world);

/** A deformer handle already on a source, reused instead of the derived placement: its world matrix and bounds. */
struct AuthoredHandle
{
    glm::dmat4 world{1.0};
    double low = 0.0, high = 2.0;
};

enum HandleKind
{
    kBend,
    kTwist,
    kSine,
    kFlare,
    kHandleKinds
};

/** One scatter source, read from the scene before planning. */
struct Source
{
    std::string name;
    int64_t objectId = 0;
    double heightOffset = 0.0;
    double weight = 1.0;
    /** Evaluated and compacted, in object space. */
    geo::Mesh mesh;
    glm::dmat4 world{1.0};
    glm::dvec3 rotatePivot{0.0}, scalePivot{0.0};
    Axes axes;
    double reach = 0.0;
    std::array<std::optional<AuthoredHandle>, kHandleKinds> authored;
    /** Rest pose 0 at the root to 1 at the tip, per point. */
    std::vector<float> lengthParams;
};

/** Fills axes, reach and lengthParams from the mesh, world matrix, pivots and the source's overrides. */
void prepareSource(Source& s, const std::string& lengthAxis, const std::string& baseEnd);

/** Every random choice one scattered instance gets. Wrapped copies share it, so their geometry matches exactly. */
struct Placement
{
    size_t source = 0;
    size_t scatterIndex = 0;
    double x = 0.0, y = 0.0, height = 0.0;
    int64_t seed = 0;
    double variation = 0.0, band = 0.0, scale = 1.0;
    double lengthFactor = 1.0, widthFactor = 1.0, thicknessFactor = 1.0;
    double yaw = 0.0, tiltA = 0.0, tiltB = 0.0;
    double bend = 0.0, twist = 0.0, sine = 0.0, sineOffset = 0.0, flareStart = 1.0, flareEnd = 1.0, flatten = 1.0;
    double shearXY = 0.0, shearXZ = 0.0, shearYZ = 0.0;
};

struct Plan
{
    /** One per built instance, wrapped copies included, in build order. */
    std::vector<Placement> placements;
    size_t scatterPoints = 0;
    std::vector<std::string> warnings;
};

/** Picks a source, band, scale, rotation and deformation for every scatter point, and the wrapped copies. */
Plan plan(const json& values, const std::vector<Source>& sources);

/** One instance's geometry: points in world space, and the node matrix a separate instance node takes. */
struct InstanceGeometry
{
    /** Per point of the source mesh, world space, flattened when that is on. */
    std::vector<glm::dvec3> world;
    /** World matrix for the instance node: placement, tilt and spin over the source's own transform (no shear). */
    glm::dmat4 nodeMatrix{1.0};
    /** Per point terrainMasks colour: R root to tip, G per instance, B height band. Empty when vertex colours are off. */
    std::vector<glm::vec4> colours;
};

/** Deforms, scales, places and snaps one instance. Pure, so instances build in parallel. */
InstanceGeometry buildInstance(const json& values, const Source& source, const Placement& p);

/** Points of the deformation alone, object space, before scale and placement: what a separate instance node keeps. */
std::vector<glm::dvec3> deformPoints(const json& values, const Source& source, const Placement& p);

/** Every instance, built on the TBB pool; index i belongs to placements[i]. */
std::vector<InstanceGeometry> buildAll(const json& values, const std::vector<Source>& sources, const Plan& plan);

/** A tile-sized flat plane with 0-1 UVs centred on the origin, facing up, subdivided per side. */
geo::Mesh tilePlane(double tileSize, const std::string& upAxis, int subdivisions);

/** The top of the tile sampled straight down: p50, p99 and the tallest point that is over the plane, above the floor. */
struct HeightProfile
{
    double p50 = 0.0, p99 = 0.0, top = 0.0;
    size_t hits = 0;
};
/** heights must hold the hit heights above the floor; sorts them. */
HeightProfile profileOf(std::vector<double>& heights);

/** What the vertex colour map file of a tile is called; the suffixes bakers name it with. */
inline constexpr std::array<const char*, 4> kVertexMapSuffixes{"vertexcolor", "vertexcolour", "vertex_color", "vertexcolormap"};
inline constexpr std::array<const char*, 8> kBakeExtensions{".psd", ".png", ".tga", ".tif", ".tiff", ".exr", ".jpg", ".jpeg"};
/** The mask stem for a vertex colour map path: "C:/x/Grass_vertexcolor.png" gives "C:/x/Grass". */
std::string maskStem(const std::string& mapPath);
} // namespace groundtiles
