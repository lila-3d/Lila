#include "render/Baker.h"

#include "bake/Backend.h"
#include "bake/BakeCache.h"
#include "bake/BakeInputs.h"
#include "bake/CpuBackend.h"
#include "bake/Maps.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Bvh.h"
#include "render/Compute.h"
#include "render/LiveBake.h"
#include "render/MeshMaps.h"
#include "render/Renderer.h"
#include "render/VulkanContext.h"

#include <glm/gtc/packing.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace lila::render
{
namespace
{
using bake::HitRecord;
using bake::TexelSample;

/** GPU time per render-thread slice: well under a frame budget's worth of stall and far from the driver's timeout. */
constexpr double kSliceMs = 40.0;
/** Bands in flight at once: one tracing while the others upload or accumulate on the CPU. */
constexpr size_t kSlots = 16;
/** Samples per band, which bounds what each worker thread holds while it waits for the GPU. */
constexpr size_t kBandSamples = size_t(1) << 17;
/** Triangles per BLAS, so no single build stalls the render thread for long. */
constexpr uint32_t kBlasChunk = 1u << 20;
constexpr size_t kDeviceBytesPerSample = sizeof(TexelSample) + sizeof(HitRecord) + sizeof(glm::vec4);
constexpr uint32_t kNone = 0xffffffffu;

enum MapKernel : uint32_t
{
    kMapNormal = 0,
    kMapObjectNormal = 1,
    kMapPosition = 2,
    kMapHeight = 3,
    kMapId = 4,
    kMapAlpha = 5,
    kMapIslands = 6,
    kMapCurvatureMesh = 7,
    kMapSlopes = 8,
    kMapVertexColor = 9,
    kMapIdVertexColor = 10,
    kMapMaterial = 11,
    kMapEdges = 12,
    kMapGradient = 13,
    kMapError = 14,
};

enum PushFlags : uint32_t
{
    kFlagCullBack = 1,
    kFlagGreenUp = 2,
    kFlagFlipX = 4,
    kFlagHalf = 8,
    kFlagScopeSelf = 16,
    kFlagResident = 32,
    kFlagNormalMaps = 64,
    kFlagObjectSpace = 128,
};

/** Mirrors BakePush in shaders/bake_common.slang. */
struct GpuBakePush
{
    uint64_t samples = 0;
    uint64_t hits = 0;
    uint64_t outFloat = 0;
    uint64_t outHalf = 0;
    uint64_t highs = 0;
    uint64_t highList = 0;
    uint64_t table = 0;
    uint64_t tableBase = 0;
    glm::vec4 boundsMin{0.0f};
    glm::vec4 boundsExt{0.0f};
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t highCount = 0;
    uint32_t map = 0;
    uint32_t flags = 0;
    uint32_t rays = 0;
    uint64_t instanceBase = 0;
    glm::vec4 params{0.0f};
    glm::vec4 params2{0.0f};
    uint64_t lows = 0;
    uint64_t extra = 0;
    uint64_t valid = 0;
    uint64_t stats = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mode = 0;
    uint32_t pad0 = 0;
    uint64_t normalMaps = 0;
};
static_assert(offsetof(GpuBakePush, params) == 128 && offsetof(GpuBakePush, normalMaps) == 208, "matches BakePush in bake_common.slang");

/** Mirrors RasterPush in shaders/bake_raster.slang. */
struct GpuRasterPush
{
    uint64_t low = 0;
    uint64_t items = 0;
    uint64_t claim = 0;
    uint64_t out = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t grid = 1;
    uint32_t y0 = 0;
    uint32_t rows = 0;
    uint32_t tileU = 0;
    uint32_t tileV = 0;
    uint32_t udim = 1001;
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t lowIndex = 0;
    uint32_t explicitCage = 0;
    float frontal = 0.0f;
    float rear = 0.0f;
    float offset = 1.0f;
    float skew = 0.0f;
};
static_assert(sizeof(GpuRasterPush) == 96, "matches RasterPush in bake_raster.slang");

/** Mirrors ImagePush in shaders/bake_image.slang. */
struct GpuImagePush
{
    uint64_t samples = 0;
    uint64_t out = 0;
    uint64_t valid = 0;
    uint64_t targets = 0;
    uint64_t low = 0;
    uint64_t value = 0;
    uint64_t weight = 0;
    uint64_t count = 0;
    uint64_t exact = 0;
    uint64_t votes = 0;
    uint64_t seedsIn = 0;
    uint64_t seedsOut = 0;
    uint64_t scratch = 0;
    uint64_t reduce = 0;
    glm::vec4 background{0.0f};
    glm::vec4 params{0.0f};
    glm::vec4 params2{0.0f};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t grid = 1;
    uint32_t y0 = 0;
    uint32_t rows = 0;
    uint32_t mode = 0;
    uint32_t first = 0;
    uint32_t total = 0;
    int32_t step = 0;
    int32_t padding = 0;
    uint32_t prefix = 0;
    uint32_t shift = 0;
};
static_assert(offsetof(GpuImagePush, background) == 112 && sizeof(GpuImagePush) == 208, "matches ImagePush in bake_image.slang");

/** Mirrors LowInfo in shaders/bake_common.slang. */
struct GpuLowInfo
{
    uint64_t positions = 0;
    uint64_t normals = 0;
    uint64_t tangents = 0;
    uint64_t uvs = 0;
    uint64_t cageNormals = 0;
    uint64_t cageScale = 0;
    uint64_t cageSkew = 0;
    uint64_t cagePositions = 0;
    uint64_t materials = 0;
    uint64_t edges = 0;
    uint32_t triangles = 0;
    uint32_t hasSkew = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};
static_assert(sizeof(GpuLowInfo) == 96);

/** Mirrors TextureRef in shaders/bake_common.slang. */
struct GpuTextureRef
{
    uint64_t texels = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t flags = 0;
    uint32_t pad[3] = {};
};
static_assert(sizeof(GpuTextureRef) == 32);

/** Mirrors MaterialSlot in shaders/bake_common.slang. */
struct GpuMaterialSlot
{
    glm::vec4 baseColor{0.5f};
    glm::vec4 emissive{0.0f};
    glm::vec4 scalars{0.55f, 0.0f, 1.0f, 1.0f};
    GpuTextureRef texture;
    uint32_t channel = 0;
    uint32_t pad[3] = {};
};
static_assert(sizeof(GpuMaterialSlot) == 96);

/** Mirrors CornerFloats / CornerColors in shaders/bake_common.slang. */
struct GpuCornerTable
{
    uint64_t corners = 0;
    uint32_t present = 0;
    uint32_t pad = 0;
};
static_assert(sizeof(GpuCornerTable) == 16);

/** Mirrors AccTarget in shaders/bake_image.slang. */
struct GpuAccTarget
{
    uint64_t value = 0;
    uint64_t weight = 0;
    uint64_t count = 0;
    uint64_t exact = 0;
    uint64_t votes = 0;
    uint32_t mode = 0;
    uint32_t id = 0;
};
static_assert(sizeof(GpuAccTarget) == 48);

/** Mirrors DilatePush in shaders/bake_dilate.slang. */
struct DilatePush
{
    uint64_t covered = 0;
    uint64_t seedsIn = 0;
    uint64_t seedsOut = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t step = 0;
    uint32_t pad = 0;
};

/** Mirrors HighInfo in shaders/bake_common.slang. */
struct GpuHighInfo
{
    uint64_t normals = 0;
    uint64_t materials = 0;
    uint64_t nodes = 0;
    uint64_t corners = 0;
    uint32_t triangles = 0;
    uint32_t materialBase = 0;
    uint32_t materialCount = 0;
    uint32_t pad = 0;
    uint64_t positions = 0;
    uint64_t uvs = 0;
};
static_assert(sizeof(GpuHighInfo) == 64);

using GpuBuffer = compute::Buffer;
using compute::copy;
using compute::destroy;
using compute::dispatchChunks;
using compute::Host;
using compute::hostBuffer;
using compute::makeBuffer;
using compute::memoryBarrier;
using compute::Submitter;

std::vector<uint32_t> loadSpirv(const char* name)
{
    std::string err;
    std::vector<uint32_t> code = compute::loadSpirv(name, &err);
    if (code.empty())
        throw std::runtime_error("GPU bake: " + err);
    return code;
}

/** Options each ported kernel understands; anything else sends the map to its CPU kernel. */
bool onlyOptions(const bake::json& options, std::initializer_list<std::string_view> keys)
{
    if (options.is_null())
        return true;
    if (!options.is_object())
        return false;
    for (const auto& [k, v] : options.items())
        if (std::ranges::find(keys, std::string_view(k)) == keys.end())
            return false;
    return true;
}

struct KernelChoice
{
    int map = -1;
    bool ao = false;
    /** RGBA16F output: exact for values on an 8-bit grid (IDs, masks) and ray counts; positions and normals get 32F. */
    bool half = false;
};

KernelChoice kernelFor(const bake::MapDef& def, const bake::MapSettings& s)
{
    const bake::json& o = s.options;
    if (def.id == "normal" && onlyOptions(o, {"greenUp", "flipX"}))
        return {kMapNormal};
    if (def.id == "objectNormal" && onlyOptions(o, {}))
        return {kMapObjectNormal};
    if (def.id == "position" && onlyOptions(o, {}))
        return {kMapPosition};
    if (def.id == "height" && onlyOptions(o, {"range"}))
        return {kMapHeight};
    if (def.id == "alpha" && onlyOptions(o, {}))
        return {kMapAlpha, false, true};
    if (def.id == "uvIslands" && onlyOptions(o, {}))
        return {kMapIslands, false, true};
    if (def.id == "id" && onlyOptions(o, {"source", "palette"}))
    {
        // Sources constant per high material slot; vertex colours stay on the CPU.
        const std::string source = o.is_object() ? o.value("source", std::string("material")) : std::string("material");
        if (source == "material" || source == "object" || source == "group")
            return {kMapId, false, true};
    }
    if (def.id == "ao" && onlyOptions(o, {"rays", "distance", "scope"}))
        return {-1, true, true};
    return {};
}

/** Device buffers of one band, kept from its projection through its map kernels. */
struct BandSlot
{
    GpuBuffer staging, samples, hits, hitsRead, out, outRead;
    size_t capacity = 0;
    const TexelSample* key = nullptr;
    size_t count = 0;
    size_t group = 0;
    bool resident = false;
};

/** The slot each worker thread holds for its current band, per backend instance. */
thread_local std::unordered_map<uint64_t, BandSlot*> tHeld;

std::atomic<uint64_t> gNextBackendId{1};


struct Blas
{
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    GpuBuffer buffer;
    VkDeviceAddress address = 0;
    uint32_t firstTriangle = 0;
};

struct Tlas
{
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    GpuBuffer buffer, instances, instanceBase;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

/** A high's device copies. Shared by the backends of successive bakes through HighCache, so they outlive any one bake. */
struct GpuHigh
{
    GpuBuffer normals, materials, nodes, corners;
    /** Uploaded when a map needs them: corner positions and uvs, mesh curvature per corner, colour sets by name. */
    GpuBuffer positions, uvs, curvature;
    std::map<std::string, GpuBuffer> colors;
    std::vector<Blas> blas;
    uint32_t triangles = 0;
    /** A triangle of each material slot (kNone when unused), for building ID colour tables. */
    std::vector<uint32_t> slotTriangle;
};

size_t highBytes(const GpuHigh& h)
{
    size_t n = 0;
    for (const GpuBuffer* b : {&h.normals, &h.materials, &h.nodes, &h.corners, &h.positions, &h.uvs, &h.curvature})
        n += b->buffer ? size_t(b->size) : 0;
    for (const auto& [name, b] : h.colors)
        n += size_t(b.size);
    for (const Blas& b : h.blas)
        n += size_t(b.buffer.size);
    return n;
}

void destroyHigh(VulkanContext& vk, GpuHigh& h)
{
    for (GpuBuffer* b : {&h.normals, &h.materials, &h.nodes, &h.corners, &h.positions, &h.uvs, &h.curvature})
        destroy(vk, *b);
    for (auto& [name, b] : h.colors)
        destroy(vk, b);
    for (Blas& b : h.blas)
    {
        if (b.as)
            vkDestroyAccelerationStructureKHR(vk.device, b.as, nullptr);
        destroy(vk, b.buffer);
    }
    h = {};
}

/**
 * Highs kept on the device between bakes, by soup content and trace mode: a rebake after a settings change finds its
 * buffers and BLAS here and uploads and builds nothing. Entries no backend holds go oldest first once over budget,
 * and all of them when the renderer stops or clearBakeCaches runs.
 */
class HighCache
{
public:
    std::shared_ptr<GpuHigh> find(uint64_t key)
    {
        std::scoped_lock lock(mMutex);
        for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
            if (it->first == key)
            {
                mEntries.splice(mEntries.begin(), mEntries, it);
                return mEntries.front().second;
            }
        return nullptr;
    }

    void add(uint64_t key, std::shared_ptr<GpuHigh> high)
    {
        std::scoped_lock lock(mMutex);
        mEntries.emplace_front(key, std::move(high));
    }

    /** Destroys unheld entries, oldest first, while the cache is over budget (0: all unheld). Render thread. */
    void trim(VulkanContext& vk, size_t budget)
    {
        std::scoped_lock lock(mMutex);
        size_t total = 0;
        for (const auto& [k, h] : mEntries)
            total += highBytes(*h);
        for (auto it = mEntries.end(); it != mEntries.begin() && (total > budget || budget == 0);)
        {
            --it;
            if (it->second.use_count() > 1)
                continue;
            total -= highBytes(*it->second);
            destroyHigh(vk, *it->second);
            it = mEntries.erase(it);
        }
    }

private:
    std::mutex mMutex;
    std::list<std::pair<uint64_t, std::shared_ptr<GpuHigh>>> mEntries;
};

HighCache& highCache()
{
    static HighCache c;
    return c;
}

/** Device memory the high cache may keep between bakes. */
constexpr size_t kHighCacheBytes = size_t(4) << 30;

/** GPU work for GpuBackend::runSteps: count items recorded in order, a barrier after each step. */
struct GpuStep
{
    size_t count = 1;
    /** Cost per item relative to rate (items per ms); steps without a rate are cheap and never split. */
    double cost = 1.0;
    std::atomic<double>* rate = nullptr;
    std::function<void(VkCommandBuffer cmd, uint32_t first, uint32_t count)> record;
    /** Where the step's GPU time goes in the bake's timings ("raster", "map:ao", ...). */
    std::string label = "other";
};


class GpuBackend final : public bake::Backend, public bake::Tracer
{
public:
    GpuBackend(Renderer& renderer, bool hardware) : mRenderer(renderer), mVk(*renderer.context()), mDevice(mVk.device), mHardware(hardware) {}
    ~GpuBackend() override;
    GpuBackend(const GpuBackend&) = delete;
    GpuBackend& operator=(const GpuBackend&) = delete;

    std::string name() const override { return "gpu"; }
    std::string mode() const override { return mFailed ? "cpu" : mHardware ? "rt" : "compute"; }
    bool hardware() const { return mHardware; }
    void setSliceMs(double ms) { mSliceMs = ms; }
    void prepare(const bake::BakeInputs& inputs) override;
    void traceProjection(size_t group, std::span<const TexelSample> samples, std::span<HitRecord> hits) const override;
    void shade(const bake::MapDef& def, const bake::MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits,
               std::span<glm::vec4> out, std::span<uint8_t> valid) const override;
    void dilate(bake::TextureBuffer& tex, int padding) const override;
    const bake::Tracer* tracer() const override { return this; }
    int bandRows(int width, int samplesPerTexel) const override;
    void endBand(std::span<const TexelSample> samples) const override;
    bool residentSupports(const bake::MapDef& def, const bake::MapSettings& settings) const override;
    std::unique_ptr<bake::ResidentBake> beginResident(const bake::BakeInputs& inputs, const std::vector<bake::ResidentMap>& maps,
                                                      const bake::OutputSettings& output) override;

    bool occluded(size_t group, bake::OccluderScope scope, uint32_t high, const glm::vec3& origin, const glm::vec3& dir, float tMax) const override
    {
        return cpu().occluded(group, scope, high, origin, dir, tMax);
    }

private:
    friend class GpuResident;
    friend struct ResidentUploads;

    /** Runs a task on the render thread and waits for it. */
    void runTask(GpuTask task) const { mRenderer.runTask(std::move(task)).get(); }
    /**
     * A task running record over [0, count) in slices sized by rate (items per ms, adapted as it goes); begin and end
     * record into the first and last slice (upload and readback).
     */
    void runSliced(size_t count, std::atomic<double>& rate, double costPerItem, const std::function<void(VkCommandBuffer)>& begin,
                   const std::function<void(VkCommandBuffer, uint32_t, uint32_t)>& record, const std::function<void(VkCommandBuffer)>& end) const;

    /**
     * Runs steps on the render thread in order, as many per slice as fit about kSliceMs. Throws progress::Cancelled
     * when cancelled (checked between slices, any thread) says so.
     */
    void runSteps(std::vector<GpuStep>& steps, const std::function<bool()>& cancelled = {}) const;

    /** Pipelines and the submitter; render thread, first task. */
    void initDevice(VulkanContext& vk) const;
    void releaseScene();
    /** The reference backend over the same inputs, built on first use (TRACE_SW builds it in prepare for its BVHs). */
    const bake::CpuBackend& cpu() const;
    void fail(const std::string& why) const;

    BandSlot* acquireSlot() const;
    BandSlot* heldSlot() const;
    void reserve(BandSlot& slot, size_t samples) const;

    Blas buildBlas(VulkanContext& vk, const bake::MeshSoup& soup, uint32_t first, uint32_t count) const;
    Tlas buildTlas(VulkanContext& vk, const std::vector<std::pair<uint32_t, const Blas*>>& instances) const;
    const Tlas& allTlas(VulkanContext& vk) const;
    const GpuBuffer& idTable(const bake::MapDef& def, const bake::MapContext& ctx) const;
    void gpuDilate(bake::TextureBuffer& tex, int padding) const;

    Renderer& mRenderer;
    VulkanContext& mVk;
    VkDevice mDevice;
    bool mHardware;
    uint64_t mId = gNextBackendId++;
    const bake::BakeInputs* mInputs = nullptr;
    mutable std::atomic<bool> mFailed{false};
    mutable std::once_flag mFailLog;

    mutable std::mutex mCpuMutex;
    mutable std::unique_ptr<bake::CpuBackend> mCpu;
    mutable std::atomic<bool> mCpuReady{false};

    // Render thread only.
    mutable Submitter mSub;
    mutable VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    mutable VkDescriptorPool mPool = VK_NULL_HANDLE;
    mutable VkPipelineLayout mLayoutSw = VK_NULL_HANDLE, mLayoutHw = VK_NULL_HANDLE;
    // Pipelines belong to compute::sharedPipelines, so the next bake finds them made.
    mutable VkPipeline mProjectSw = VK_NULL_HANDLE, mAoSw = VK_NULL_HANDLE, mShade = VK_NULL_HANDLE;
    mutable VkPipeline mProjectHw = VK_NULL_HANDLE, mAoHw = VK_NULL_HANDLE;
    mutable VkPipeline mJfaInit = VK_NULL_HANDLE, mJfaStep = VK_NULL_HANDLE;
    // Resident bakes: ray kernels per trace mode, the raster and the texture passes.
    mutable VkPipeline mThicknessSw = VK_NULL_HANDLE, mBentSw = VK_NULL_HANDLE, mRoundedSw = VK_NULL_HANDLE, mErrorSw = VK_NULL_HANDLE;
    mutable VkPipeline mThicknessHw = VK_NULL_HANDLE, mBentHw = VK_NULL_HANDLE, mRoundedHw = VK_NULL_HANDLE, mErrorHw = VK_NULL_HANDLE;
    mutable VkPipelineLayout mLayoutRaster = VK_NULL_HANDLE, mLayoutImage = VK_NULL_HANDLE;
    mutable VkPipeline mClaim = VK_NULL_HANDLE, mSamplesPipe = VK_NULL_HANDLE;
    mutable std::map<std::string, VkPipeline> mImagePipes;
    /** Timestamps around runSteps' steps, and the GPU milliseconds they measured per step label. */
    mutable VkQueryPool mQueries = VK_NULL_HANDLE;
    mutable std::mutex mGpuTimeMutex;
    mutable std::map<std::string, double> mGpuTimes;
    mutable Tlas mAll;
    mutable bool mAllBuilt = false;

    // Scene, immutable after prepare.
    std::vector<std::shared_ptr<GpuHigh>> mHighs;
    /** First ID colour table entry of each high, within its group. */
    std::vector<uint32_t> mMaterialBase;
    std::vector<size_t> mGroupBase;
    std::vector<size_t> mGroupHighs;
    std::vector<Tlas> mGroupTlas;
    /** Per group: [first table entry of each low][packed island colour of each low triangle]. */
    std::vector<GpuBuffer> mIslands;
    GpuBuffer mHighInfo;
    /** What mHighInfo holds, to rewrite when buffers are added to highs. */
    std::vector<GpuHighInfo> mHighInfoHost;
    GpuBuffer mIota;

    mutable std::mutex mTableMutex;
    mutable std::map<std::string, GpuBuffer> mIdTables;

    mutable std::mutex mSlotMutex;
    mutable std::condition_variable mSlotCv;
    mutable std::vector<std::unique_ptr<BandSlot>> mSlots;
    mutable std::vector<BandSlot*> mFree;

    // Throughput, items per ms, so slices land near kSliceMs.
    mutable std::atomic<double> mProjectRate{mHardware ? 20000.0 : 2000.0};
    mutable std::atomic<double> mShadeRate{100000.0};
    mutable std::atomic<double> mRayRate{mHardware ? 200000.0 : 20000.0};
    mutable std::atomic<double> mRasterRate{2000000.0};
    mutable std::atomic<double> mImageRate{2000000.0};
    /** GPU time runSteps aims each render-thread slice at; live bakes keep it short so frames keep their pace. */
    double mSliceMs = kSliceMs;
};

GpuBackend::~GpuBackend()
{
    if (!mRenderer.running() || !mRenderer.context() || mRenderer.context()->device != mDevice)
        return;
    try
    {
        runTask([this](VulkanContext& vk) {
            releaseScene();
            for (auto& s : mSlots)
                for (GpuBuffer* b : {&s->staging, &s->samples, &s->hits, &s->hitsRead, &s->out, &s->outRead})
                    destroy(vk, *b);
            for (VkPipelineLayout l : {mLayoutSw, mLayoutHw, mLayoutRaster, mLayoutImage})
                if (l)
                    vkDestroyPipelineLayout(vk.device, l, nullptr);
            if (mPool)
                vkDestroyDescriptorPool(vk.device, mPool, nullptr);
            if (mQueries)
                vkDestroyQueryPool(vk.device, mQueries, nullptr);
            if (mSetLayout)
                vkDestroyDescriptorSetLayout(vk.device, mSetLayout, nullptr);
            mSub.destroy(vk);
            return true;
        });
    }
    catch (const std::exception& e)
    {
        log::warn("GPU bake: cleanup failed: {}", e.what());
    }
}

void GpuBackend::releaseScene()
{
    VulkanContext& vk = mVk;
    auto dropTlas = [&](Tlas& t) {
        if (t.as)
            vkDestroyAccelerationStructureKHR(vk.device, t.as, nullptr);
        destroy(vk, t.buffer);
        destroy(vk, t.instances);
        destroy(vk, t.instanceBase);
        t = {};
    };
    mHighs.clear();
    highCache().trim(vk, kHighCacheBytes);
    for (Tlas& t : mGroupTlas)
        dropTlas(t);
    mGroupTlas.clear();
    dropTlas(mAll);
    mAllBuilt = false;
    for (GpuBuffer& b : mIslands)
        destroy(vk, b);
    mIslands.clear();
    destroy(vk, mHighInfo);
    destroy(vk, mIota);
    {
        std::scoped_lock lock(mTableMutex);
        for (auto& [k, b] : mIdTables)
            destroy(vk, b);
        mIdTables.clear();
    }
    if (mPool)
        vkResetDescriptorPool(vk.device, mPool, 0);
}

const bake::CpuBackend& GpuBackend::cpu() const
{
    if (!mCpuReady.load(std::memory_order_acquire))
    {
        std::scoped_lock lock(mCpuMutex);
        if (!mCpuReady.load(std::memory_order_relaxed))
        {
            auto c = std::make_unique<bake::CpuBackend>();
            c->prepare(*mInputs);
            mCpu = std::move(c);
            mCpuReady.store(true, std::memory_order_release);
        }
    }
    return *mCpu;
}

void GpuBackend::fail(const std::string& why) const
{
    mFailed = true;
    std::call_once(mFailLog, [&] { log::warn("GPU bake failed, continuing on the CPU: {}", why); });
}

void GpuBackend::initDevice(VulkanContext& vk) const
{
    if (mSub.pool)
        return;
    LILA_ZONE();

    mSub.init(vk);
    struct Module
    {
        std::vector<uint32_t> code;
        uint64_t hash = 0;
    };
    std::map<std::string, Module> modules;
    auto module = [&](const char* name) -> const Module* {
        Module& m = modules[name];
        if (m.code.empty())
        {
            m.code = loadSpirv(name);
            m.hash = compute::spirvHash(m.code);
        }
        return &m;
    };
    // Layout keys: equal keys go only to layouts made identically below.
    const uint64_t pushKey = compute::layoutKey(sizeof(GpuBakePush), VK_SHADER_STAGE_COMPUTE_BIT);
    const uint64_t rasterKey = compute::layoutKey(sizeof(GpuRasterPush), VK_SHADER_STAGE_COMPUTE_BIT);
    const uint64_t imageKey = compute::layoutKey(sizeof(GpuImagePush), VK_SHADER_STAGE_COMPUTE_BIT);
    const uint64_t hwKey = compute::layoutKey(sizeof(GpuBakePush), VK_SHADER_STAGE_COMPUTE_BIT, "bake:tlas");
    auto pipeline = [&](VkPipelineLayout layout, const Module* mod, const char* entry) {
        const uint64_t key = layout == mLayoutSw ? pushKey : layout == mLayoutRaster ? rasterKey : layout == mLayoutImage ? imageKey : hwKey;
        return compute::sharedPipelines().get(vk, layout, key, mod->code, mod->hash, entry);
    };
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuBakePush)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    vkCreatePipelineLayout(vk.device, &lci, nullptr, &mLayoutSw);
    const Module* sw = module("bake_sw.spv");
    const Module* dilate = module("bake_dilate.spv");
    mShade = pipeline(mLayoutSw, sw, "shadeCS");
    mProjectSw = pipeline(mLayoutSw, sw, "projectCS");
    mAoSw = pipeline(mLayoutSw, sw, "aoCS");
    mJfaInit = pipeline(mLayoutSw, dilate, "jfaInitCS");
    mJfaStep = pipeline(mLayoutSw, dilate, "jfaStepCS");
    mThicknessSw = pipeline(mLayoutSw, sw, "thicknessCS");
    mBentSw = pipeline(mLayoutSw, sw, "bentCS");
    mRoundedSw = pipeline(mLayoutSw, sw, "roundedCS");
    mErrorSw = pipeline(mLayoutSw, sw, "errorCS");

    VkPushConstantRange rasterRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuRasterPush)};
    VkPipelineLayoutCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    rci.pushConstantRangeCount = 1;
    rci.pPushConstantRanges = &rasterRange;
    vkCreatePipelineLayout(vk.device, &rci, nullptr, &mLayoutRaster);
    const Module* raster = module(vk.shaderFloat64 ? "bake_raster64.spv" : "bake_raster32.spv");
    mClaim = pipeline(mLayoutRaster, raster, "claimCS");
    mSamplesPipe = pipeline(mLayoutRaster, raster, "samplesCS");

    VkPushConstantRange imageRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuImagePush)};
    VkPipelineLayoutCreateInfo ici{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ici.pushConstantRangeCount = 1;
    ici.pPushConstantRanges = &imageRange;
    vkCreatePipelineLayout(vk.device, &ici, nullptr, &mLayoutImage);
    const Module* image = module("bake_image.spv");
    for (const char* entry : {"accumulateCS", "resolveCS", "renormalizeCS", "maxCS", "heightCS", "thicknessApplyCS", "copyRedCS", "slopeCurvatureCS",
                              "histogramCS", "curvatureCS", "jfaInitCS", "jfaStepCS", "dilateApplyCS", "clearTilesCS", "packTexelsCS"})
        mImagePipes[entry] = pipeline(mLayoutImage, image, entry);
    if (!mHardware)
        return;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sci.bindingCount = 1;
    sci.pBindings = &b;
    vkCreateDescriptorSetLayout(vk.device, &sci, nullptr, &mSetLayout);
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &mSetLayout;
    vkCreatePipelineLayout(vk.device, &lci, nullptr, &mLayoutHw);
    const Module* hw = module("bake_hw.spv");
    mProjectHw = pipeline(mLayoutHw, hw, "projectCS");
    mAoHw = pipeline(mLayoutHw, hw, "aoCS");
    mThicknessHw = pipeline(mLayoutHw, hw, "thicknessCS");
    mBentHw = pipeline(mLayoutHw, hw, "bentCS");
    mRoundedHw = pipeline(mLayoutHw, hw, "roundedCS");
    mErrorHw = pipeline(mLayoutHw, hw, "errorCS");
}

