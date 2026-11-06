#pragma once

#include "Erosion.h"
#include "Terrain.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace terrain
{
/** Elevation data as read from disk: rows from north to south, heights in metres, NaN where the data has a void. */
struct Dem
{
    int width = 0;
    int height = 0;
    std::vector<float> metres;
    /** Coordinates are degrees of longitude and latitude; otherwise projected metres (easting, northing). */
    bool geographic = false;
    /** Coordinates of the first (north-west) sample's centre. */
    double x0 = 0.0;
    double y0 = 0.0;
    /** Spacing between samples east and south, in the coordinates' units; 0 when the file doesn't say. */
    double dx = 0.0;
    double dy = 0.0;
    /** 1 where fillVoids filled a void. */
    std::vector<uint8_t> filled;
    /** EPSG code from the GeoTIFF's geokeys (4326, 326zz UTM north ...); 0 when unknown. */
    int epsg = 0;
};

enum class DemFormat : uint8_t
{
    Auto,
    GeoTiff,
    Hgt,
    Asc,
    Xyz,
    Terrarium,
    TerrainRgb,
};

const std::vector<std::string>& demFormatNames();
bool parseDemFormat(std::string_view name, DemFormat* out);

/**
 * Reads elevation data: GeoTIFF (classic TIFF, strips or tiles, uncompressed, deflate or LZW, any integer or float
 * samples), SRTM .hgt, ESRI ASCII grid (.asc), XYZ points on a regular grid, or Terrarium and Terrain-RGB PNG tiles. Auto
 * picks by extension (a .png is Terrarium). Throws std::runtime_error.
 */
Dem readDem(const std::filesystem::path& path, DemFormat format = DemFormat::Auto);

/** Whether the file is elevation data rather than an image: .hgt, .asc, .xyz, or a TIFF with geographic tags. */
bool looksLikeElevation(const std::filesystem::path& path);

/** Fills voids smoothly from the data around them. Returns how many samples were filled. */
size_t fillVoids(Dem& d);

struct DemOptions
{
    glm::dvec2 center{0.0};
    /** Grid spacing, cm; 0 keeps the data's own. */
    double cell = 0.0;
    /** Sample spacing for files that carry none (PNG tiles, bare grids), metres. */
    double spacing = 30.0;
    /** Multiplies heights. */
    double exaggeration = 1.0;
    /** Puts the lowest point at 0 instead of sea level. */
    bool fromLowest = false;
};

/**
 * The data as a heightfield in cm, centred on center: +X east, -Z north. Geographic data is flattened around its centre
 * (equirectangular, close enough for areas up to about 100 km). Writes mask "voids" where fillVoids filled.
 */
Heightfield demToHeightfield(const Dem& d, const DemOptions& o);

/** How a painted image becomes terrain. */
enum class ImageMode : uint8_t
{
    /** Brightness is height, smoothed and remapped. */
    Height,
    /** The painting's big shapes, with erosion adding natural detail. */
    Guide,
    /** Brightness is how fast the land rises; erosion carves it as it does. */
    Uplift,
    /** Flat ground, brightness into a mask. */
    Mask,
    /** Colour-coded strokes (ridge, valley, river, flat) over a grey base, kept through erosion as constraints. */
    Sketch,
};

const std::vector<std::string>& imageModeNames();
bool parseImageMode(std::string_view name, ImageMode* out);

struct ImageOptions
{
    ImageMode mode = ImageMode::Guide;
    /** Heights of black and white, cm. */
    double low = 0.0;
    double high = 50000.0;
    /** Blur radius in samples, removing 8-bit steps and brush noise. */
    double smooth = 1.0;
    /** Exponent on brightness: above 1 widens valleys and sharpens peaks. */
    double shape = 1.0;
    /** 0 to 1: how far erosion may move the painting (Guide and Uplift). */
    double erosion = 0.5;
    /** Noise added before erosion, a fraction of the height range, so rivers find their own paths. */
    double detail = 0.03;
    uint32_t seed = 0;
    /** Mask written in Mask mode. */
    std::string mask = "painted";
};

/**
 * Removes the steps of 8- or 16-bit brightness (heights 0 to 1 that are all multiples of 1/255 or 1/65535): smooth ramps
 * that stay within half a level of every pixel, so big shapes are kept exactly. Returns the level size found (0: none).
 */
double dequantize(Heightfield& h);

/** Turns h, whose heights are the image's brightness from 0 to 1, into terrain. */
void interpretImage(Heightfield& h, const ImageOptions& o);
} // namespace terrain
