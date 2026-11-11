#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lila::bake
{
struct BakeInputs;
struct GroupInputs;
struct TextureBuffer;

/** How a texel's samples become its value. */
enum class Resolve : uint8_t
{
    /** Mean of the valid samples (normals, heights, AO). */
    Average,
    /** Majority vote of exact colours, ties to the lowest sub-sample, never blended (IDs, islands). */
    Exact,
    /** The fraction of the texel's samples that landed on the low (alpha / coverage). */
    Coverage,
};

/** Which highs block secondary rays (AO and other occlusion maps). */
enum class OccluderScope : uint8_t
{
    /** Only the high the projection ray hit. */
    Self,
    /** The group's highs: the default, so overlapping groups never shadow each other. */
    Group,
    /** Every group's highs. */
    All,
};
LILA_SCENE_API OccluderScope occluderScope(std::string_view name);

/** The nearest surface a secondary ray met. */
struct TraceHit
{
    float distance = 0.0f;
    /** Interpolated shading normal (unit); zero when the tracer can't tell. */
    glm::vec3 normal{0.0f};
    /** The ray met the surface's back. */
    bool backface = false;
    /** The high (in the ray's group) and its soup triangle, kNoHit when the tracer can't tell or it lies in another group. */
    uint32_t high = kNoHit;
    uint32_t triangle = kNoHit;
};

/** Secondary-ray queries a backend offers kernels. */
class LILA_SCENE_API Tracer
{
public:
    virtual ~Tracer() = default;
    /**
     * True if anything in scope lies within tMax along origin + t * dir (dir unit). high is the hit's high, for Self; low
     * (the sample's, or kNoHit) skips the highs its group's links unlink from it for occlusion.
     */
    virtual bool occluded(size_t group, OccluderScope scope, uint32_t high, uint32_t low, const glm::vec3& origin, const glm::vec3& dir,
                          float tMax) const = 0;
    /**
     * The nearest surface in scope within tMax, front or back facing, skipping what the low's links say for kind. The
     * default bisects occluded() (no normal, occlusion links only), so a backend overrides it to be faster, report normals
     * or honour projection links.
     */
    virtual bool closestHit(size_t group, OccluderScope scope, uint32_t high, uint32_t low, RayKind kind, const glm::vec3& origin, const glm::vec3& dir, float tMax,
                            TraceHit& hit) const;
};

/** What a map kernel sees besides the samples and hits. */
struct MapContext
{
    const MapSettings& settings;
    const BakeInputs& inputs;
    const GroupInputs& group;
    size_t groupIndex = 0;
    /** Null when the backend has none (then secondaryRays maps are skipped). */
    const Tracer* tracer = nullptr;
    /** Output texture size in texels. */
    int width = 0;
    int height = 0;
    /** What the map's MapDef::prepare returned for this bake, else null. */
    const void* prepared = nullptr;
};

/**
 * One value per sample into out. valid[i] = 0 leaves sample i out of the texel's value (it is still counted for
 * coverage); a texel with no valid sample is filled by dilation. Runs on worker threads, many bands at once.
 */
using CpuKernel = std::function<void(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits,
                                     std::span<glm::vec4> out, std::span<uint8_t> valid)>;

/**
 * After every group has written into a texture and its texels are resolved, before dilation: normalises values that need
 * the whole texture (height's auto range) and records what a reader needs to decode them into meta.
 */
using Finalize = std::function<void(const MapSettings& settings, const BakeInputs& inputs, TextureBuffer& tex, json& meta)>;

/**
 * Once per bake, on the calling thread before any kernel runs: per-mesh data a kernel would otherwise rebuild per band
 * (welded curvature, loaded material textures, edge flags). Kernels see it as MapContext::prepared.
 */
using Prepare = std::function<std::shared_ptr<const void>(const MapSettings& settings, const BakeInputs& inputs)>;

/** A bakeable map. New maps register one of these (a CPU kernel at least; a GPU backend looks kernels up by id). */
struct MapDef
{
    std::string id;
    std::string label;
    /** The {map} token when MapSettings::suffix is empty. */
    std::string suffix;
    std::string doc;
    Resolve resolve = Resolve::Average;
    /** Texels no sample reached and dilation didn't fill. */
    glm::vec4 background{0.0f, 0.0f, 0.0f, 1.0f};
    bool dilate = true;
    /** Texture holds linear data (normals, heights) rather than colour. Informational for importers. */
    bool linear = true;
    /** Smallest PNG depth that holds it without banding (height wants 16). */
    int preferredBits = 8;
    /** Shoots secondary rays (needs the occluder scope). */
    bool secondaryRays = false;
    CpuKernel cpu;
    Finalize finalize;
    Prepare prepare;
    /**
     * Non-null makes this a channel pack: it has no kernel and its texture is composed after every other map from
     * options.channels, else these defaults. Four entries, R G B A, each a constant number or
     * {"map": id, "channel": 0-3, "options": {...}, "invert": false}. A map a pack names is baked with it (hidden unless
     * the project bakes it too, with the same options), finalized and dilated on its own, then copied in.
     */
    json pack;
};

/** Registers or replaces a map. Main thread, before a bake. */
LILA_SCENE_API void registerMap(MapDef def);
/** Null when unknown. */
LILA_SCENE_API const MapDef* findMap(std::string_view id);
/** Every registered map, by id. */
LILA_SCENE_API std::vector<const MapDef*> allMaps();
/**
 * Registers the built-in maps: normal, objectNormal, position, height, id, alpha, uvIslands, ao, curvature,
 * normalToCurvature, convexity, cavity, thickness, bentNormal, roundedNormal, vertexColor, baseColor, roughness,
 * metallic, emissive, materialOpacity, wireframe, islandBorder, error, gradient, normalToHeight, detailNormal, edgeWear, pack, orm.
 * Idempotent.
 */
LILA_SCENE_API void registerBuiltinMaps();

/**
 * Reads a material texture for the transfer maps into RGBA floats (row 0 the top, colour maps left sRGB coded). The
 * default reads what core/ImageIO reads; a host with more formats installs its own. Called on the bake's thread.
 */
using TextureLoader = std::function<bool(const std::string& path, int& width, int& height, std::vector<float>& rgba, std::string& error)>;
LILA_SCENE_API void setTextureLoader(TextureLoader loader);

/** The exact 8-bit colour a name gets in ID maps (stable per name), 0 to 1 per channel. */
LILA_SCENE_API glm::vec3 idColour(std::string_view name);
/** The exact colour of palette entry i (UV islands): golden-angle hues, 8-bit exact. */
LILA_SCENE_API glm::vec3 paletteColour(uint32_t i);

/**
 * The built-in maps' per-mesh data as plain arrays, for backends that run the kernels themselves; each is what the CPU
 * kernel's prepare computes.
 */
struct MeshSoup;
struct LowInput;
struct MaterialInput;

/** Signed mean curvature per soup corner (1/cm, convex positive), as the curvature maps' mesh source reads it. Cached by positions. */
LILA_SCENE_API std::shared_ptr<const std::vector<float>> highCurvature(const MeshSoup& soup);
/** Per low triangle, bit k set when its edge k (corner k to k + 1) is drawn: polygon edges, or UV island borders. */
LILA_SCENE_API std::vector<uint8_t> lowEdgeFlags(const LowInput& low, bool borders);

/** Where a transfer map (baseColor, roughness, metallic, emissive, materialOpacity) reads a material. */
struct TransferSource
{
    /** Texture to sample, empty for the material's constant; fallback is tried when it can't be read. */
    std::string texture;
    int channel = 0;
    std::string fallback;
    int fallbackChannel = 0;
    /** Whether texture's and fallback's values are sRGB-encoded. */
    bool srgb = false;
    bool fallbackSrgb = false;
};
LILA_SCENE_API TransferSource transferSource(const MaterialInput& material, std::string_view map);
/** Reads a texture through the installed loader (setTextureLoader): RGBA floats, row 0 the top. */
LILA_SCENE_API bool loadBakeTexture(const std::string& path, int& width, int& height, std::vector<float>& rgba, std::string& error);

/** MikkTSpace per-fragment frame encode, the inverse of the viewport's decode (mesh.slang). greenUp is OpenGL. */
LILA_SCENE_API glm::vec3 encodeTangentNormal(const glm::vec3& highNormal, const glm::vec3& lowNormal, const glm::vec4& lowTangent, bool greenUp,
                                            bool flipX = false);
} // namespace lila::bake