Blas GpuBackend::buildBlas(VulkanContext& vk, const bake::MeshSoup& soup, uint32_t first, uint32_t count) const
{
    LILA_ZONE();

    Blas out;
    out.firstTriangle = first;
    GpuBuffer staging = hostBuffer(vk, soup.positions.data() + size_t(first) * 3, size_t(count) * 3 * sizeof(glm::vec3));
    GpuBuffer positions = makeBuffer(vk, staging.size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, Host::None);

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = positions.address;
    geom.geometry.triangles.vertexStride = sizeof(glm::vec3);
    geom.geometry.triangles.maxVertex = count * 3 - 1;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);

    out.buffer = makeBuffer(vk, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, Host::None);
    VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    aci.buffer = out.buffer.buffer;
    aci.size = sizes.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    vkCreateAccelerationStructureKHR(vk.device, &aci, nullptr, &out.as);
    const VkDeviceSize align = vk.asScratchAlignment;
    GpuBuffer scratch = makeBuffer(vk, sizes.buildScratchSize + align, 0, Host::None);
    info.dstAccelerationStructure = out.as;
    info.scratchData.deviceAddress = (scratch.address + align - 1) / align * align;
    VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    try
    {
        mSub.run(vk, [&](VkCommandBuffer cmd) {
            copy(cmd, staging, positions, staging.size);
            memoryBarrier(cmd);
            vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &ranges);
        });
    }
    catch (...)
    {
        for (GpuBuffer* b : {&staging, &positions, &scratch})
            destroy(vk, *b);
        throw;
    }
    for (GpuBuffer* b : {&staging, &positions, &scratch})
        destroy(vk, *b);
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, nullptr, out.as};
    out.address = vkGetAccelerationStructureDeviceAddressKHR(vk.device, &ai);
    return out;
}

Tlas GpuBackend::buildTlas(VulkanContext& vk, const std::vector<std::pair<uint32_t, const Blas*>>& instances) const
{
    LILA_ZONE();

    Tlas t;
    std::vector<VkAccelerationStructureInstanceKHR> inst(instances.size());
    std::vector<uint32_t> base(std::max<size_t>(instances.size(), 1), 0);
    for (size_t i = 0; i < instances.size(); ++i)
    {
        VkAccelerationStructureInstanceKHR& v = inst[i];
        v.transform.matrix[0][0] = v.transform.matrix[1][1] = v.transform.matrix[2][2] = 1.0f;
        v.instanceCustomIndex = instances[i].first;
        v.mask = 0xff;
        // Ray tracing judges winding looking along the ray (left handed), so the soups' counter-clockwise fronts already
        // count as front faces without VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR.
        v.flags = 0;
        v.accelerationStructureReference = instances[i].second->address;
        base[i] = instances[i].second->firstTriangle;
    }
    t.instances = hostBuffer(vk, inst.data(), inst.size() * sizeof(VkAccelerationStructureInstanceKHR),
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    t.instanceBase = hostBuffer(vk, base.data(), base.size() * sizeof(uint32_t));

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geom.geometry.instances.data.deviceAddress = t.instances.address;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geom;
    const uint32_t count = uint32_t(inst.size());
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
    t.buffer = makeBuffer(vk, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, Host::None);
    VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    aci.buffer = t.buffer.buffer;
    aci.size = sizes.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    vkCreateAccelerationStructureKHR(vk.device, &aci, nullptr, &t.as);
    const VkDeviceSize align = vk.asScratchAlignment;
    GpuBuffer scratch = makeBuffer(vk, sizes.buildScratchSize + align, 0, Host::None);
    info.dstAccelerationStructure = t.as;
    info.scratchData.deviceAddress = (scratch.address + align - 1) / align * align;
    VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    try
    {
        mSub.run(vk, [&](VkCommandBuffer cmd) { vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &ranges); });
    }
    catch (...)
    {
        destroy(vk, scratch);
        throw;
    }
    destroy(vk, scratch);

    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = mPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &mSetLayout;
    if (vkAllocateDescriptorSets(vk.device, &dai, &t.set) != VK_SUCCESS)
        throw std::runtime_error("GPU bake: out of descriptor sets");
    VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asw.accelerationStructureCount = 1;
    asw.pAccelerationStructures = &t.as;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asw};
    w.dstSet = t.set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(vk.device, 1, &w, 0, nullptr);
    return t;
}

