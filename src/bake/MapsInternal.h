#pragma once

#include "bake/BakeTypes.h"
#include "bake/Maps.h"

#include <glm/glm.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

/** Shared pieces of the built-in map kernels (Maps*.cpp). Not part of the module's API. */
namespace lila::bake
{
struct BakeInputs;
struct GroupInputs;
struct HighInput;
struct LowInput;
struct MaterialInput;
struct TextureBuffer;

void registerSurfaceMaps();
void registerMaterialMaps();
void registerUvMaps();
void registerDerivedMaps();
void registerPackMaps();

/** The object normal a tangent-space normal kernel encodes: the high's own normal map applied when it has one. */
struct HighNormalMaps;
std::shared_ptr<const void> prepareHighNormalMaps(const MapSettings& settings, const BakeInputs& inputs);
/** The hit's shading normal, bent by its high's normal map when prepared holds one for it. */
glm::vec3 hitNormal(const void* prepared, const MapContext& ctx, const HitRecord& hit);

/** A loaded texture sampled bilinearly with wrap at Lila UVs (V up; row 0 is the top). */
struct TextureSampler
{
    int width = 0;
    int height = 0;
    std::vector<float> rgba;

    bool empty() const { return width <= 0; }
    glm::vec4 sample(const glm::vec2& uv) const;
};

/** Loads (once per path per bake) through the texture loader; null and a warning when unreadable. */
class TextureCache
{
public:
    std::shared_ptr<const TextureSampler> get(const std::string& path);
    std::vector<std::string> warnings;

private:
    std::vector<std::pair<std::string, std::shared_ptr<const TextureSampler>>> mLoaded;
};

/** The high's UV at a hit, (0, 0) when it has none. */
glm::vec2 hitUv(const HighInput& high, const HitRecord& hit);
/** Interpolates a per-corner attribute at a hit. */
template <class T> T atHit(const std::vector<T>& corners, const HitRecord& hit)
{
    const size_t c = size_t(hit.triangle) * 3;
    return corners[c] * (1.0f - hit.bary[0] - hit.bary[1]) + corners[c + 1] * hit.bary[0] + corners[c + 2] * hit.bary[1];
}

/** World length of one UV unit on a low triangle (sqrt of 3D area over UV area): converts texel steps to cm. */
float worldPerUv(const LowInput& low, uint32_t triangle);

/** Tangent-space slopes of a hit normal in the low's frame: (x / z, y / z) with y along +V; (0, 0) on a miss. */
glm::vec2 tangentSlopes(const TexelSample& s, const glm::vec3& normal);

/** The raw signed mean curvature (1/cm, convex positive) of every texel from a slope texture (kernel slopeKernel). */
std::vector<float> curvatureFromSlopes(const TextureBuffer& tex, int radius);

/**
 * Writes the slopes and world-per-uv scale a normal-derived map's finalize reads: (sx, sy, 0, worldPerUv). normalMaps is
 * prepareHighNormalMaps' result or null.
 */
void slopeKernel(const MapContext& ctx, const void* normalMaps, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                 std::span<uint8_t> valid);

/** Index of each position among the distinct ones (exact float equality), and how many there are. */
std::vector<uint32_t> weldPositions(const std::vector<glm::vec3>& positions, size_t& count);

/** One channel of a pack: a constant, or channel `channel` of map `map` baked with `options`. */
inline constexpr int kPackLuminance = 4;
inline constexpr int kPackMax = 5;

struct PackChannel
{
    std::string map;
    json options = json::object();
    /** options were given (else the project's settings for that map, if any, are used). */
    bool hasOptions = false;
    /** 0-3 copies R G B or A; kPackLuminance and kPackMax reduce the source's RGB to one value. */
    int channel = 0;
    bool invert = false;
    float constant = 0.0f;
};

/** A pack map's four channels from its options.channels or its MapDef::pack defaults. Problems go to warnings. */
std::vector<PackChannel> packChannels(const MapSettings& settings, const MapDef& def, std::vector<std::string>& warnings);

/** Fills pack's value from the per-channel source textures (null: the channel's constant) and lists them in meta. */
void composePack(TextureBuffer& pack, const std::vector<PackChannel>& channels, const std::vector<const TextureBuffer*>& sources, json& meta);

/** The value at the given percentile (0 to 1) of |v| over the covered texels; 0 when none. */
float percentileAbs(const std::vector<float>& v, const std::vector<uint8_t>& covered, float percentile);
} // namespace lila::bake
