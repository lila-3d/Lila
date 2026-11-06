#pragma once

#include "render/Compute.h"
#include "render/FrameState.h"
#include "render/MeshPush.h"

#include <volk.h>

#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

namespace lila::render
{
class VulkanContext;

/**
 * Min and max heights over blocks of a grid, a level per doubling: level k's blocks span kPatch << k samples (edges
 * shared), up to one block covering everything. What a patch selection culls and refines by.
 */
struct HeightPyramid
{
    std::vector<glm::ivec2> dims;               // blocks along X and Z, per level
    std::vector<std::vector<glm::vec2>> ranges; // min, max per block, rows along X
    int levels() const { return int(dims.size()); }
};

/** Quads along a patch side; a heightfield is drawn as patches of kPatch by kPatch quads. */
constexpr int kHeightfieldPatch = 32;
constexpr int kHeightfieldMaxLods = 24;
constexpr int kHeightfieldMaxLayers = 8;

/** One patch to draw: kHeightfieldPatch quads a side, spacing samples apart from origin (samples), at lod. */
struct HeightfieldPatch
{
    glm::vec2 origin{0.0f};
    float spacing = 1.0f;
    uint32_t lod = 0;
};

struct HeightfieldView
{
    glm::dmat4 viewProj{1.0}; // unjittered, reversed-Z
    glm::dvec3 eye{0.0};
    bool ortho = false;
    double focalPx = 1000.0;  // perspective: the view's focal length in device pixels
    double pixelWorld = 1.0;  // ortho: world size of a device pixel
};

/** What a selection chose: the patches, and each level's morph start and 1 / morph length. */
struct HeightfieldSelection
{
    std::vector<HeightfieldPatch> patches;
    glm::vec2 morph[kHeightfieldMaxLods]{};
    int levels = 0;
};

LILA_UI_API HeightPyramid buildHeightPyramid(const std::vector<float>& heights, int nx, int nz);

/**
 * Continuous level of detail (CDLOD) for one heightfield seen from view: patches over the visible grid, finer near the
 * camera so a quad stays about pixelsPerQuad device pixels across, every level at most one apart from its neighbours.
 */
LILA_UI_API HeightfieldSelection selectHeightfieldPatches(const HeightfieldDraw& d, const HeightPyramid& pyramid, const HeightfieldView& view,
                                                          double pixelsPerQuad = 4.0, size_t maxPatches = size_t(1) << 16);

/**
 * The viewport's heightfield draw path: heights and masks kept on the GPU, patches picked per view and drawn with the
 * mesh shader's lighting (heightfield.slang). Render thread only.
 */
class HeightfieldRenderer
{
public:
    /** Pipelines the renderer builds with its mesh layout from heightfield(_bindless).spv. */
    VkPipeline shaded = VK_NULL_HANDLE;  // heightfieldPS, opaque
    VkPipeline wire = VK_NULL_HANDLE;    // heightfieldWirePS, blended, no depth write
    VkPipeline prepass = VK_NULL_HANDLE; // heightfieldPrepassPS, depth and normal
    bool bindless = false;

    /** A map's table handle in this frame (a normal map's when normal is set), 0xffff when it has none. */
    using MapHandle = std::function<uint32_t(const TexturePtr&, bool normal)>;

    /**
     * Uploads the grids drawn for the first time, picks each heightfield's patches for this view and writes their
     * parameters into frameKey's buffer (one per frame in flight: its last use has finished). Before rendering begins.
     */
    void prepare(VulkanContext& vk, VkCommandBuffer cmd, const void* frameKey, const FrameState& s, uint32_t heightPx, uint64_t frame,
                 const MapHandle& handle);

    size_t count() const { return mPrepared.size(); }
    const HeightfieldDraw& draw(size_t i) const { return *mPrepared[i].draw; }
    /** Patches and triangles prepared for this frame. */
    size_t patches() const { return mPatches; }

    /** Records prepared draw i with whichever heightfield pipeline is bound; p holds its material's push constants. */
    void record(VkCommandBuffer cmd, VkPipelineLayout layout, size_t i, MeshPush p, const glm::mat4& viewProj) const;

    /** Drops grids no frame has drawn for a while, and at once those nothing else holds. */
    void evict(VulkanContext& vk, uint64_t frame);
    void destroy(VulkanContext& vk);

private:
    struct Grid
    {
        compute::Buffer buffer;
        GridValues keep;
        uint64_t lastUsed = 0;
        HeightPyramid pyramid; // heights only
    };
    struct FrameData
    {
        compute::Buffer host;
        std::vector<compute::Buffer> staging;
        uint64_t lastUsed = 0;
    };
    struct Prepared
    {
        const HeightfieldDraw* draw = nullptr;
        VkDeviceAddress params = 0;
        uint32_t patches = 0;
    };

    Grid* heights(VulkanContext& vk, VkCommandBuffer cmd, FrameData& f, const HeightfieldDraw& d, uint64_t frame);
    VkDeviceAddress mask(VulkanContext& vk, VkCommandBuffer cmd, FrameData& f, const GridValues& values, size_t count, bool stretch, uint64_t frame);

    std::unordered_map<const void*, Grid> mHeights;
    std::map<std::pair<const void*, bool>, Grid> mMasks; // keyed by values and whether stretched to their range
    std::unordered_map<const void*, FrameData> mFrames;
    std::vector<Prepared> mPrepared;
    compute::Buffer mIndices; // one patch's triangles over its (kHeightfieldPatch + 1)-square vertex grid
    size_t mPatches = 0;
    bool mWarnedMaps = false;
};
} // namespace lila::render