const Tlas& GpuBackend::allTlas(VulkanContext& vk) const
{
    if (!mAllBuilt)
    {
        std::vector<std::pair<uint32_t, const Blas*>> inst;
        for (size_t h = 0; h < mHighs.size(); ++h)
            for (const Blas& b : mHighs[h]->blas)
                inst.emplace_back(uint32_t(h), &b);
        mAll = buildTlas(vk, inst);
        mAllBuilt = true;
    }
    return mAll;
}

void GpuBackend::prepare(const bake::BakeInputs& in)
{
    LILA_ZONE();

    if (mInputs)
        runTask([this](VulkanContext&) {
            releaseScene();
            return true;
        });
    mInputs = &in;
    mFailed = false;
    {
        std::scoped_lock lock(mCpuMutex);
        mCpu.reset();
        mCpuReady = false;
    }

    // Host side: the global high order, material tables and (compute traversal) flat BVHs.
    std::vector<const bake::HighInput*> highs;
    std::vector<std::pair<size_t, size_t>> local;
    mGroupBase.clear();
    mGroupHighs.clear();
    for (size_t g = 0; g < in.groups.size(); ++g)
    {
        mGroupBase.push_back(highs.size());
        mGroupHighs.push_back(in.groups[g].highs.size());
        for (size_t h = 0; h < in.groups[g].highs.size(); ++h)
        {
            highs.push_back(&in.groups[g].highs[h]);
            local.emplace_back(g, h);
        }
    }
    // Highs already on the device from an earlier bake are reused as they are.
    mHighs.assign(highs.size(), nullptr);
    std::vector<uint64_t> keys(highs.size());
    tbb::parallel_for(size_t(0), highs.size(), [&](size_t k) { keys[k] = bake::soupHash(highs[k]->soup) ^ (mHardware ? 0x5bd1e995ull : 0x2545f491ull); });
    std::vector<uint8_t> fresh(highs.size(), 0);
    for (size_t k = 0; k < highs.size(); ++k)
    {
        mHighs[k] = highCache().find(keys[k]);
        if (mHighs[k])
            continue;
        fresh[k] = 1;
        mHighs[k] = std::make_shared<GpuHigh>();
        const bake::MeshSoup& soup = highs[k]->soup;
        GpuHigh& gh = *mHighs[k];
        gh.triangles = soup.triangleCount();
        uint32_t slots = 1;
        for (uint32_t m : soup.materials)
            slots = std::max(slots, m + 1);
        gh.slotTriangle.assign(slots, kNone);
        for (uint32_t t = 0; t < soup.triangleCount(); ++t)
            if (gh.slotTriangle[soup.materials[t]] == kNone)
                gh.slotTriangle[soup.materials[t]] = t;
    }
    mMaterialBase.assign(highs.size(), 0);
    std::vector<GpuHighInfo> info(highs.size());
    for (size_t g = 0; g < in.groups.size(); ++g)
    {
        uint32_t base = 0;
        for (size_t k = mGroupBase[g]; k < mGroupBase[g] + mGroupHighs[g]; ++k)
        {
            const GpuHigh& gh = *mHighs[k];
            const uint32_t slots = uint32_t(gh.slotTriangle.size());
            mMaterialBase[k] = base;
            info[k].normals = gh.normals.address;
            info[k].materials = gh.materials.address;
            info[k].nodes = gh.nodes.address;
            info[k].corners = gh.corners.address;
            info[k].positions = gh.positions.address;
            info[k].uvs = gh.uvs.address;
            info[k].triangles = gh.triangles;
            info[k].materialBase = base;
            info[k].materialCount = slots;
            base += slots + 1;
        }
    }
    std::vector<geo::FlatBvh> flat(mHardware ? 0 : highs.size());
    if (!mHardware && std::ranges::find(fresh, uint8_t(1)) != fresh.end())
    {
        const bake::CpuBackend& c = cpu();
        tbb::parallel_for(size_t(0), highs.size(), [&](size_t k) {
            const auto [g, h] = local[k];
            const geo::MeshBvh* bvh = fresh[k] ? c.bvh(g, h) : nullptr;
            if (!bvh)
                return;
            flat[k] = bvh->exportFlat();
            const std::vector<uint32_t>& soupTri = c.soupTriangles(g, h);
            for (size_t s = 0; s < flat[k].corners.size(); s += 3)
            {
                uint32_t t;
                std::memcpy(&t, &flat[k].corners[s].w, 4);
                const uint32_t soupT = soupTri[t];
                std::memcpy(&flat[k].corners[s].w, &soupT, 4);
            }
        });
    }
    std::vector<std::vector<uint32_t>> islands(in.groups.size());
    for (size_t g = 0; g < in.groups.size(); ++g)
    {
        const auto& lows = in.groups[g].lows;
        std::vector<uint32_t>& t = islands[g];
        t.resize(lows.size());
        uint32_t base = 0;
        for (size_t l = 0; l < lows.size(); ++l)
        {
            t[l] = base;
            base += lows[l].soup.triangleCount();
        }
        for (const bake::LowInput& low : lows)
            for (uint32_t tri = 0; tri < low.soup.triangleCount(); ++tri)
                t.push_back(bake::packRgba8(glm::vec4(bake::paletteColour(tri < low.islands.size() ? low.islands[tri] : 0u), 1.0f)));
    }
    size_t maxHighs = 1;
    for (size_t n : mGroupHighs)
        maxHighs = std::max(maxHighs, n);
    std::vector<uint32_t> iota(std::max(maxHighs, highs.size()));
    for (size_t i = 0; i < iota.size(); ++i)
        iota[i] = uint32_t(i);

    // Device side, a step per high (and per BLAS chunk) so a big scene never holds the render thread for long.
    std::deque<std::function<void(VulkanContext&)>> steps;
    for (size_t k = 0; k < highs.size(); ++k)
    {
        if (!fresh[k])
            continue;
        steps.push_back([this, k, &highs, &flat, &info](VulkanContext& vk) {
            const bake::MeshSoup& soup = highs[k]->soup;
            GpuHigh& gh = *mHighs[k];
            std::vector<GpuBuffer> staging;
            auto upload = [&](const void* data, size_t size) {
                staging.push_back(hostBuffer(vk, data, size));
                return makeBuffer(vk, size, 0, Host::None);
            };
            gh.normals = upload(soup.normals.data(), soup.normals.size() * sizeof(glm::vec3));
            gh.materials = upload(soup.materials.data(), soup.materials.size() * sizeof(uint32_t));
            std::vector<std::pair<const GpuBuffer*, GpuBuffer*>> copies{{&staging[0], &gh.normals}, {&staging[1], &gh.materials}};
            if (!flat.empty() && !flat[k].nodes.empty())
            {
                gh.nodes = upload(flat[k].nodes.data(), flat[k].nodes.size() * sizeof(geo::FlatBvh::Node));
                gh.corners = upload(flat[k].corners.data(), flat[k].corners.size() * sizeof(glm::vec4));
                copies.push_back({&staging[2], &gh.nodes});
                copies.push_back({&staging[3], &gh.corners});
            }
            info[k].normals = gh.normals.address;
            info[k].materials = gh.materials.address;
            info[k].nodes = gh.nodes.address;
            info[k].corners = gh.corners.address;
            try
            {
                mSub.run(vk, [&](VkCommandBuffer cmd) {
                    for (auto [from, to] : copies)
                        copy(cmd, *from, *to, from->size);
                });
            }
            catch (...)
            {
                for (GpuBuffer& b : staging)
                    destroy(vk, b);
                throw;
            }
            for (GpuBuffer& b : staging)
                destroy(vk, b);
        });
        if (mHardware)
            for (uint32_t first = 0; first < highs[k]->soup.triangleCount(); first += kBlasChunk)
                steps.push_back([this, k, first, &highs](VulkanContext& vk) {
                    const bake::MeshSoup& soup = highs[k]->soup;
                    mHighs[k]->blas.push_back(buildBlas(vk, soup, first, std::min(kBlasChunk, soup.triangleCount() - first)));
                });
    }
    steps.push_back([this, &info, &islands, &iota, &in](VulkanContext& vk) {
        mHighInfoHost = info;
        mHighInfo = hostBuffer(vk, info.data(), info.size() * sizeof(GpuHighInfo));
        mIota = hostBuffer(vk, iota.data(), iota.size() * sizeof(uint32_t));
        for (const auto& t : islands)
            mIslands.push_back(hostBuffer(vk, t.data(), t.size() * sizeof(uint32_t)));
        if (!mHardware)
            return;
        if (!mPool)
        {
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, uint32_t(in.groups.size() + 1)};
            VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pci.maxSets = uint32_t(in.groups.size() + 1);
            pci.poolSizeCount = 1;
            pci.pPoolSizes = &size;
            vkCreateDescriptorPool(vk.device, &pci, nullptr, &mPool);
        }
        mGroupTlas.resize(in.groups.size());
        for (size_t g = 0; g < in.groups.size(); ++g)
        {
            std::vector<std::pair<uint32_t, const Blas*>> inst;
            for (size_t h = 0; h < mGroupHighs[g]; ++h)
                for (const Blas& b : mHighs[mGroupBase[g] + h]->blas)
                    inst.emplace_back(uint32_t(h), &b);
            if (!inst.empty())
                mGroupTlas[g] = buildTlas(vk, inst);
        }
    });

    try
    {
        runTask([this, &steps](VulkanContext& vk) {
            initDevice(vk);
            if (steps.empty())
                return true;
            steps.front()(vk);
            steps.pop_front();
            return steps.empty();
        });
        for (size_t k = 0; k < highs.size(); ++k)
            if (fresh[k])
                highCache().add(keys[k], mHighs[k]);
    }
    catch (const std::exception& e)
    {
        fail(e.what());
    }
}

int GpuBackend::bandRows(int width, int samplesPerTexel) const
{
    const int grid = std::max(1, int(std::sqrt(double(std::max(1, samplesPerTexel)))));
    const size_t perRow = size_t(std::max(1, width)) * size_t(grid * grid);
    size_t budget = 64ull << 20;
    if (mVk.memoryBudget)
    {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(mVk.allocator, budgets);
        const VkPhysicalDeviceMemoryProperties* props = nullptr;
        vmaGetMemoryProperties(mVk.allocator, &props);
        uint64_t free = 0;
        for (uint32_t h = 0; h < props->memoryHeapCount; ++h)
            if (props->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                free = std::max(free, budgets[h].budget > budgets[h].usage ? budgets[h].budget - budgets[h].usage : 0);
        budget = std::clamp<size_t>(size_t(free / (2 * kSlots)), 8ull << 20, 512ull << 20);
    }
    const size_t samples = std::min(kBandSamples, budget / kDeviceBytesPerSample);
    return int(std::clamp<size_t>(samples / perRow, 1, 4096));
}

BandSlot* GpuBackend::heldSlot() const
{
    auto it = tHeld.find(mId);
    return it == tHeld.end() ? nullptr : it->second;
}

BandSlot* GpuBackend::acquireSlot() const
{
    if (BandSlot* s = heldSlot())
        return s;
    std::unique_lock lock(mSlotMutex);
    mSlotCv.wait(lock, [&] { return !mFree.empty() || mSlots.size() < kSlots; });
    BandSlot* s = nullptr;
    if (!mFree.empty())
    {
        s = mFree.back();
        mFree.pop_back();
    }
    else
    {
        mSlots.push_back(std::make_unique<BandSlot>());
        s = mSlots.back().get();
    }
    tHeld[mId] = s;
    return s;
}

void GpuBackend::endBand(std::span<const TexelSample>) const
{
    BandSlot* s = heldSlot();
    if (!s)
        return;
    tHeld.erase(mId);
    s->resident = false;
    s->key = nullptr;
    {
        std::scoped_lock lock(mSlotMutex);
        mFree.push_back(s);
    }
    mSlotCv.notify_one();
}

void GpuBackend::reserve(BandSlot& s, size_t n) const
{
    if (s.capacity >= n)
        return;
    LILA_ZONE();

    for (GpuBuffer* b : {&s.staging, &s.samples, &s.hits, &s.hitsRead, &s.out, &s.outRead})
        destroy(mVk, *b);
    s.capacity = 0;
    s.staging = makeBuffer(mVk, n * sizeof(TexelSample), 0, Host::Write);
    s.samples = makeBuffer(mVk, n * sizeof(TexelSample), 0, Host::None);
    s.hits = makeBuffer(mVk, n * sizeof(HitRecord), 0, Host::None);
    s.hitsRead = makeBuffer(mVk, n * sizeof(HitRecord), 0, Host::Read);
    s.out = makeBuffer(mVk, n * sizeof(glm::vec4), 0, Host::None);
    s.outRead = makeBuffer(mVk, n * sizeof(glm::vec4), 0, Host::Read);
    s.capacity = n;
}

void GpuBackend::runSliced(size_t count, std::atomic<double>& rate, double costPerItem, const std::function<void(VkCommandBuffer)>& begin,
                           const std::function<void(VkCommandBuffer, uint32_t, uint32_t)>& record,
                           const std::function<void(VkCommandBuffer)>& end) const
{
    size_t next = 0;
    runTask([&](VulkanContext& vk) {
        initDevice(vk);
        const double perSlice = rate.load() * kSliceMs / std::max(costPerItem, 1.0);
        const size_t n = std::min(count - next, std::max<size_t>(256, size_t(perSlice)));
        const double ms = mSub.run(vk, [&](VkCommandBuffer cmd) {
            if (next == 0 && begin)
            {
                begin(cmd);
                memoryBarrier(cmd);
            }
            // Dispatches stay under the 65535 workgroup limit.
            constexpr uint32_t kMaxDispatch = 65535u * 64u;
            for (size_t f = next; f < next + n; f += kMaxDispatch)
                record(cmd, uint32_t(f), uint32_t(std::min<size_t>(kMaxDispatch, next + n - f)));
            if (next + n == count && end)
            {
                memoryBarrier(cmd);
                end(cmd);
            }
        });
        if (n >= 4096)
        {
            const double measured = double(n) * costPerItem / std::max(ms, 0.05);
            rate = 0.5 * rate.load() + 0.5 * measured;
        }
        next += n;
        return next >= count;
    });
}

void GpuBackend::traceProjection(size_t group, std::span<const TexelSample> samples, std::span<HitRecord> hits) const
{
    LILA_ZONE();

    if (BandSlot* s = heldSlot())
        s->resident = false;
    if (mFailed || samples.empty())
    {
        cpu().traceProjection(group, samples, hits);
        return;
    }
    if (group >= mGroupHighs.size() || mGroupHighs[group] == 0 || (mHardware && !mGroupTlas[group].as))
    {
        // Nothing to hit, as the CPU tracer finds it.
        for (HitRecord& r : hits)
        {
            r = HitRecord{};
            r.high = bake::kNoHit;
            r.triangle = bake::kNoHit;
        }
        return;
    }
    BandSlot* slot = acquireSlot();
    slot->resident = false;
    try
    {
        const size_t n = samples.size();
        reserve(*slot, n);
        std::memcpy(slot->staging.mapped, samples.data(), n * sizeof(TexelSample));
        vmaFlushAllocation(mVk.allocator, slot->staging.alloc, 0, VK_WHOLE_SIZE);
        GpuBakePush p;
        p.samples = slot->samples.address;
        p.hits = slot->hits.address;
        p.highs = mHighInfo.address + mGroupBase[group] * sizeof(GpuHighInfo);
        p.highList = mIota.address;
        p.highCount = uint32_t(mGroupHighs[group]);
        p.flags = mInputs->groups[group].settings.ignoreBackfaces ? kFlagCullBack : 0u;
        const VkPipeline pipe = mHardware ? mProjectHw : mProjectSw;
        const VkPipelineLayout layout = mHardware ? mLayoutHw : mLayoutSw;
        if (mHardware)
            p.instanceBase = mGroupTlas[group].instanceBase.address;
        runSliced(
            n, mProjectRate, 1.0, [&](VkCommandBuffer cmd) { copy(cmd, slot->staging, slot->samples, n * sizeof(TexelSample)); },
            [&](VkCommandBuffer cmd, uint32_t first, uint32_t count) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
                if (mHardware)
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &mGroupTlas[group].set, 0, nullptr);
                GpuBakePush q = p;
                q.first = first;
                q.count = count;
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
                vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
            },
            [&](VkCommandBuffer cmd) { copy(cmd, slot->hits, slot->hitsRead, n * sizeof(HitRecord)); });
        vmaInvalidateAllocation(mVk.allocator, slot->hitsRead.alloc, 0, VK_WHOLE_SIZE);
        std::memcpy(hits.data(), slot->hitsRead.mapped, n * sizeof(HitRecord));
        slot->key = samples.data();
        slot->count = n;
        slot->group = group;
        slot->resident = true;
    }
    catch (const std::exception& e)
    {
        fail(e.what());
        cpu().traceProjection(group, samples, hits);
    }
}

