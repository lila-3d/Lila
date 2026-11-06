#pragma once

#include "Projection.h"
#include "Terrain.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace terrain
{
using OsmTags = std::map<std::string, std::string, std::less<>>;

/** A point of a way: its node id (or a key made from its position when the data gave positions only) and where it is. */
struct OsmPoint
{
    int64_t key = 0;
    LatLon at;
};

/** OpenStreetMap data as read: ways resolved to positions, relations listing their member ways. */
struct OsmData
{
    struct Way
    {
        int64_t id = 0;
        std::vector<OsmPoint> points;
        OsmTags tags;
    };
    struct Member
    {
        /** 'n' node, 'w' way, 'r' relation. */
        char type = 'w';
        int64_t ref = 0;
        std::string role;
        /** Positions given inline (Overpass "out geom"), used when the way itself isn't in the data. */
        std::vector<OsmPoint> points;
    };
    struct Relation
    {
        int64_t id = 0;
        std::vector<Member> members;
        OsmTags tags;
    };
    std::vector<Way> ways;
    std::vector<Relation> relations;
    size_t nodes = 0;
    /** Ways that named nodes the data doesn't have (cut off at the extract's edge); their known part is kept. */
    size_t missingNodes = 0;
};

/** .osm XML. Throws std::runtime_error. */
OsmData parseOsmXml(std::string_view text);
/** Overpass API JSON ("out body" with nodes, or "out geom"). */
OsmData parseOverpassJson(std::string_view text);
/** .osm.pbf (zlib or uncompressed blobs). */
OsmData parseOsmPbf(std::string_view bytes);
/** By extension: .osm/.xml, .json, .pbf. */
OsmData readOsm(const std::filesystem::path& path);

enum class OsmFeatureKind : uint8_t
{
    Road,
    Path,
    River,
    Stream,
    Lake,
    Coastline,
};

const std::vector<std::string>& osmFeatureKindNames();

/** A line or closed outline for a terrain feature, in degrees. */
struct OsmFeature
{
    OsmFeatureKind kind = OsmFeatureKind::Road;
    /** The way or relation id. */
    int64_t id = 0;
    std::string name;
    /** The OSM class: the highway or waterway value, "lake", "reservoir", "coastline". */
    std::string cls;
    /** Full width, cm: the width tag, lanes x 3.5 m, or a default for the class. */
    double width = 0.0;
    bool closed = false;
    std::vector<LatLon> points;
};

/** Land cover from OSM tags: rings of one polygon, outer and inner (holes), filled even-odd. */
struct OsmArea
{
    /** forest, grassland, farmland, scrub, rock, sand, urban, wetland, water or glacier. */
    std::string cls;
    int64_t id = 0;
    std::vector<std::vector<LatLon>> rings;
};

const std::vector<std::string>& landcoverClasses();

struct OsmExtract
{
    std::vector<OsmFeature> features;
    std::vector<OsmArea> areas;
};

/** Roads, paths, rivers, streams, lakes and coastline as features, and land cover as areas. Multipolygons are assembled. */
OsmExtract extractOsm(const OsmData& d);

/** A feature's line on the heightfield (cm, XZ), cut where it leaves the heightfield; each run is its own line. */
std::vector<std::vector<glm::dvec2>> clipToHeightfield(const std::vector<glm::dvec2>& line, const Heightfield& h, bool closed);

/** Fills masks landcover_<class> (1 inside) for every class present. Returns the covered fraction per class. */
std::map<std::string, double> rasterizeLandcover(Heightfield& h, const std::vector<OsmArea>& areas, const Georef& g);

/** The ODbL attribution a game or film using the data must show. */
const char* osmAttribution();
} // namespace terrain
