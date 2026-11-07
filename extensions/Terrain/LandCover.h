#pragma once

#include "Fetch.h"
#include "Inputs.h"
#include "Projection.h"
#include "Terrain.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace terrain
{
/** ESA WorldCover 2021 v200 tiles (3 x 3 degrees, named by the south-west corner, e.g. N45E006) covering the box. */
std::vector<std::string> worldCoverTileNames(const GeoBox& b);
std::string worldCoverUrl(const std::string& tile);
/** The landcover_<class> terrainImportOsm writes for a WorldCover code; empty for no data or an unknown code. */
std::string worldCoverClass(int code);
/** The credit the CC BY 4.0 licence requires. */
const char* worldCoverAttribution();

/** Fetches count bytes of url from offset into out; returns the HTTP status (206 or 200), or 0 with why set. */
using RangeDownloader = std::function<int(const std::string& url, uint64_t offset, uint64_t count, std::vector<uint8_t>* out, std::string* why)>;

struct LandCoverFetch
{
    /** Class codes in degrees on WorldCover's own grid over the box, 0 where there is no data. */
    Dem codes;
    int tiles = 0;
    /** Tiles WorldCover has none of (open sea). */
    int empty = 0;
    uint64_t bytes = 0;
};

/**
 * Reads the box from each WorldCover tile over HTTP ranges (the tiles are cloud-optimised GeoTIFFs, so only the parts
 * covering the box are downloaded) and merges them. Throws std::runtime_error.
 */
LandCoverFetch fetchWorldCover(const GeoBox& b, const RangeDownloader& download);

/**
 * The pixel window [x0, y0, x1, y1) of a raster (as GeoTiffWindowReader::info gives it, in degrees or UTM) that covers
 * the heightfield placed by g, with a margin; empty (x0 == x1) when they don't overlap. Throws std::runtime_error for a
 * projection Lila doesn't convert.
 */
std::array<int, 4> rasterWindow(const Dem& info, const Heightfield& h, const Georef& g);

/**
 * WorldCover codes onto the heightfield by nearest sample, as codes must never blend: mask holds code / 255 (what
 * terrainBiomes' landcover reads) and, with classes, landcover_<class> is 1 where that class is, as terrainImportOsm
 * writes. Returns the share of the heightfield per class, and "nodata" for samples without a code.
 */
std::map<std::string, double> rasterizeWorldCover(Heightfield& h, const Dem& codes, const Georef& g, const std::string& mask, bool classes);
} // namespace terrain