const GpuBuffer& GpuBackend::idTable(const bake::MapDef& def, const bake::MapContext& ctx) const
{
    const std::string key = std::to_string(ctx.groupIndex) + "|" + ctx.settings.options.dump();
    std::scoped_lock lock(mTableMutex);
    auto it = mIdTables.find(key);
    if (it != mIdTables.end())
        return it->second;
    // The CPU kernel itself names each slot's colour, so palettes and sources stay its business.
    std::vector<TexelSample> samples;
    std::vector<HitRecord> hits;
    const size_t base = mGroupBase[ctx.groupIndex];
    for (size_t h = 0; h < mGroupHighs[ctx.groupIndex]; ++h)
    {
        const GpuHigh& gh = *mHighs[base + h];
        for (size_t s = 0; s <= gh.slotTriangle.size(); ++s)
        {
            HitRecord r{};
            r.flags = bake::kHitValid;
            r.high = uint32_t(h);
            r.triangle = s < gh.slotTriangle.size() && gh.slotTriangle[s] != kNone ? gh.slotTriangle[s] : 0u;
            hits.push_back(r);
            samples.push_back({});
        }
    }
    std::vector<glm::vec4> out(hits.size(), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    std::vector<uint8_t> valid(hits.size(), 0);
    if (def.cpu)
        def.cpu(ctx, samples, hits, out, valid);
    std::vector<uint32_t> table(hits.size());
    for (size_t i = 0; i < hits.size(); ++i)
        table[i] = bake::packRgba8(glm::vec4(glm::vec3(out[i]), 1.0f));
    return mIdTables.emplace(key, hostBuffer(mVk, table.data(), table.size() * sizeof(uint32_t))).first->second;
}

void GpuBackend::shade(const bake::MapDef& def, const bake::MapContext& ctx, std::span<const TexelSample> samples,
                       std::span<const HitRecord> hits, std::span<glm::vec4> out, std::span<uint8_t> valid) const
{
    LILA_ZONE();

    const KernelChoice k = kernelFor(def, ctx.settings);
    BandSlot* slot = heldSlot();
    if ((k.map < 0 && !k.ao) || mFailed || !slot || !slot->resident || slot->key != samples.data() || slot->count != samples.size() ||
        slot->group != ctx.groupIndex)
    {
        Backend::shade(def, ctx, samples, hits, out, valid);
        return;
    }
    try
    {
        const size_t n = samples.size();
        const size_t g = ctx.groupIndex;
        GpuBakePush p;
        p.samples = slot->samples.address;
        p.hits = slot->hits.address;
        p.outFloat = slot->out.address;
        p.outHalf = slot->out.address;
        p.highs = mHighInfo.address + mGroupBase[g] * sizeof(GpuHighInfo);
        p.highList = mIota.address;
        p.highCount = uint32_t(mGroupHighs[g]);
        p.flags = k.half ? kFlagHalf : 0u;
        p.boundsMin = glm::vec4(mInputs->highMin, 0.0f);
        p.boundsExt = glm::vec4(mInputs->highMax - mInputs->highMin, 0.0f);
        VkPipeline pipe = mShade;
        VkPipelineLayout layout = mLayoutSw;
        const VkDescriptorSet* set = nullptr;
        double cost = 1.0;
        std::atomic<double>* rate = &mShadeRate;
        if (k.ao)
        {
            const bake::GroupInputs& gi = ctx.group;
            const int rays = std::clamp(ctx.settings.options.value("rays", 64), 1, 4096);
            const float diag = glm::length(gi.highMax - gi.highMin);
            const double distOpt = ctx.settings.options.value("distance", 0.0);
            const bake::OccluderScope scope = bake::occluderScope(ctx.settings.options.value("scope", gi.settings.aoScope));
            p.rays = uint32_t(rays);
            p.boundsMin.w = distOpt > 0.0 ? float(distOpt) : std::max(diag, 1e-3f);
            p.boundsExt.w = std::max(1e-4f * diag, 1e-4f);
            if (scope == bake::OccluderScope::Self)
                p.flags |= kFlagScopeSelf;
            cost = double(rays);
            rate = &mRayRate;
            pipe = mHardware ? mAoHw : mAoSw;
            layout = mHardware ? mLayoutHw : mLayoutSw;
            if (scope == bake::OccluderScope::All)
            {
                p.highs = mHighInfo.address;
                p.highCount = uint32_t(mHighs.size());
            }
            if (mHardware)
            {
                if (scope != bake::OccluderScope::All)
                {
                    if (!mGroupTlas[g].as)
                    {
                        Backend::shade(def, ctx, samples, hits, out, valid);
                        return;
                    }
                    set = &mGroupTlas[g].set;
                }
            }
        }
        else
        {
            p.map = uint32_t(k.map);
            if (k.map == kMapNormal)
                p.flags |= (ctx.settings.options.value("greenUp", false) ? kFlagGreenUp : 0u) |
                           (ctx.settings.options.value("flipX", false) ? kFlagFlipX : 0u);
            if (k.map == kMapId)
                p.table = idTable(def, ctx).address;
            if (k.map == kMapIslands)
            {
                p.tableBase = mIslands[g].address;
                p.table = mIslands[g].address + ctx.group.lows.size() * sizeof(uint32_t);
            }
        }
        if (k.ao && mHardware && !set)
        {
            runTask([this](VulkanContext& vk) {
                initDevice(vk);
                allTlas(vk);
                return true;
            });
            set = &mAll.set;
        }
        runSliced(
            n, *rate, cost, nullptr,
            [&](VkCommandBuffer cmd, uint32_t first, uint32_t count) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
                if (set)
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, set, 0, nullptr);
                GpuBakePush q = p;
                q.first = first;
                q.count = count;
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
                vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
            },
            [&](VkCommandBuffer cmd) { copy(cmd, slot->out, slot->outRead, n * (k.half ? 8 : 16)); });
        vmaInvalidateAllocation(mVk.allocator, slot->outRead.alloc, 0, VK_WHOLE_SIZE);
        if (k.half)
        {
            const uint64_t* h = static_cast<const uint64_t*>(slot->outRead.mapped);
            for (size_t i = 0; i < n; ++i)
            {
                const glm::vec4 v = glm::unpackHalf4x16(h[i]);
                valid[i] = v.w > 0.5f ? 1 : 0;
                out[i] = glm::vec4(glm::vec3(v), 1.0f);
            }
        }
        else
        {
            const glm::vec4* f = static_cast<const glm::vec4*>(slot->outRead.mapped);
            for (size_t i = 0; i < n; ++i)
            {
                valid[i] = f[i].w > 0.5f ? 1 : 0;
                out[i] = glm::vec4(glm::vec3(f[i]), 1.0f);
            }
        }
    }
    catch (const std::exception& e)
    {
        fail(e.what());
        Backend::shade(def, ctx, samples, hits, out, valid);
    }
}

void GpuBackend::dilate(bake::TextureBuffer& tex, int padding) const
{
    if (padding == 0)
        return;
    if (mFailed || tex.texelCount() < 128 * 128)
    {
        Backend::dilate(tex, padding);
        return;
    }
    try
    {
        gpuDilate(tex, padding);
    }
    catch (const std::exception& e)
    {
        fail(e.what());
        Backend::dilate(tex, padding);
    }
}

void GpuBackend::gpuDilate(bake::TextureBuffer& tex, int padding) const
{
    LILA_ZONE();

    const size_t n = tex.texelCount();
    if (std::ranges::none_of(tex.covered, [](uint8_t c) { return c != 0; }))
        return;
    std::vector<uint8_t> mask((n + 3) / 4 * 4, 0);
    for (size_t i = 0; i < n; ++i)
        mask[i] = tex.covered[i] ? 1 : 0;
    GpuBuffer covered = hostBuffer(mVk, mask.data(), mask.size());
    GpuBuffer seeds[2] = {makeBuffer(mVk, n * 4, 0, Host::None), makeBuffer(mVk, n * 4, 0, Host::None)};
    GpuBuffer read = makeBuffer(mVk, n * 4, 0, Host::Read);
    auto cleanup = [&] {
        for (GpuBuffer* b : {&covered, &seeds[0], &seeds[1], &read})
            destroy(mVk, *b);
    };

    // Steps k, k/2 .. 1 reach 2k - 1 texels; two more at 2 and 1 mend jump flood's rare misses.
    const int extent = std::max(tex.width, tex.height);
    const int reach = padding < 0 ? extent : std::min(padding, extent);
    std::vector<int> passes;
    for (int k = int(std::bit_ceil(unsigned((reach + 1) / 2 + 1))); k >= 1; k /= 2)
        passes.push_back(k);
    passes.push_back(2);
    passes.push_back(1);
    int cur = 0;
    size_t pass = 0;
    try
    {
        runTask([&](VulkanContext& vk) {
            initDevice(vk);
            mSub.run(vk, [&](VkCommandBuffer cmd) {
                DilatePush p;
                p.covered = covered.address;
                p.width = uint32_t(tex.width);
                p.height = uint32_t(tex.height);
                const uint32_t gx = (p.width + 7) / 8, gy = (p.height + 7) / 8;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass == 0 ? mJfaInit : mJfaStep);
                if (pass == 0)
                {
                    p.seedsOut = seeds[0].address;
                    vkCmdPushConstants(cmd, mLayoutSw, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
                    vkCmdDispatch(cmd, gx, gy, 1);
                    memoryBarrier(cmd);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mJfaStep);
                }
                // Passes over small textures share a slice; big ones get one each.
                const size_t perSlice = n <= (size_t(4) << 20) ? passes.size() : 1;
                for (size_t k = 0; k < perSlice && pass < passes.size(); ++k, ++pass)
                {
                    p.seedsIn = seeds[cur].address;
                    p.seedsOut = seeds[1 - cur].address;
                    p.step = passes[pass];
                    vkCmdPushConstants(cmd, mLayoutSw, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
                    vkCmdDispatch(cmd, gx, gy, 1);
                    memoryBarrier(cmd);
                    cur = 1 - cur;
                }
                if (pass == passes.size())
                    copy(cmd, seeds[cur], read, n * 4);
            });
            return pass == passes.size();
        });
    }
    catch (...)
    {
        cleanup();
        throw;
    }
    vmaInvalidateAllocation(mVk.allocator, read.alloc, 0, VK_WHOLE_SIZE);
    const uint32_t* seed = static_cast<const uint32_t*>(read.mapped);
    const int w = tex.width;
    tbb::parallel_for(size_t(0), n, [&](size_t i) {
        if (tex.covered[i] || seed[i] == kNone)
            return;
        const int dx = std::abs(int(seed[i] % uint32_t(w)) - int(i % size_t(w)));
        const int dy = std::abs(int(seed[i] / uint32_t(w)) - int(i / size_t(w)));
        if (padding < 0 || std::max(dx, dy) <= padding)
            tex.value[i] = tex.value[seed[i]];
    });
    cleanup();
}
void GpuBackend::runSteps(std::vector<GpuStep>& steps, const std::function<bool()>& cancelled) const
{
    LILA_ZONE();

    constexpr uint32_t kQueries = 1024;
    size_t step = 0, done = 0;
    bool stopped = false;
    runTask([&](VulkanContext& vk) {
        initDevice(vk);
        if (cancelled && cancelled())
        {
            stopped = true;
            return true;
        }
        if (!mQueries)
        {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = kQueries;
            vkCreateQueryPool(vk.device, &qci, nullptr, &mQueries);
        }
        std::vector<std::pair<std::atomic<double>*, double>> used;
        std::vector<const std::string*> labels;
        double predicted = 0.0;
        const double ms = mSub.run(vk, [&](VkCommandBuffer cmd) {
            vkCmdResetQueryPool(cmd, mQueries, 0, kQueries);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQueries, 0);
            while (step < steps.size())
            {
                GpuStep& st = steps[step];
                size_t n = st.count - done;
                double perItem = 0.0;
                if (st.rate && n)
                {
                    perItem = st.cost / std::max(st.rate->load(), 1e-3);
                    n = compute::sliceFit(n, perItem, predicted, mSliceMs);
                    if (n == 0)
                        break;
                    used.emplace_back(st.rate, double(n) * perItem);
                }
                if (n)
                    st.record(cmd, uint32_t(done), uint32_t(n));
                if (labels.size() + 2 < kQueries)
                {
                    labels.push_back(&st.label);
                    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQueries, uint32_t(labels.size()));
                }
                predicted += double(n) * perItem;
                done += n;
                if (done < st.count)
                    break;
                memoryBarrier(cmd);
                ++step;
                done = 0;
            }
        });
        std::vector<uint64_t> stamps(labels.size() + 1);
        if (!labels.empty() && vkGetQueryPoolResults(vk.device, mQueries, 0, uint32_t(stamps.size()), stamps.size() * sizeof(uint64_t), stamps.data(),
                                                     sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            const double period = double(vk.properties.limits.timestampPeriod) * 1e-6;
            std::scoped_lock lock(mGpuTimeMutex);
            for (size_t i = 0; i < labels.size(); ++i)
                mGpuTimes[*labels[i]] += double(stamps[i + 1] - stamps[i]) * period;
        }
        if (predicted > 2.0 && ms > 0.05)
        {
            // Each rate that ran a good share of the slice moves toward what the slice measured.
            for (auto& [rate, share] : used)
                if (share > 0.2 * predicted)
                    *rate = compute::adaptRate(rate->load(), predicted, ms);
        }
        return step >= steps.size();
    });
    if (stopped)
        throw progress::Cancelled();
}

bool GpuBackend::residentSupports(const bake::MapDef& def, const bake::MapSettings&) const
{
    // LILA_BAKE_GPU_RESIDENT=0 keeps every map on the band path (shade per band, CPU accumulation), for comparisons.
    static const bool off = [] {
        const char* e = std::getenv("LILA_BAKE_GPU_RESIDENT");
        return e && std::string_view(e) == "0";
    }();
    if (mFailed || off)
        return false;
    // The built-in maps whose kernels and finalize steps have device versions; an extension map runs per band.
    static const std::set<std::string, std::less<>> kMaps = {
        "normal",    "objectNormal", "position", "height",   "id",       "alpha",           "uvIslands", "ao",          "curvature",
        "normalToCurvature", "convexity", "cavity", "edgeWear", "thickness", "bentNormal",   "roundedNormal", "vertexColor", "baseColor",
        "roughness", "metallic",     "emissive", "materialOpacity", "wireframe", "islandBorder", "error",    "gradient",    "normalToHeight"};
    return kMaps.contains(def.id) && def.cpu;
}

/** How a resident texture finishes after its resolve. */
enum class Finish
{
    /** No finalize, or one that only writes meta (run on the CPU after the read back). */
    None,
    Renormalize,
    Height,
    Thickness,
    Curvature,
    /** The finalize runs on the CPU (normal to height's Poisson solve), and so does its dilation. */
    Cpu,
};

Finish finishOf(const std::string& id)
{
    if (id == "normal" || id == "objectNormal" || id == "bentNormal" || id == "roundedNormal")
        return Finish::Renormalize;
    if (id == "height")
        return Finish::Height;
    if (id == "thickness")
        return Finish::Thickness;
    if (id == "curvature" || id == "normalToCurvature" || id == "convexity" || id == "cavity" || id == "edgeWear")
        return Finish::Curvature;
    if (id == "normalToHeight")
        return Finish::Cpu;
    return Finish::None;
}

