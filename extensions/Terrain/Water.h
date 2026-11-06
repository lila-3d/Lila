#pragma once

#include "Analysis.h"
#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace terrain
{
/**
 * A river or stream carved into the terrain. Its bed falls monotonically from source to mouth; width and depth follow its
 * drainage area unless given. Lengths cm and points in the heightfield's own space; areas m2.
 */
struct RiverSpec
{
    std::string id;
    /** path (a drawn polyline), object (a curve through an Object Info node, drawn or imported) or auto (every channel past threshold). */
    std::string source = "path";
    std::vector<glm::dvec2> path;
    std::string object;
    /** auto: drainage area past which a channel is a river. */
    double threshold = 1.0e6;
    /** auto: springs (rivers with nothing upstream) shorter than this are left out. */
    double minLength = 0.0;
    /** Drawn: catchment above the first point, for a river entering from off the map. */
    double upstreamArea = 0.0;
    /** Fixed width and depth; 0 takes them from drainage. */
    double width = 0.0;
    double depth = 0.0;
    double widthScale = 1.0;
    double depthScale = 1.0;
    double minWidth = 150.0;
    double minDepth = 40.0;
    /** Meander amplitude in widths (0 is straight); wavelength in widths. Only flat stretches meander fully. */
    double meander = 0.0;
    double meanderLength = 14.0;
    uint32_t seed = 0;
    /** Bank slope from the water's edge, degrees, and how far out banks are cut, cm (0: four widths). */
    double bankAngle = 30.0;
    double bankWidth = 0.0;
    /** Erosion steps run around the carved channel so its banks settle into the ground; 0 is off. */
    int reErode = 6;
    double reErodeStrength = 0.0005;
    bool gravelBars = true;
    /** Waterfalls where the ground falls steeper than cliffAngle by more than waterfallHeight, with a plunge pool below. */
    bool waterfalls = true;
    double waterfallHeight = 300.0;
    double cliffAngle = 55.0;
    /** Plunge pool depth as a fraction of the drop, at most three channel depths. */
    double plungeDepth = 0.4;
    /** Fixed flow speed, m/s; 0 works it out from depth and slope. */
    double flowSpeed = 0.0;
    /** Drawn: keep the drawn direction even when it runs uphill. */
    bool keepDirection = false;

    nlohmann::json toJson() const;
    /** Throws std::invalid_argument on a bad source. */
    static RiverSpec fromJson(const nlohmann::json& j);
};

/** A lake or pond: the water at a level over a basin. */
struct LakeSpec
{
    std::string id;
    /** point (the basin under at), region (a drawn polygon), object (a closed curve) or auto (every basin past minDepth and minArea). */
    std::string source = "point";
    glm::dvec2 at{0.0};
    std::vector<glm::dvec2> polygon;
    std::string object;
    /** Water level; NaN is the basin's spill level (a region: the lowest ground around it). */
    double level;
    /** auto: shallowest basin and smallest area (m2) that become lakes. */
    double minDepth = 50.0;
    double minArea = 2000.0;
    /** region: depth dug below the level where the ground inside is higher, and the bed's slope from the shore, degrees. */
    double depth = 300.0;
    double shoreSlope = 12.0;
    /** region with a level above the lowest ground around it: raise that ground into a dam instead of lowering the level. */
    bool dam = true;

    LakeSpec();
    nlohmann::json toJson() const;
    static LakeSpec fromJson(const nlohmann::json& j);
};

/** Sea level and the shore and pit settings for all the water of a terrain. */
struct WaterSettings
{
    /** NaN: no sea. Ground below it and joined to the edge is sea. */
    double seaLevel;
    /** Shore band width, cm; beaches are shore no steeper than beachSlope (degrees) and at most beachHeight above the water. */
    double shoreWidth = 1500.0;
    double beachSlope = 10.0;
    double beachHeight = 300.0;
    /** Fill basins no lake holds so every sample drains to the edge, the sea or a lake. */
    bool drainPits = true;

    WaterSettings();
    nlohmann::json toJson() const;
    static WaterSettings fromJson(const nlohmann::json& j);
};

struct WaterSpec
{
    std::vector<RiverSpec> rivers;
    std::vector<LakeSpec> lakes;
    WaterSettings settings;
    bool empty() const { return rivers.empty() && lakes.empty() && std::isnan(settings.seaLevel); }
};

/** One point along a carved river, in the heightfield's space. */
struct WaterPoint
{
    glm::dvec2 p{0.0};
    /** Unit direction of flow across the ground. */
    glm::dvec2 dir{1.0, 0.0};
    double distance = 0.0;
    double bed = 0.0;
    double level = 0.0;
    double width = 0.0;
    double depth = 0.0;
    /** m/s and m3/s. */
    double speed = 0.0;
    double discharge = 0.0;
    double area = 0.0;
};

struct RiverResult
{
    /** The spec's id; auto rivers add _<n>. */
    std::string id;
    std::string spec;
    std::vector<WaterPoint> points;
    /** edge, sea, lake, river (joins another) or pond (ends inland, where a pond was made). */
    std::string mouth;
    /** Index of the river it flows into, or -1. */
    int downstream = -1;
    /** Starts at a spring rather than a confluence or a lake. */
    bool spring = true;
    bool reversed = false;
    /** Deepest the channel cut below the ground, cm. */
    double maxCut = 0.0;
};

struct LakeResult
{
    std::string id;
    std::string spec;
    /** lake, pond (a drawn river's end), pool (below a waterfall), auto or sea. */
    std::string kind = "lake";
    double level = 0.0;
    /** Where the water leaves: the lowest ground on its rim; NaN for the sea. */
    glm::dvec2 spill{0.0};
    double spillHeight = 0.0;
    std::vector<uint32_t> cells;
    std::vector<std::vector<glm::dvec2>> shoreline;
    double area = 0.0;
    double maxDepth = 0.0;
    /** m3. */
    double volume = 0.0;
    /** region with dam: how high the rim was raised, cm. */
    double damHeight = 0.0;
};

struct Waterfall
{
    std::string river;
    glm::dvec3 top{0.0};
    glm::dvec3 bottom{0.0};
    double drop = 0.0;
    /** The plunge pool's lake id; empty when none formed. */
    std::string pool;
};

struct Spring
{
    std::string river;
    glm::dvec3 at{0.0};
};

struct WaterResult
{
    std::vector<RiverResult> rivers;
    std::vector<LakeResult> lakes;
    std::vector<Waterfall> waterfalls;
    std::vector<Spring> springs;
    std::vector<std::string> warnings;
    /** Basins filled so they drain, and the volume added, m3. */
    int pitsFilled = 0;
    double pitVolume = 0.0;
    /** Volumes moved, m3. */
    double cut = 0.0;
    double fill = 0.0;
};

/** Open polylines and closed rings (last point not repeated) of each curve object, by Object Info node name, in heightfield space. */
struct WaterObjects
{
    std::map<std::string, std::vector<std::vector<glm::dvec2>>> lines;
    std::map<std::string, std::vector<std::vector<glm::dvec2>>> rings;
};

/** Mask names the water writes: riverBed, riverBank, gravelBar, waterfall, lakeBed, sea, water, shore, beach. */
const std::vector<std::string>& waterMasks();

/**
 * Applies the water to h: sea, rivers (carved with a monotonic bed, banks re-eroded, gravel bars, waterfalls and plunge
 * pools), lakes at their levels, then fills basins no water holds. Writes the water masks and refreshes flow and lakes.
 * Deterministic at any thread count.
 */
WaterResult applyWater(Heightfield& h, const WaterSpec& spec, const WaterObjects& objects = {});


/** What a Terrain Water Features node (by name) of a modifier made on its last evaluation. */
void publishWater(uint64_t modifier, const std::string& node, WaterResult r);
bool publishedWater(uint64_t modifier, const std::string& node, WaterResult* out);

/**
 * The water as placement data in world space (cm): splines (rivers with per-point width, depth, bedHeight, waterLevel,
 * flowSpeed, discharge and flow direction; lake and sea shorelines with waterLevel) and locators (lakeLevel, seaLevel,
 * spring, waterfallTop, waterfallBottom). This is the file format of <name>_water.json.
 */
nlohmann::json waterJson(const WaterResult& r, const WorldMap& w, double seaLevel);

/** setWater's parameters for LilaLink: the same splines and locators in Unreal space, keyed under id. */
nlohmann::json unrealWaterParams(const nlohmann::json& water, const std::string& id, const std::string& label);

/** A flat preview of the water surfaces in world space: river ribbons, lake and sea cells at their levels. maxQuads bounds it. */
lila::geo::Mesh waterPreviewMesh(const Heightfield& h, const WaterResult& r, const glm::dmat4& world, size_t maxQuads = 200000);

/** The water node type, its commands and the export hooks. */
void registerWater(lila::ext::Context& ctx);
/** Writes <name>_water.json next to a terrain export when the terrain has water; null when it has none. */
nlohmann::json exportTerrainWater(const std::string& terrain, const std::filesystem::path& folder, const std::string& name);
/** Sends a terrain's water to Unreal as spline and locator actors; null when it has none. Throws std::runtime_error. */
nlohmann::json sendTerrainWater(const std::string& terrain, double timeoutSeconds);
} // namespace terrain
