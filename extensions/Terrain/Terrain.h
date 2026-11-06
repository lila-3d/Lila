#pragma once

#include <lila/GeometryNodes.h>

#include <lila/Heightfield.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace terrain
{
using lila::geo::Heightfield;

/** How a new value combines with the height (or mask) already there. */
enum class Blend : uint8_t
{
    Replace,
    Add,
    Subtract,
    Multiply,
    Max,
    Min,
};

const std::vector<std::string>& blendNames();
bool parseBlend(std::string_view name, Blend* out);
/** old combined with value, then mixed back toward old by 1 - factor. */
float blend(Blend mode, float old, float value, float factor);

/** Fractal noise over world XZ. Lengths and heights are in scene units (cm). */
struct NoiseParams
{
    double featureSize = 40000.0;
    double amplitude = 15000.0;
    int octaves = 7;
    double roughness = 0.5;
    double lacunarity = 2.0;
    /** 0 rolling hills, 1 sharp ridges. */
    double ridged = 0.5;
    /** Domain warp, as a fraction of featureSize. */
    double warp = 0.25;
    uint32_t seed = 0;
};

/** Gradient noise in about -1 to 1. */
double gradientNoise(double x, double z, uint32_t seed);
/** Height offset at a world position: 0 to amplitude, before any blend. */
double noiseHeight(const NoiseParams& p, double wx, double wz);

/** The geometry's heightfield, or null. */
const Heightfield* heightfieldOf(const lila::gn::Geometry& g);

/**
 * A float field's value at every sample. Fields see each sample as a point at its position, with "height" and every
 * mask as point attributes.
 */
std::vector<double> evaluateOnGrid(const Heightfield& h, const lila::gn::FieldPtr& field);

/** Lossless heightfield file (.lilahf): grid, origin, cell, heights and masks. Throw std::runtime_error. */
void writeHeightfield(const std::filesystem::path& path, const Heightfield& h);
Heightfield readHeightfield(const std::filesystem::path& path);

/**
 * A heightfield from a .lilahf file, or an image (.png, .exr, .tif, .psd, .tga) read as height: its red channel from 0
 * to 1 maps to low..high, except float EXR and TIFF, whose values are heights already when raw is set. The image is
 * width wide along X, centred on center, with square cells; its top row is the -Z edge, as seen from above.
 */
Heightfield loadHeightfield(const std::filesystem::path& path, glm::dvec2 center, double width, double low, double high, bool raw);

/** A terrain for code outside Commands.cpp: its heightfield in its own space, its placement and its mesh's name. */
struct TerrainRef
{
    std::shared_ptr<const Heightfield> local;
    glm::dmat4 world{1.0};
    std::string name;
};
/** The terrain a mesh or its modifier makes; throws CommandError naming cmd. */
TerrainRef findTerrainRef(const std::string& node, const std::string& cmd);

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
/** The Sculpt Terrain viewport tool (terrain.sculptTool). */
void registerSculptTool(lila::ext::Context& ctx);
} // namespace terrain