/** The curvature maps' source, as sourceOf in MapsSurface.cpp. */
bool curvatureFromMesh(const bake::MapSettings& s)
{
    const char* fallback = s.map == "edgeWear" || s.map == "normalToCurvature" ? "normals" : "mesh";
    if (s.map == "normalToCurvature")
        return false;
    const std::string v = s.options.is_object() ? s.options.value("source", std::string(fallback)) : std::string(fallback);
    return !(v == "normals" || v == "normal");
}

template <class T> T option(const bake::json& o, const char* key, T fallback)
{
    return o.is_object() ? o.value(key, fallback) : fallback;
}

/** Device-local copies of host data, uploaded together in one task. */
struct ResidentUploads
{
    GpuBackend& b;
    std::vector<std::pair<GpuBuffer, GpuBuffer*>> pending;

    void add(GpuBuffer& into, const void* data, size_t bytes)
    {
        if (!bytes)
            return;
        pending.emplace_back(hostBuffer(b.mVk, data, bytes), &into);
        into = makeBuffer(b.mVk, bytes, 0, Host::None);
    }

    void flush()
    {
        if (pending.empty())
            return;
        try
        {
            b.runTask([&](VulkanContext& vk) {
                b.initDevice(vk);
                b.mSub.run(vk, [&](VkCommandBuffer cmd) {
                    for (auto& [from, to] : pending)
                        copy(cmd, from, *to, from.size);
                });
                return true;
            });
        }
        catch (...)
        {
            for (auto& [from, to] : pending)
                destroy(b.mVk, from);
            pending.clear();
            throw;
        }
        for (auto& [from, to] : pending)
            destroy(b.mVk, from);
        pending.clear();
    }
};

/**
 * A bake kept on the device: per band the raster claims samples, the projection traces them once and every map's
 * kernel shades them into per-texel accumulators of the textures they write. Only finished textures come back.
 */
class GpuResident final : public bake::ResidentBake, public DeviceBake
{
public:
    GpuResident(GpuBackend& backend, const bake::BakeInputs& inputs, std::vector<bake::ResidentMap> maps, const bake::OutputSettings& output)
        : mB(backend), mVk(backend.mVk), mIn(&inputs), mMaps(std::move(maps)), mOut(output)
    {
        for (const bake::ResidentMap& m : mMaps)
            mNeedEdges = mNeedEdges || m.def->id == "wireframe" || m.def->id == "islandBorder";
        initLows();
    }

    ~GpuResident() override
    {
        try
        {
            mB.runTask([this](VulkanContext&) {
                release();
                return true;
            });
        }
        catch (const std::exception& e)
        {
            log::warn("GPU bake: cleanup failed: {}", e.what());
        }
    }

    int bandRows(int width, int samplesPerTexel) const override
    {
        size_t budget = 512ull << 20;
        if (mVk.memoryBudget)
        {
            VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
            vmaGetHeapBudgets(mVk.allocator, budgets);
            const VkPhysicalDeviceMemoryProperties* props = nullptr;
            vmaGetMemoryProperties(mVk.allocator, &props);
            uint64_t free = 0;
            for (uint32_t h = 0; h < props->memoryHeapCount; ++h)
                if (props->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    free = std::max(free, budgets[h].budget > budgets[h].usage ? budgets[h].budget - budgets[h].usage : 0);
            budget = std::clamp<size_t>(size_t(free / 4), 64ull << 20, 1536ull << 20);
        }
        const size_t perRow = size_t(std::max(1, width)) * size_t(std::max(1, samplesPerTexel)) * kSlotBytes;
        return int(std::clamp<size_t>(budget / perRow, 1, 16384));
    }

    bool fits(size_t texels) const override
    {
        if (!mVk.memoryBudget)
            return true;
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(mVk.allocator, budgets);
        const VkPhysicalDeviceMemoryProperties* props = nullptr;
        vmaGetMemoryProperties(mVk.allocator, &props);
        uint64_t free = 0;
        for (uint32_t h = 0; h < props->memoryHeapCount; ++h)
            if (props->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                free = std::max(free, budgets[h].budget > budgets[h].usage ? budgets[h].budget - budgets[h].usage : 0);
        // Accumulators and votes per texel, plus room for a band and one texture's dilation and read back.
        const uint64_t need = uint64_t(texels) * 32 + (256ull << 20);
        return need < free * 8 / 10;
    }

    bake::ResidentStats bakeTile(const bake::ResidentTile& tile) override;
    bool finish(bake::TextureBuffer& tex, int padding) override;

    void rebind(const bake::BakeInputs& inputs) override;
    void clearTiles(const bake::TextureBuffer& tex, std::span<const uint32_t> tiles, int tile) override;
    void finishInto(bake::TextureBuffer& tex, int padding, const GpuImageRef& image) override;
    void drop(const bake::TextureBuffer& tex) override;

    bake::json timings() const override
    {
        bake::json t = bake::json::object();
        {
            std::scoped_lock lock(mB.mGpuTimeMutex);
            for (const auto& [label, ms] : mB.mGpuTimes)
                t["gpu:" + label] = ms * 1e-3;
        }
        t["gpuSetup"] = mSetupSeconds;
        return t;
    }

private:
    /** Device bytes per sample slot: sample, hit, map output, valid flag and claim. */
    static constexpr size_t kSlotBytes = sizeof(TexelSample) + sizeof(HitRecord) + sizeof(glm::vec4) + 8;

    struct DevLow
    {
        GpuBuffer positions, normals, tangents, uvs, cageNormals, cageScale, cageSkew, cagePositions, materials, edges;
    };

    struct DevTex
    {
        GpuBuffer value, weight, count, exact, votes;
        uint32_t id = 0;
    };

    /** A map's kernel for one group: pipeline and push constants, minus the band's buffers. */
    struct Kernel
    {
        VkPipeline pipe = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        GpuBakePush push;
        double cost = 1.0;
        std::atomic<double>* rate = nullptr;
    };

    DevLow& low(size_t g, size_t l);
    DevTex& texture(const bake::TextureBuffer& tex);
    const Kernel& kernel(size_t map, size_t group);
    Kernel makeKernel(size_t map, size_t group);
    void scope(Kernel& k, size_t group, const std::string& name, VkPipeline sw, VkPipeline hw);
    /** The high's own normal maps per material slot of a group (0 when none applies). */
    uint64_t normalMapTable(size_t group);
    uint64_t materialTable(size_t group, const std::string& map);
    uint64_t curvatureTable(size_t group);
    uint64_t colorTable(size_t group, const std::string& set, bool first);
    /** A texture by path, uploaded once per bake; width 0 when it can't be read. */
    GpuTextureRef loadTexture(const std::string& path);
    /** Uploads the highs' positions and uvs (normal and material maps read them) and rewrites the high table. */
    void needHighSurfaces(size_t group, ResidentUploads& up);
    void reserveBand(size_t slots, size_t items);
    /** Keeps a host-visible table alive for the bake and returns its address. */
    uint64_t table(const void* data, size_t bytes);
    /** Frees everything made for the inputs; keepTextures keeps the textures' accumulators. */
    void release(bool keepTextures = false);
    /** Sizes the per-low tables for mIn. */
    void initLows();
    bool finishImpl(bake::TextureBuffer& tex, int padding, const GpuImageRef* into);
    /** Steps that pack base's finished values into image's format and copy them into it. */
    void writeImage(std::vector<GpuStep>& steps, const GpuImagePush& base, GpuImage& image);

    GpuBackend& mB;
    VulkanContext& mVk;
    const bake::BakeInputs* mIn;
    std::vector<bake::ResidentMap> mMaps;
    bake::OutputSettings mOut;
    bool mNeedEdges = false;

    std::vector<std::vector<std::unique_ptr<DevLow>>> mLows;
    std::vector<GpuBuffer> mLowTables;
    std::map<std::pair<size_t, size_t>, Kernel> mKernels;
    std::map<const bake::TextureBuffer*, DevTex> mTex;
    uint32_t mNextTexId = 1;
    std::map<std::string, GpuTextureRef> mTextures;
    std::map<size_t, uint64_t> mNormalMapTables, mCurvatureTables;
    std::map<std::pair<size_t, std::string>, uint64_t> mMaterialTables, mColorTables;
    /** Buffers made for this bake: tables, textures. */
    std::vector<GpuBuffer> mOwned;

    // Band buffers, grown as needed.
    GpuBuffer mClaim, mSamples, mHits, mOutBuf, mValid, mItems, mStats, mStatsRead;
    size_t mSlotCap = 0, mItemCap = 0;
    // Finish buffers.
    GpuBuffer mSeeds[2], mScratch, mReduce, mReduceRead, mReadback;
    size_t mTexelCap = 0;
    // Finishing into images: the accumulators' working copy and the packed texels.
    GpuBuffer mWorkValue, mWorkCount, mPack;
    size_t mWorkCap = 0;
    std::vector<GpuBuffer> mStaging;
    GpuBuffer mTileList;
    size_t mTileListCap = 0;
    /** CPU time making kernels' tables and uploading lows (curvature, textures, ...). */
    double mSetupSeconds = 0.0;
};

uint64_t GpuResident::table(const void* data, size_t bytes)
{
    mOwned.push_back(hostBuffer(mVk, data, bytes));
    return mOwned.back().address;
}

void GpuResident::initLows()
{
    mLows.clear();
    mLows.resize(mIn->groups.size());
    mLowTables.resize(mIn->groups.size());
    for (size_t g = 0; g < mIn->groups.size(); ++g)
    {
        mLows[g].resize(mIn->groups[g].lows.size());
        std::vector<GpuLowInfo> zero(std::max<size_t>(1, mIn->groups[g].lows.size()));
        mLowTables[g] = hostBuffer(mVk, zero.data(), zero.size() * sizeof(GpuLowInfo));
    }
}

void GpuResident::rebind(const bake::BakeInputs& inputs)
{
    LILA_ZONE();

    mB.runTask([this](VulkanContext&) {
        release(true);
        return true;
    });
    mKernels.clear();
    mTextures.clear();
    mNormalMapTables.clear();
    mCurvatureTables.clear();
    mMaterialTables.clear();
    mColorTables.clear();
    mIn = &inputs;
    initLows();
}

void GpuResident::clearTiles(const bake::TextureBuffer& tex, std::span<const uint32_t> tiles, int tile)
{
    LILA_ZONE();

    DevTex& d = texture(tex);
    if (tiles.empty())
    {
        mB.runTask([&](VulkanContext& vk) {
            mB.initDevice(vk);
            mB.mSub.run(vk, [&](VkCommandBuffer cmd) {
                for (GpuBuffer* buf : {&d.value, &d.weight, &d.count, &d.exact, &d.votes})
                    if (buf->buffer)
                        vkCmdFillBuffer(cmd, buf->buffer, 0, VK_WHOLE_SIZE, 0);
            });
            return true;
        });
        return;
    }
    if (tiles.size() > mTileListCap)
    {
        destroy(mVk, mTileList);
        mTileListCap = std::max<size_t>(tiles.size(), 1024);
        mTileList = makeBuffer(mVk, mTileListCap * sizeof(uint32_t), 0, Host::Write);
    }
    std::memcpy(mTileList.mapped, tiles.data(), tiles.size_bytes());
    vmaFlushAllocation(mVk.allocator, mTileList.alloc, 0, VK_WHOLE_SIZE);
    GpuImagePush q;
    q.value = d.value.address;
    q.weight = d.weight.address;
    q.count = d.count.address;
    q.exact = d.exact.address;
    q.votes = d.votes.address;
    q.seedsIn = mTileList.address;
    q.width = uint32_t(tex.width);
    q.height = uint32_t(tex.height);
    q.step = tile;
    q.shift = uint32_t((tex.width + tile - 1) / tile);
    q.mode = tex.def && tex.def->resolve == bake::Resolve::Exact ? 1u : 0u;
    GpuBackend& b = mB;
    std::vector<GpuStep> steps;
    steps.push_back({tiles.size() * size_t(tile) * size_t(tile), 1.0, &b.mImageRate, [&b, q](VkCommandBuffer cmd, uint32_t first, uint32_t count) {
                         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mImagePipes.at("clearTilesCS"));
                         dispatchChunks(cmd, b.mLayoutImage, q, first, count, 64, [](GpuImagePush& r, uint32_t f, uint32_t c) {
                             r.first = f;
                             r.total = f + c;
                         });
                     },
                     "clear"});
    b.runSteps(steps);
}

void GpuResident::drop(const bake::TextureBuffer& tex)
{
    auto it = mTex.find(&tex);
    if (it == mTex.end())
        return;
    mB.runTask([&](VulkanContext&) {
        for (GpuBuffer* buf : {&it->second.value, &it->second.weight, &it->second.count, &it->second.exact, &it->second.votes})
            destroy(mVk, *buf);
        return true;
    });
    mTex.erase(it);
}

void GpuResident::writeImage(std::vector<GpuStep>& steps, const GpuImagePush& base, GpuImage& image)
{
    GpuBackend& b = mB;
    const size_t n = size_t(base.width) * size_t(base.height);
    GpuImagePush q = base;
    q.seedsOut = mPack.address;
    size_t items = n;
    switch (image.format())
    {
    case TextureFormat::RGBA16F: q.mode = 0; break;
    case TextureFormat::R16F:
        q.mode = 1;
        items = (n + 1) / 2;
        break;
    case TextureFormat::RGBA8: q.mode = 2; break;
    default: throw std::invalid_argument("GPU bake: images take RGBA16F, R16F or RGBA8");
    }
    steps.push_back({items, 1.0, &b.mImageRate, [&b, q](VkCommandBuffer cmd, uint32_t first, uint32_t count) {
                         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mImagePipes.at("packTexelsCS"));
                         dispatchChunks(cmd, b.mLayoutImage, q, first, count, 64, [](GpuImagePush& r, uint32_t f, uint32_t c) {
                             r.first = f;
                             r.total = f + c;
                         });
                     },
                     "write"});
    steps.push_back({1, 0.0, nullptr, [this, &image](VkCommandBuffer cmd, uint32_t, uint32_t) {
                         if (!image.prepare(mVk, cmd, mStaging))
                             throw std::runtime_error("GPU bake: cannot make the image " + image.desc().name);
                         image.beginWrite(cmd);
                         memoryBarrier(cmd);
                         VkBufferImageCopy region{};
                         region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                         region.imageExtent = {uint32_t(image.width()), uint32_t(image.height()), 1};
                         vkCmdCopyBufferToImage(cmd, mPack.buffer, image.image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                         memoryBarrier(cmd);
                         image.endWrite(cmd);
                     },
                     "write"});
}

void GpuResident::release(bool keepTextures)
{
    for (auto& per : mLows)
        for (auto& l : per)
            if (l)
                for (GpuBuffer* b : {&l->positions, &l->normals, &l->tangents, &l->uvs, &l->cageNormals, &l->cageScale, &l->cageSkew, &l->cagePositions,
                                     &l->materials, &l->edges})
                    destroy(mVk, *b);
    mLows.clear();
    for (GpuBuffer& b : mLowTables)
        destroy(mVk, b);
    mLowTables.clear();
    for (GpuBuffer& b : mOwned)
        destroy(mVk, b);
    mOwned.clear();
    if (keepTextures)
        return;
    for (auto& [k, t] : mTex)
        for (GpuBuffer* b : {&t.value, &t.weight, &t.count, &t.exact, &t.votes})
            destroy(mVk, *b);
    mTex.clear();
    for (GpuBuffer* b : {&mClaim, &mSamples, &mHits, &mOutBuf, &mValid, &mItems, &mStats, &mStatsRead, &mSeeds[0], &mSeeds[1], &mScratch, &mReduce,
                         &mReduceRead, &mReadback, &mWorkValue, &mWorkCount, &mPack, &mTileList})
        destroy(mVk, *b);
    for (GpuBuffer& b : mStaging)
        destroy(mVk, b);
    mStaging.clear();
}

GpuResident::DevLow& GpuResident::low(size_t g, size_t l)
{
    std::unique_ptr<DevLow>& slot = mLows[g][l];
    if (slot)
        return *slot;
    LILA_ZONE();

    const bake::LowInput& in = mIn->groups[g].lows[l];
    const bake::MeshSoup& s = in.soup;
    auto d = std::make_unique<DevLow>();
    ResidentUploads up{mB};
    up.add(d->positions, s.positions.data(), s.positions.size() * sizeof(glm::vec3));
    up.add(d->normals, s.normals.data(), s.normals.size() * sizeof(glm::vec3));
    up.add(d->tangents, in.tangents.data(), in.tangents.size() * sizeof(glm::vec4));
    up.add(d->uvs, s.uvs.data(), s.uvs.size() * sizeof(glm::vec2));
    up.add(d->cageNormals, in.cageNormals.data(), in.cageNormals.size() * sizeof(glm::vec3));
    up.add(d->cageScale, in.cageScale.data(), in.cageScale.size() * sizeof(float));
    const bool skew = in.cageSkew.size() == s.positions.size();
    if (skew)
        up.add(d->cageSkew, in.cageSkew.data(), in.cageSkew.size() * sizeof(float));
    const bool cage = in.cagePositions.size() == s.positions.size();
    if (cage)
        up.add(d->cagePositions, in.cagePositions.data(), in.cagePositions.size() * sizeof(glm::vec3));
    up.add(d->materials, s.materials.data(), s.materials.size() * sizeof(uint32_t));
    std::vector<uint32_t> edges;
    if (mNeedEdges)
    {
        const std::vector<uint8_t> wire = bake::lowEdgeFlags(in, false), border = bake::lowEdgeFlags(in, true);
        edges.resize(s.triangleCount());
        for (size_t t = 0; t < edges.size(); ++t)
            edges[t] = uint32_t(wire[t]) | (uint32_t(border[t]) << 3);
        up.add(d->edges, edges.data(), edges.size() * sizeof(uint32_t));
    }
    up.flush();
    GpuLowInfo info;
    info.positions = d->positions.address;
    info.normals = d->normals.address;
    info.tangents = d->tangents.address;
    info.uvs = d->uvs.address;
    info.cageNormals = d->cageNormals.address;
    info.cageScale = d->cageScale.address;
    info.cageSkew = d->cageSkew.address;
    info.cagePositions = d->cagePositions.address;
    info.materials = d->materials.address;
    info.edges = d->edges.address;
    info.triangles = s.triangleCount();
    info.hasSkew = skew ? 1u : 0u;
    std::memcpy(static_cast<GpuLowInfo*>(mLowTables[g].mapped) + l, &info, sizeof info);
    vmaFlushAllocation(mVk.allocator, mLowTables[g].alloc, 0, VK_WHOLE_SIZE);
    slot = std::move(d);
    return *slot;
}

GpuResident::DevTex& GpuResident::texture(const bake::TextureBuffer& tex)
{
    auto it = mTex.find(&tex);
    if (it != mTex.end())
        return it->second;
    LILA_ZONE();

    const size_t n = tex.texelCount();
    DevTex d;
    d.id = mNextTexId++;
    d.value = makeBuffer(mVk, n * sizeof(glm::vec4), 0, Host::None);
    d.weight = makeBuffer(mVk, n * sizeof(float), 0, Host::None);
    d.count = makeBuffer(mVk, n * sizeof(uint32_t), 0, Host::None);
    const bool exact = tex.def && tex.def->resolve == bake::Resolve::Exact;
    if (exact)
    {
        d.exact = makeBuffer(mVk, n * sizeof(uint32_t), 0, Host::None);
        d.votes = makeBuffer(mVk, n * sizeof(uint32_t), 0, Host::None);
    }
    mB.runTask([&](VulkanContext& vk) {
        mB.initDevice(vk);
        mB.mSub.run(vk, [&](VkCommandBuffer cmd) {
            for (GpuBuffer* b : {&d.value, &d.weight, &d.count, &d.exact, &d.votes})
                if (b->buffer)
                    vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
        });
        return true;
    });
    return mTex.emplace(&tex, std::move(d)).first->second;
}

GpuTextureRef GpuResident::loadTexture(const std::string& path)
{
    if (path.empty())
        return {};
    auto it = mTextures.find(path);
    if (it != mTextures.end())
        return it->second;
    GpuTextureRef ref;
    int w = 0, h = 0;
    std::vector<float> rgba;
    std::string err;
    if (bake::loadBakeTexture(path, w, h, rgba, err))
    {
        ResidentUploads up{mB};
        mOwned.emplace_back();
        up.add(mOwned.back(), rgba.data(), size_t(w) * size_t(h) * 4 * sizeof(float));
        up.flush();
        ref.texels = mOwned.back().address;
        ref.width = uint32_t(w);
        ref.height = uint32_t(h);
    }
    else
        log::warn("bake: can't read texture {}{}", path, err.empty() ? std::string() : ": " + err);
    mTextures.emplace(path, ref);
    return ref;
}

void GpuResident::needHighSurfaces(size_t g, ResidentUploads& up)
{
    GpuBackend& b = mB;
    bool changed = false;
    for (size_t h = 0; h < b.mGroupHighs[g]; ++h)
    {
        const size_t k = b.mGroupBase[g] + h;
        GpuHigh& gh = *b.mHighs[k];
        const bake::MeshSoup& soup = mIn->groups[g].highs[h].soup;
        if (!gh.positions.buffer && !soup.positions.empty())
        {
            up.add(gh.positions, soup.positions.data(), soup.positions.size() * sizeof(glm::vec3));
            b.mHighInfoHost[k].positions = gh.positions.address;
            changed = true;
        }
        if (!gh.uvs.buffer && !soup.uvs.empty())
        {
            up.add(gh.uvs, soup.uvs.data(), soup.uvs.size() * sizeof(glm::vec2));
            b.mHighInfoHost[k].uvs = gh.uvs.address;
            changed = true;
        }
    }
    if (!changed)
        return;
    up.flush();
    std::memcpy(b.mHighInfo.mapped, b.mHighInfoHost.data(), b.mHighInfoHost.size() * sizeof(GpuHighInfo));
    vmaFlushAllocation(mVk.allocator, b.mHighInfo.alloc, 0, VK_WHOLE_SIZE);
}

uint64_t GpuResident::normalMapTable(size_t g)
{
    if (auto it = mNormalMapTables.find(g); it != mNormalMapTables.end())
        return it->second;
    const bake::GroupInputs& gi = mIn->groups[g];
    std::vector<GpuTextureRef> refs;
    bool any = false;
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        const bake::MeshSoup& soup = gi.highs[h].soup;
        const GpuHigh& gh = *mB.mHighs[mB.mGroupBase[g] + h];
        const uint32_t materialBase = mB.mMaterialBase[mB.mGroupBase[g] + h];
        refs.resize(std::max(refs.size(), size_t(materialBase) + gh.slotTriangle.size() + 1));
        if (soup.uvs.empty())
            continue;
        for (size_t m = 0; m < soup.materialNames.size() && m < gh.slotTriangle.size(); ++m)
        {
            auto mat = mIn->materials.find(soup.materialNames[m]);
            if (mat == mIn->materials.end() || mat->second.normalMap.empty())
                continue;
            GpuTextureRef r = loadTexture(mat->second.normalMap);
            r.flags = mat->second.normalOpenGL ? 1u : 0u;
            refs[materialBase + m] = r;
            any = any || r.width;
        }
    }
    uint64_t address = 0;
    if (any)
    {
        ResidentUploads up{mB};
        needHighSurfaces(g, up);
        address = table(refs.data(), refs.size() * sizeof(GpuTextureRef));
    }
    mNormalMapTables[g] = address;
    return address;
}

uint64_t GpuResident::materialTable(size_t g, const std::string& map)
{
    const auto key = std::make_pair(g, map);
    if (auto it = mMaterialTables.find(key); it != mMaterialTables.end())
        return it->second;
    const bake::GroupInputs& gi = mIn->groups[g];
    static const bake::MaterialInput kNeutral;
    std::vector<GpuMaterialSlot> slots;
    bool textured = false;
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        const bake::MeshSoup& soup = gi.highs[h].soup;
        const GpuHigh& gh = *mB.mHighs[mB.mGroupBase[g] + h];
        const uint32_t materialBase = mB.mMaterialBase[mB.mGroupBase[g] + h];
        const size_t count = gh.slotTriangle.size();
        slots.resize(std::max(slots.size(), size_t(materialBase) + count + 1));
        for (size_t m = 0; m <= count; ++m)
        {
            GpuMaterialSlot& slot = slots[materialBase + m];
            const bool named = m < soup.materialNames.size() && m < count;
            auto it = named ? mIn->materials.find(soup.materialNames[m]) : mIn->materials.end();
            const bake::MaterialInput& mat = it != mIn->materials.end() ? it->second : kNeutral;
            slot.baseColor = glm::vec4(mat.baseColor, 1.0f);
            slot.emissive = glm::vec4(mat.emissive, float(mat.blendMode));
            slot.scalars = glm::vec4(mat.roughness, mat.metallic, mat.opacity, mat.emissiveIntensity);
            if (!named || soup.uvs.empty())
                continue;
            const bake::TransferSource ts = bake::transferSource(mat, map);
            GpuTextureRef ref = loadTexture(ts.texture);
            slot.channel = uint32_t(ts.channel);
            if (!ref.width && !ts.fallback.empty())
            {
                ref = loadTexture(ts.fallback);
                slot.channel = uint32_t(ts.fallbackChannel);
            }
            slot.texture = ref;
            textured = textured || ref.width;
        }
    }
    if (textured)
    {
        ResidentUploads up{mB};
        needHighSurfaces(g, up);
    }
    const uint64_t address = table(slots.data(), std::max<size_t>(1, slots.size()) * sizeof(GpuMaterialSlot));
    mMaterialTables[key] = address;
    return address;
}

uint64_t GpuResident::curvatureTable(size_t g)
{
    if (auto it = mCurvatureTables.find(g); it != mCurvatureTables.end())
        return it->second;
    GpuBackend& b = mB;
    const bake::GroupInputs& gi = mIn->groups[g];
    std::vector<GpuCornerTable> entries(std::max<size_t>(1, gi.highs.size()));
    std::vector<std::shared_ptr<const std::vector<float>>> corner(gi.highs.size());
    tbb::parallel_for(size_t(0), gi.highs.size(), [&](size_t h) { corner[h] = bake::highCurvature(gi.highs[h].soup); });
    ResidentUploads up{mB};
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        GpuHigh& gh = *b.mHighs[b.mGroupBase[g] + h];
        if (!gh.curvature.buffer && corner[h] && !corner[h]->empty())
            up.add(gh.curvature, corner[h]->data(), corner[h]->size() * sizeof(float));
    }
    up.flush();
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        const GpuHigh& gh = *b.mHighs[b.mGroupBase[g] + h];
        entries[h] = {gh.curvature.address, gh.curvature.buffer ? 1u : 0u, 0u};
    }
    const uint64_t address = table(entries.data(), entries.size() * sizeof(GpuCornerTable));
    mCurvatureTables[g] = address;
    return address;
}

uint64_t GpuResident::colorTable(size_t g, const std::string& set, bool first)
{
    const auto key = std::make_pair(g, (first ? std::string("\x01") : std::string("\x02")) + set);
    if (auto it = mColorTables.find(key); it != mColorTables.end())
        return it->second;
    GpuBackend& b = mB;
    const bake::GroupInputs& gi = mIn->groups[g];
    std::vector<GpuCornerTable> entries(std::max<size_t>(1, gi.highs.size()));
    ResidentUploads up{mB};
    std::vector<std::string> names(gi.highs.size());
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        const bake::MeshSoup& soup = gi.highs[h].soup;
        // colorSet in MapsMaterial.cpp: the named set (empty: the first); the ID map's vertex colours read the first.
        const std::vector<glm::vec4>* c = nullptr;
        if (first)
            c = soup.colors.empty() ? nullptr : &soup.colors;
        else
            for (size_t k = 0; k < soup.colorSetNames.size() && !c; ++k)
                if (set.empty() || soup.colorSetNames[k] == set)
                {
                    const std::vector<glm::vec4>& v = k ? soup.moreColors[k - 1] : soup.colors;
                    c = v.empty() ? nullptr : &v;
                    if (!c)
                        break;
                }
        if (!c)
            continue;
        names[h] = first ? std::string() : set;
        GpuHigh& gh = *b.mHighs[b.mGroupBase[g] + h];
        GpuBuffer& buf = gh.colors[(first ? std::string("\x01") : std::string("\x02")) + set];
        if (!buf.buffer)
            up.add(buf, c->data(), c->size() * sizeof(glm::vec4));
    }
    up.flush();
    for (size_t h = 0; h < gi.highs.size(); ++h)
    {
        GpuHigh& gh = *b.mHighs[b.mGroupBase[g] + h];
        auto it = gh.colors.find((first ? std::string("\x01") : std::string("\x02")) + set);
        if (it != gh.colors.end() && it->second.buffer)
            entries[h] = {it->second.address, 1u, 0u};
    }
    const uint64_t address = table(entries.data(), entries.size() * sizeof(GpuCornerTable));
    mColorTables[key] = address;
    return address;
}

void GpuResident::scope(Kernel& k, size_t g, const std::string& name, VkPipeline sw, VkPipeline hw)
{
    GpuBackend& b = mB;
    const bake::OccluderScope s = bake::occluderScope(name);
    k.pipe = b.mHardware ? hw : sw;
    k.layout = b.mHardware ? b.mLayoutHw : b.mLayoutSw;
    k.rate = &b.mRayRate;
    if (s == bake::OccluderScope::Self)
        k.push.flags |= kFlagScopeSelf;
    if (s == bake::OccluderScope::All)
    {
        k.push.highs = b.mHighInfo.address;
        k.push.highCount = uint32_t(b.mHighs.size());
    }
    if (!b.mHardware)
        return;
    if (s == bake::OccluderScope::All || !b.mGroupTlas[g].as)
    {
        b.runTask([&](VulkanContext& vk) {
            b.initDevice(vk);
            b.allTlas(vk);
            return true;
        });
        k.set = b.mAll.set;
        k.push.instanceBase = b.mAll.instanceBase.address;
        return;
    }
    k.set = b.mGroupTlas[g].set;
    k.push.instanceBase = b.mGroupTlas[g].instanceBase.address;
}

const GpuResident::Kernel& GpuResident::kernel(size_t m, size_t g)
{
    const auto key = std::make_pair(m, g);
    auto it = mKernels.find(key);
    if (it == mKernels.end())
        it = mKernels.emplace(key, makeKernel(m, g)).first;
    return it->second;
}

GpuResident::Kernel GpuResident::makeKernel(size_t m, size_t g)
{
    LILA_ZONE();

    GpuBackend& b = mB;
    const bake::ResidentMap& rm = mMaps[m];
    const std::string& id = rm.def->id;
    const bake::json& o = rm.settings.options;
    const bake::GroupInputs& gi = mIn->groups[g];
    const float diag = glm::length(gi.highMax - gi.highMin);
    const float eps = std::max(1e-4f * diag, 1e-4f);
    Kernel k;
    GpuBakePush& p = k.push;
    p.flags = kFlagResident;
    p.highs = b.mHighInfo.address + b.mGroupBase[g] * sizeof(GpuHighInfo);
    p.highList = b.mIota.address;
    p.highCount = uint32_t(b.mGroupHighs[g]);
    p.lows = mLowTables[g].address;
    p.boundsMin = glm::vec4(mIn->highMin, 0.0f);
    p.boundsExt = glm::vec4(mIn->highMax - mIn->highMin, 0.0f);
    p.width = uint32_t(mOut.width);
    p.height = uint32_t(mOut.height);
    k.pipe = b.mShade;
    k.layout = b.mLayoutSw;
    k.rate = &b.mShadeRate;
    auto normalMaps = [&] {
        if (!option(o, "highNormalMap", true))
            return;
        if (const uint64_t t = normalMapTable(g))
        {
            p.flags |= kFlagNormalMaps;
            p.normalMaps = t;
        }
    };
    auto frame = [&] {
        p.flags |= (option(o, "greenUp", false) ? kFlagGreenUp : 0u) | (option(o, "flipX", false) ? kFlagFlipX : 0u);
        if (option(o, "space", std::string("tangent")) == "object")
            p.flags |= kFlagObjectSpace;
    };
    auto distance = [&](const char* key) {
        const double d = option(o, key, 0.0);
        return d > 0.0 ? float(d) : std::max(diag, 1e-3f);
    };
    const std::string groupScope = option(o, "scope", gi.settings.aoScope);
    if (id == "normal" || id == "objectNormal")
    {
        p.map = id == "normal" ? kMapNormal : kMapObjectNormal;
        if (id == "normal")
            frame();
        normalMaps();
    }
    else if (id == "position")
        p.map = kMapPosition;
    else if (id == "height")
        p.map = kMapHeight;
    else if (id == "alpha")
        p.map = kMapAlpha;
    else if (id == "uvIslands")
    {
        p.map = kMapIslands;
        p.tableBase = b.mIslands[g].address;
        p.table = b.mIslands[g].address + gi.lows.size() * sizeof(uint32_t);
    }
    else if (id == "id")
    {
        if (option(o, "source", std::string("material")) == "vertexColor")
        {
            p.map = kMapIdVertexColor;
            p.extra = colorTable(g, std::string(), true);
        }
        else
        {
            p.map = kMapId;
            const bake::MapContext ctx{rm.settings, *mIn, gi, g, nullptr, mOut.width, mOut.height, nullptr};
            p.table = b.idTable(*rm.def, ctx).address;
        }
    }
    else if (id == "curvature" || id == "normalToCurvature" || id == "convexity" || id == "cavity" || id == "edgeWear" || id == "normalToHeight")
    {
        if (id != "normalToHeight" && curvatureFromMesh(rm.settings))
        {
            p.map = kMapCurvatureMesh;
            p.extra = curvatureTable(g);
        }
        else
        {
            p.map = kMapSlopes;
            normalMaps();
        }
    }
    else if (id == "vertexColor")
    {
        p.map = kMapVertexColor;
        p.extra = colorTable(g, option(o, "set", std::string()), false);
    }
    else if (id == "baseColor" || id == "roughness" || id == "metallic" || id == "emissive" || id == "materialOpacity")
    {
        p.map = kMapMaterial;
        p.mode = id == "baseColor" ? 0u : id == "roughness" ? 1u : id == "metallic" ? 2u : id == "emissive" ? 3u : 4u;
        p.extra = materialTable(g, id);
    }
    else if (id == "wireframe" || id == "islandBorder")
    {
        p.map = kMapEdges;
        p.mode = id == "wireframe" ? 0u : 1u;
        p.params.x = 0.5f * std::max(0.0f, option(o, "width", id == "wireframe" ? 1.0f : 2.0f));
    }
    else if (id == "gradient")
    {
        p.map = kMapGradient;
        glm::vec3 d(0.0f, 1.0f, 0.0f);
        const bake::json dir = option(o, "direction", bake::json());
        if (dir.is_array() && dir.size() >= 3)
            d = glm::vec3(dir[0].get<float>(), dir[1].get<float>(), dir[2].get<float>());
        const float dl = glm::length(d);
        d = dl > 1e-20f ? d / dl : glm::vec3(0.0f, 1.0f, 0.0f);
        const bool all = option(o, "bounds", std::string("group")) == "all";
        const glm::vec3 lo = all ? mIn->highMin : gi.highMin, hi = all ? mIn->highMax : gi.highMax;
        float mn = 1e30f, mx = -1e30f;
        for (int c = 0; c < 8; ++c)
        {
            const glm::vec3 q((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
            mn = std::min(mn, glm::dot(q, d));
            mx = std::max(mx, glm::dot(q, d));
        }
        p.params = glm::vec4(d, 0.0f);
        p.params2 = glm::vec4(mn, mx - mn, 0.0f, 0.0f);
    }
    else if (id == "ao")
    {
        p.rays = uint32_t(std::clamp(option(o, "rays", 64), 1, 4096));
        p.boundsMin.w = distance("distance");
        p.boundsExt.w = eps;
        scope(k, g, groupScope, b.mAoSw, b.mAoHw);
        k.cost = double(p.rays);
    }
    else if (id == "thickness")
    {
        p.rays = uint32_t(std::clamp(option(o, "rays", 64), 1, 4096));
        const float half = glm::radians(std::clamp(option(o, "spread", 180.0f), 0.0f, 180.0f)) * 0.5f;
        p.params = glm::vec4(distance("distance"), eps, half, 0.0f);
        scope(k, g, groupScope, b.mThicknessSw, b.mThicknessHw);
        k.cost = double(p.rays) * 1.5;
    }
    else if (id == "bentNormal")
    {
        p.rays = uint32_t(std::clamp(option(o, "rays", 64), 1, 4096));
        p.params = glm::vec4(distance("distance"), eps, 0.0f, 0.0f);
        frame();
        scope(k, g, groupScope, b.mBentSw, b.mBentHw);
        k.cost = double(p.rays);
    }
    else if (id == "roundedNormal")
    {
        p.rays = uint32_t(std::clamp(option(o, "rays", 32), 1, 4096));
        const double r = option(o, "radius", 0.0);
        p.params.x = r > 0.0 ? float(r) : std::max(0.01f * diag, 1e-3f);
        frame();
        scope(k, g, option(o, "scope", std::string("group")), b.mRoundedSw, b.mRoundedHw);
        k.cost = double(p.rays) * 3.0;
    }
    else if (id == "error")
    {
        const double beyond = option(o, "beyond", 0.0);
        p.params.x = beyond > 0.0 ? float(beyond) : std::max(2.0f * diag, 1e-3f);
        scope(k, g, "group", b.mErrorSw, b.mErrorHw);
        k.cost = 4.0;
    }
    return k;
}

void GpuResident::reserveBand(size_t slots, size_t items)
{
    if (slots > mSlotCap)
    {
        for (GpuBuffer* b : {&mClaim, &mSamples, &mHits, &mOutBuf, &mValid})
            destroy(mVk, *b);
        mClaim = makeBuffer(mVk, slots * sizeof(uint32_t), 0, Host::None);
        mSamples = makeBuffer(mVk, slots * sizeof(TexelSample), 0, Host::None);
        mHits = makeBuffer(mVk, slots * sizeof(HitRecord), 0, Host::None);
        mOutBuf = makeBuffer(mVk, slots * sizeof(glm::vec4), 0, Host::None);
        mValid = makeBuffer(mVk, slots * sizeof(uint32_t), 0, Host::None);
        mSlotCap = slots;
    }
    if (items > mItemCap || !mItems.buffer)
    {
        destroy(mVk, mItems);
        mItemCap = std::max<size_t>(items, 4096);
        mItems = makeBuffer(mVk, mItemCap * 3 * sizeof(uint32_t), 0, Host::Write);
    }
    if (!mStats.buffer)
    {
        mStats = makeBuffer(mVk, 16, 0, Host::None);
        mStatsRead = makeBuffer(mVk, 16, 0, Host::Read);
    }
}

bake::ResidentStats GpuResident::bakeTile(const bake::ResidentTile& tile)
{
    LILA_ZONE();

    GpuBackend& b = mB;
    const size_t g = tile.group;
    const bake::TexelRaster& raster = *tile.raster;
    const int W = raster.settings().width, H = raster.settings().height, rows = raster.settings().bandRows;
    const uint32_t grid = uint32_t(raster.gridSize()), S = grid * grid;
    const bake::LowInput& lowIn = mIn->groups[g].lows[tile.low];
    const auto setupStarted = std::chrono::steady_clock::now();
    low(g, tile.low);
    const uint64_t lowInfo = mLowTables[g].address + tile.low * sizeof(GpuLowInfo);

    // Per map: its kernel and where each low material slot's samples accumulate.
    struct Job
    {
        const Kernel* kernel;
        uint64_t targets;
        std::string label;
    };
    std::vector<Job> jobs;
    for (size_t m = 0; m < mMaps.size() && m < tile.targets.size(); ++m)
    {
        const std::vector<bake::TextureBuffer*>& t = tile.targets[m];
        if (std::ranges::none_of(t, [](const bake::TextureBuffer* x) { return x != nullptr; }))
            continue;
        uint32_t slots = uint32_t(t.size());
        for (uint32_t mat : lowIn.soup.materials)
            slots = std::max(slots, mat + 1);
        std::vector<GpuAccTarget> targets(slots);
        for (size_t mat = 0; mat < t.size(); ++mat)
            if (t[mat])
            {
                DevTex& d = texture(*t[mat]);
                const bake::Resolve r = t[mat]->def ? t[mat]->def->resolve : bake::Resolve::Average;
                targets[mat] = {d.value.address, d.weight.address, d.count.address, d.exact.address, d.votes.address,
                                r == bake::Resolve::Exact ? 1u : r == bake::Resolve::Coverage ? 2u : 0u, d.id};
            }
        jobs.push_back({&kernel(m, g), table(targets.data(), targets.size() * sizeof(GpuAccTarget)), "map:" + mMaps[m].def->id});
    }

    mSetupSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - setupStarted).count();
    const int udim = tile.udim;
    const int tileU = (udim - 1001) % 10, tileV = (udim - 1001) / 10;
    const bool cullBack = mIn->groups[g].settings.ignoreBackfaces;
    const bool traceable = b.mGroupHighs[g] > 0 && (!b.mHardware || b.mGroupTlas[g].as);
    bake::ResidentStats stats;
    std::vector<uint32_t> items;
    // Texel tiles: which the mask lets through, and which had work.
    const int maskTile = std::max(8, tile.maskTile / 8 * 8);
    const int tileCols = (W + maskTile - 1) / maskTile, tileRows = (H + maskTile - 1) / maskTile;
    const std::vector<uint8_t>* mask = tile.mask && tile.mask->size() == size_t(tileCols) * size_t(tileRows) ? tile.mask : nullptr;
    std::vector<uint8_t> worked(size_t(tileCols) * size_t(tileRows), 0);
    for (size_t band = 0; band < raster.bandCount(); ++band)
    {
        const int y0 = int(band) * rows, y1 = std::min(H, y0 + rows);
        const std::vector<uint32_t>& tris = raster.bandTriangles(udim, band);
        if (tris.empty() || jobs.empty())
        {
            if (tile.onBand)
                tile.onBand(y1 - y0);
            continue;
        }
        // 8 x 8 texel blocks of each triangle's bounds in the band, as TexelRaster::band walks them.
        items.clear();
        for (uint32_t t : tris)
        {
            const glm::vec2* uv = &lowIn.soup.uvs[size_t(t) * 3];
            glm::dvec2 q[3];
            for (int k = 0; k < 3; ++k)
                q[k] = {(double(uv[k].x) - tileU) * W, (1.0 - (double(uv[k].y) - tileV)) * H};
            const double area = (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[1].y - q[0].y) * (q[2].x - q[0].x);
            if (std::abs(area) < 1e-12)
                continue;
            const int xa = std::clamp(int(std::floor(std::min({q[0].x, q[1].x, q[2].x}))), 0, W);
            const int xb = std::clamp(int(std::ceil(std::max({q[0].x, q[1].x, q[2].x}))), 0, W);
            const int ya = std::clamp(int(std::floor(std::min({q[0].y, q[1].y, q[2].y}))), y0, y1);
            const int yb = std::clamp(int(std::ceil(std::max({q[0].y, q[1].y, q[2].y}))), y0, y1);
            if (xa >= xb || ya >= yb)
                continue;
            for (int by = ya / 8; by <= (yb - 1) / 8; ++by)
                for (int bx = xa / 8; bx <= (xb - 1) / 8; ++bx)
                {
                    const size_t mt = size_t(std::min(by * 8 / maskTile, tileRows - 1)) * size_t(tileCols) + size_t(bx * 8 / maskTile);
                    if (mask && !(*mask)[mt])
                        continue;
                    worked[mt] = 1;
                    items.insert(items.end(), {t, uint32_t(bx), uint32_t(by)});
                }
        }
        if (items.empty())
        {
            if (tile.onBand)
                tile.onBand(y1 - y0);
            continue;
        }
        const size_t texels = size_t(W) * size_t(y1 - y0);
        const size_t slots = texels * S;
        const size_t itemCount = items.size() / 3;
        reserveBand(slots, itemCount);
        std::memcpy(mItems.mapped, items.data(), items.size() * sizeof(uint32_t));
        vmaFlushAllocation(mVk.allocator, mItems.alloc, 0, VK_WHOLE_SIZE);

        GpuRasterPush rp;
        rp.low = lowInfo;
        rp.items = mItems.address;
        rp.claim = mClaim.address;
        rp.out = mSamples.address;
        rp.width = uint32_t(W);
        rp.height = uint32_t(H);
        rp.grid = grid;
        rp.y0 = uint32_t(y0);
        rp.rows = uint32_t(y1 - y0);
        rp.tileU = uint32_t(tileU);
        rp.tileV = uint32_t(tileV);
        rp.udim = uint32_t(udim);
        rp.lowIndex = uint32_t(tile.low);
        rp.explicitCage = lowIn.cagePositions.size() == lowIn.soup.positions.size() ? 1u : 0u;
        rp.frontal = tile.cage.frontal;
        rp.rear = tile.cage.rear;
        rp.offset = tile.cage.offset;
        rp.skew = tile.cage.skew;

        std::vector<GpuStep> steps;
        steps.push_back({1, 0.0, nullptr, [&](VkCommandBuffer cmd, uint32_t, uint32_t) {
                             vkCmdFillBuffer(cmd, mClaim.buffer, 0, slots * sizeof(uint32_t), 0xffffffffu);
                             vkCmdFillBuffer(cmd, mStats.buffer, 0, 16, 0);
                         },
                         "raster"});
        steps.push_back({itemCount, double(S) * 64.0, &b.mRasterRate, [&](VkCommandBuffer cmd, uint32_t first, uint32_t n) {
                             vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mClaim);
                             dispatchChunks(cmd, b.mLayoutRaster, rp, first, n, 1, [](GpuRasterPush& q, uint32_t f, uint32_t c) {
                                 q.first = f;
                                 q.count = f + c;
                             });
                         },
                         "raster"});
        steps.push_back({slots, 4.0, &b.mRasterRate, [&](VkCommandBuffer cmd, uint32_t first, uint32_t n) {
                             vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mSamplesPipe);
                             dispatchChunks(cmd, b.mLayoutRaster, rp, first, n, 64, [](GpuRasterPush& q, uint32_t f, uint32_t c) {
                                 q.first = f;
                                 q.count = f + c;
                             });
                         },
                         "raster"});

        GpuBakePush pp;
        pp.samples = mSamples.address;
        pp.hits = mHits.address;
        pp.highs = b.mHighInfo.address + b.mGroupBase[g] * sizeof(GpuHighInfo);
        pp.highList = b.mIota.address;
        pp.highCount = traceable ? uint32_t(b.mGroupHighs[g]) : 0u;
        pp.flags = kFlagResident | (cullBack ? kFlagCullBack : 0u);
        pp.stats = mStats.address;
        const bool hw = b.mHardware && traceable;
        if (hw)
            pp.instanceBase = b.mGroupTlas[g].instanceBase.address;
        steps.push_back({slots, 1.0, &b.mProjectRate, [&](VkCommandBuffer cmd, uint32_t first, uint32_t n) {
                             const VkPipelineLayout layout = hw ? b.mLayoutHw : b.mLayoutSw;
                             vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hw ? b.mProjectHw : b.mProjectSw);
                             if (hw)
                                 vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &b.mGroupTlas[g].set, 0, nullptr);
                             dispatchChunks(cmd, layout, pp, first, n, 64, [](GpuBakePush& q, uint32_t f, uint32_t c) {
                                 q.first = f;
                                 q.count = c;
                             });
                         },
                         "trace"});

        GpuImagePush ap;
        ap.samples = mSamples.address;
        ap.out = mOutBuf.address;
        ap.valid = mValid.address;
        ap.low = lowInfo;
        ap.width = uint32_t(W);
        ap.height = uint32_t(H);
        ap.grid = grid;
        ap.y0 = uint32_t(y0);
        ap.rows = uint32_t(y1 - y0);
        std::vector<GpuBakePush> kernelPush(jobs.size());
        std::vector<GpuImagePush> accPush(jobs.size(), ap);
        for (size_t j = 0; j < jobs.size(); ++j)
        {
            const Kernel& k = *jobs[j].kernel;
            GpuBakePush& kp = kernelPush[j];
            kp = k.push;
            kp.samples = mSamples.address;
            kp.hits = mHits.address;
            kp.outFloat = mOutBuf.address;
            kp.valid = mValid.address;
            accPush[j].targets = jobs[j].targets;
            steps.push_back({slots, k.cost, k.rate, [&, j](VkCommandBuffer cmd, uint32_t first, uint32_t n) {
                                 const Kernel& kk = *jobs[j].kernel;
                                 vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kk.pipe);
                                 if (kk.set)
                                     vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kk.layout, 0, 1, &kk.set, 0, nullptr);
                                 dispatchChunks(cmd, kk.layout, kernelPush[j], first, n, 64, [](GpuBakePush& q, uint32_t f, uint32_t c) {
                                     q.first = f;
                                     q.count = c;
                                 });
                             },
                             jobs[j].label});
            steps.push_back({texels, double(S), &b.mImageRate, [&, j](VkCommandBuffer cmd, uint32_t first, uint32_t n) {
                                 vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mImagePipes.at("accumulateCS"));
                                 dispatchChunks(cmd, b.mLayoutImage, accPush[j], first, n, 64, [](GpuImagePush& q, uint32_t f, uint32_t c) {
                                     q.first = f;
                                     q.total = f + c;
                                 });
                             },
                             "accumulate"});
        }
        steps.push_back({1, 0.0, nullptr, [&](VkCommandBuffer cmd, uint32_t, uint32_t) { copy(cmd, mStats, mStatsRead, 16); }});
        b.runSteps(steps, tile.cancelled);
        vmaInvalidateAllocation(mVk.allocator, mStatsRead.alloc, 0, VK_WHOLE_SIZE);
        const uint32_t* counts = static_cast<const uint32_t*>(mStatsRead.mapped);
        stats.samples += counts[0];
        stats.hits += counts[1];
        stats.backfaces += counts[2];
        if (tile.onBand)
            tile.onBand(y1 - y0);
    }
    stats.tiles = uint64_t(std::ranges::count(worked, uint8_t(1)));
    return stats;
}

bool GpuResident::finish(bake::TextureBuffer& tex, int padding) { return finishImpl(tex, padding, nullptr); }

void GpuResident::finishInto(bake::TextureBuffer& tex, int padding, const GpuImageRef& image)
{
    if (!image || image->width() != tex.width || image->height() != tex.height)
        throw std::invalid_argument("GPU bake: the image does not match the texture");
    if (finishOf(tex.def->id) == Finish::Cpu)
        throw std::runtime_error("GPU bake: " + tex.def->id + " finalizes on the CPU and cannot finish into an image");
    finishImpl(tex, padding, &image);
}

bool GpuResident::finishImpl(bake::TextureBuffer& tex, int padding, const GpuImageRef* into)
{
    LILA_ZONE();

    GpuBackend& b = mB;
    DevTex& d = texture(tex);
    const size_t n = tex.texelCount();
    const bake::MapDef& def = *tex.def;
    const bake::MapSettings& ms = tex.settings;
    const Finish kind = finishOf(def.id);
    if (into && n > mWorkCap)
    {
        for (GpuBuffer* buf : {&mWorkValue, &mWorkCount, &mPack})
            destroy(mVk, *buf);
        mWorkValue = makeBuffer(mVk, n * sizeof(glm::vec4), 0, Host::None);
        mWorkCount = makeBuffer(mVk, n * sizeof(uint32_t), 0, Host::None);
        mPack = makeBuffer(mVk, n * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Host::None);
        mWorkCap = n;
    }
    if (n > mTexelCap)
    {
        for (GpuBuffer* buf : {&mSeeds[0], &mSeeds[1], &mScratch, &mReadback})
            destroy(mVk, *buf);
        mSeeds[0] = makeBuffer(mVk, n * 4, 0, Host::None);
        mSeeds[1] = makeBuffer(mVk, n * 4, 0, Host::None);
        mScratch = makeBuffer(mVk, n * 4, 0, Host::None);
        mReadback = makeBuffer(mVk, n * (sizeof(glm::vec4) + sizeof(uint32_t)), 0, Host::Read);
        mTexelCap = n;
    }
    if (!mReduce.buffer)
    {
        mReduce = makeBuffer(mVk, 256 * 4, 0, Host::None);
        mReduceRead = makeBuffer(mVk, 256 * 4, 0, Host::Read);
    }

    GpuImagePush base;
    // Into an image, the passes work on a copy so the accumulators stay for the next incremental update.
    base.value = into ? mWorkValue.address : d.value.address;
    base.weight = d.weight.address;
    base.count = into ? mWorkCount.address : d.count.address;
    base.exact = d.exact.address;
    base.votes = d.votes.address;
    base.scratch = mScratch.address;
    base.reduce = mReduce.address;
    base.width = uint32_t(tex.width);
    base.height = uint32_t(tex.height);
    base.grid = uint32_t(std::max(1, int(std::sqrt(double(std::max(1, tex.samplesPerTexel))) + 1e-6)));
    base.background = def.background;
    base.mode = def.resolve == bake::Resolve::Exact ? 1u : def.resolve == bake::Resolve::Coverage ? 2u : 0u;

    std::vector<GpuStep> steps;
    // A per-texel pass over the whole texture.
    auto pass = [&](const char* entry, GpuImagePush push) {
        const std::string e = entry;
        const char* label = e == "resolveCS" ? "resolve" : e.starts_with("jfa") || e == "dilateApplyCS" ? "dilate" : "finalize";
        steps.push_back({n, 1.0, &b.mImageRate, [&b, entry, push](VkCommandBuffer cmd, uint32_t first, uint32_t count) {
                             vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mImagePipes.at(entry));
                             dispatchChunks(cmd, b.mLayoutImage, push, first, count, 64, [](GpuImagePush& q, uint32_t f, uint32_t c) {
                                 q.first = f;
                                 q.total = f + c;
                             });
                         },
                         label});
    };
    auto run = [&] {
        b.runSteps(steps);
        steps.clear();
    };
    auto clearReduce = [&] { steps.push_back({1, 0.0, nullptr, [this](VkCommandBuffer cmd, uint32_t, uint32_t) { vkCmdFillBuffer(cmd, mReduce.buffer, 0, VK_WHOLE_SIZE, 0); }}); };
    auto readReduce = [&] {
        steps.push_back({1, 0.0, nullptr, [this](VkCommandBuffer cmd, uint32_t, uint32_t) { copy(cmd, mReduce, mReduceRead, 256 * 4); }});
        run();
        vmaInvalidateAllocation(mVk.allocator, mReduceRead.alloc, 0, VK_WHOLE_SIZE);
        return static_cast<const uint32_t*>(mReduceRead.mapped);
    };
    // The largest |r| (mode 0) or r (mode 1) over covered texels; 0 when none.
    auto largest = [&](uint32_t mode) {
        clearReduce();
        GpuImagePush q = base;
        q.mode = mode;
        pass("maxCS", q);
        return std::bit_cast<float>(readReduce()[0]);
    };

    if (into)
        steps.push_back({1, 0.0, nullptr, [&](VkCommandBuffer cmd, uint32_t, uint32_t) {
                             copy(cmd, d.value, mWorkValue, n * sizeof(glm::vec4));
                             copy(cmd, d.count, mWorkCount, n * sizeof(uint32_t));
                         },
                         "resolve"});
    pass("resolveCS", base);
    bool deviceFinalized = kind != Finish::Cpu;
    switch (kind)
    {
    case Finish::None:
    case Finish::Cpu: break;
    case Finish::Renormalize: pass("renormalizeCS", base); break;
    case Finish::Height:
    {
        double range = option(ms.options, "range", 0.0);
        const bool automatic = !(range > 0.0);
        if (automatic)
        {
            range = largest(0);
            if (range <= 0.0)
                range = 1.0;
        }
        GpuImagePush q = base;
        q.params.x = float(range);
        pass("heightCS", q);
        tex.meta["heightRange"] = range;
        tex.meta["heightMin"] = -range;
        tex.meta["heightMax"] = range;
        tex.meta["heightAuto"] = automatic;
        tex.meta["decode"] = "height = (value - 0.5) * 2 * heightRange";
        break;
    }
    case Finish::Thickness:
    {
        double range = option(ms.options, "distance", 0.0);
        const bool automatic = !(range > 0.0);
        if (automatic)
        {
            range = largest(1);
            if (!(range > 0.0))
                range = 1.0;
        }
        const bool invert = option(ms.options, "invert", false);
        GpuImagePush q = base;
        q.params.x = float(range);
        q.mode = invert ? 1u : 0u;
        pass("thicknessApplyCS", q);
        tex.meta["thicknessRange"] = range;
        tex.meta["thicknessAuto"] = automatic;
        tex.meta["decode"] = invert ? "thickness = (1 - value) * thicknessRange (cm)" : "thickness = value * thicknessRange (cm)";
        break;
    }
    case Finish::Curvature:
    {
        const bool mesh = curvatureFromMesh(ms);
        if (mesh)
            pass("copyRedCS", base);
        else
        {
            GpuImagePush q = base;
            q.mode = uint32_t(std::max(1, option(ms.options, "radius", 1)));
            pass("slopeCurvatureCS", q);
        }
        double range = option(ms.options, "range", 0.0);
        const bool automatic = !(range > 0.0);
        if (automatic)
        {
            // percentileAbs(0.99) by radix select over the float bits of |h|, a byte per pass from the top.
            uint32_t prefix = 0;
            size_t k = 0, covered = 0;
            for (int p = 0; p < 4; ++p)
            {
                clearReduce();
                GpuImagePush q = base;
                q.shift = uint32_t(24 - 8 * p);
                q.prefix = prefix;
                steps.push_back({n, 1.0, &b.mImageRate, [&b, q](VkCommandBuffer cmd, uint32_t first, uint32_t count) {
                                     vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.mImagePipes.at("histogramCS"));
                                     dispatchChunks(cmd, b.mLayoutImage, q, first, count, 256 * 16, [](GpuImagePush& r, uint32_t f, uint32_t c) {
                                         r.first = f;
                                         r.total = f + c;
                                     });
                                 },
                                 "finalize"});
                const uint32_t* hist = readReduce();
                if (p == 0)
                {
                    for (int i = 0; i < 256; ++i)
                        covered += hist[i];
                    if (!covered)
                        break;
                    k = std::min(covered - 1, size_t(0.99f * float(covered - 1) + 0.5f));
                }
                size_t below = 0;
                int bin = 255;
                for (int i = 0; i < 256; ++i)
                {
                    if (k < below + hist[i])
                    {
                        bin = i;
                        break;
                    }
                    below += hist[i];
                }
                k -= below;
                prefix = (prefix << 8) | uint32_t(bin);
            }
            range = covered ? double(std::bit_cast<float>(prefix)) : 0.0;
            if (!(range > 0.0))
                range = 1.0;
        }
        const std::string id = def.id;
        GpuImagePush q = base;
        q.mode = id == "convexity" ? 1u : id == "cavity" ? 2u : id == "edgeWear" ? 3u : 0u;
        q.params = glm::vec4(float(range), option(ms.options, "threshold", 0.1f), option(ms.options, "softness", 0.4f),
                             std::clamp(option(ms.options, "noise", 0.0f), 0.0f, 1.0f));
        q.params2.x = std::max(1.0f, option(ms.options, "noiseScale", 8.0f));
        pass("curvatureCS", q);
        tex.meta["curvatureRange"] = range;
        tex.meta["curvatureAuto"] = automatic;
        tex.meta["source"] = mesh ? "mesh" : "normals";
        if (q.mode == 0)
            tex.meta["decode"] = "curvature = (value - 0.5) * 2 * curvatureRange (1/cm mean curvature, convex positive)";
        break;
    }
    }

    if (deviceFinalized && def.dilate && padding != 0)
    {
        // Steps k, k/2 .. 1 reach 2k - 1 texels; two more at 2 and 1 mend jump flood's rare misses.
        const int extent = std::max(tex.width, tex.height);
        const int reach = padding < 0 ? extent : std::min(padding, extent);
        std::vector<int> jumps;
        for (int k = int(std::bit_ceil(unsigned((reach + 1) / 2 + 1))); k >= 1; k /= 2)
            jumps.push_back(k);
        jumps.push_back(2);
        jumps.push_back(1);
        GpuImagePush q = base;
        q.seedsOut = mSeeds[0].address;
        pass("jfaInitCS", q);
        int cur = 0;
        for (int step : jumps)
        {
            q.seedsIn = mSeeds[cur].address;
            q.seedsOut = mSeeds[1 - cur].address;
            q.step = step;
            pass("jfaStepCS", q);
            cur = 1 - cur;
        }
        q.seedsIn = mSeeds[cur].address;
        q.padding = padding;
        pass("dilateApplyCS", q);
    }
    if (into)
    {
        writeImage(steps, base, **into);
        run();
        for (GpuBuffer& buf : mStaging)
            destroy(mVk, buf);
        mStaging.clear();
        return true;
    }
    steps.push_back({1, 0.0, nullptr, [&](VkCommandBuffer cmd, uint32_t, uint32_t) {
                         VkBufferCopy regions[2] = {{0, 0, n * sizeof(glm::vec4)}, {0, n * sizeof(glm::vec4), n * sizeof(uint32_t)}};
                         vkCmdCopyBuffer(cmd, d.value.buffer, mReadback.buffer, 1, &regions[0]);
                         vkCmdCopyBuffer(cmd, d.count.buffer, mReadback.buffer, 1, &regions[1]);
                     },
                     "readback"});
    run();
    vmaInvalidateAllocation(mVk.allocator, mReadback.alloc, 0, VK_WHOLE_SIZE);
    tex.value.resize(n);
    tex.covered.resize(n);
    const auto* values = static_cast<const glm::vec4*>(mReadback.mapped);
    const auto* covered = reinterpret_cast<const uint32_t*>(static_cast<const char*>(mReadback.mapped) + n * sizeof(glm::vec4));
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 1 << 16), [&](const tbb::blocked_range<size_t>& r) {
        std::memcpy(tex.value.data() + r.begin(), values + r.begin(), (r.end() - r.begin()) * sizeof(glm::vec4));
        for (size_t i = r.begin(); i < r.end(); ++i)
            tex.covered[i] = covered[i] ? 1 : 0;
    });
    for (GpuBuffer* buf : {&d.value, &d.weight, &d.count, &d.exact, &d.votes})
        destroy(mVk, *buf);
    mTex.erase(&tex);
    if (kind == Finish::None && def.finalize)
        def.finalize(ms, *mIn, tex, tex.meta);
    return deviceFinalized;
}

std::unique_ptr<bake::ResidentBake> GpuBackend::beginResident(const bake::BakeInputs& inputs, const std::vector<bake::ResidentMap>& maps,
                                                              const bake::OutputSettings& output)
{
    if (mFailed || mInputs != &inputs || maps.empty())
        return nullptr;
    try
    {
        runTask([this](VulkanContext& vk) {
            initDevice(vk);
            return true;
        });
    }
    catch (const std::exception& e)
    {
        fail(e.what());
        return nullptr;
    }
    {
        std::scoped_lock lock(mGpuTimeMutex);
        mGpuTimes.clear();
    }
    return std::make_unique<GpuResident>(*this, inputs, maps, output);
}
} // namespace

std::unique_ptr<DeviceBake> beginDeviceBake(bake::Backend& backend, const bake::BakeInputs& inputs, std::vector<bake::ResidentMap> maps,
                                            const bake::OutputSettings& output, double sliceMs)
{
    auto* gpu = dynamic_cast<GpuBackend*>(&backend);
    if (!gpu || maps.empty())
        return nullptr;
    gpu->setSliceMs(std::clamp(sliceMs, 2.0, kSliceMs));
    std::unique_ptr<bake::ResidentBake> r = gpu->beginResident(inputs, maps, output);
    if (!r)
        return nullptr;
    return std::unique_ptr<DeviceBake>(static_cast<GpuResident*>(r.release()));
}

std::unique_ptr<bake::Backend> createGpuBakeBackend(GpuTrace trace)
{
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
        throw std::runtime_error("GPU bake: no Vulkan 1.3 device (the renderer could not start)");
    VulkanContext* vk = r.context();
    if (!vk)
        throw std::runtime_error("GPU bake: the renderer is not running");
    if (trace == GpuTrace::Hardware && !vk->rayQuery)
        throw std::runtime_error("GPU bake: " + vk->deviceName + " has no ray query");
    const char* env = std::getenv("LILA_BAKE_GPU_TRACE");
    const bool forceSw = env && std::string_view(env) == "sw";
    const bool hw = trace == GpuTrace::Hardware || (trace == GpuTrace::Auto && vk->rayQuery && !forceSw);
    static std::once_flag hooks;
    std::call_once(hooks, [&r] {
        r.addStopHook([](VulkanContext& ctx) { highCache().trim(ctx, 0); });
        bake::onClearBakeCaches([] {
            Renderer& renderer = Renderer::get();
            if (renderer.running())
                renderer
                    .runTask([](VulkanContext& ctx) {
                        highCache().trim(ctx, 0);
                        return true;
                    })
                    .get();
        });
    });
    return std::make_unique<GpuBackend>(r, hw);
}

void registerGpuBakeBackend()
{
    bake::registerBackend("gpu", [] { return createGpuBakeBackend(GpuTrace::Auto); });
    static std::once_flag commands;
    std::call_once(commands, [] { registerLiveMapCommands(); });
}

std::string gpuBakeMode(const bake::Backend& backend)
{
    const auto* g = dynamic_cast<const GpuBackend*>(&backend);
    return g ? (g->hardware() ? "rt" : "compute") : std::string();
}
} // namespace lila::render
