#pragma once

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
#include <tbb/task_arena.h>

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

namespace lila::render::detail
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
    uint64_t blocks = 0;
};
static_assert(sizeof(GpuRasterPush) == 104, "matches RasterPush in bake_raster.slang");

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
    uint64_t blocks = 0;
};
static_assert(offsetof(GpuImagePush, background) == 112 && sizeof(GpuImagePush) == 216, "matches ImagePush in bake_image.slang");

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

inline std::atomic<uint64_t> gNextBackendId{1};

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
    bool closestHit(size_t group, bake::OccluderScope scope, uint32_t high, const glm::vec3& origin, const glm::vec3& dir, float tMax,
                    bake::TraceHit& hit) const override
    {
        return cpu().closestHit(group, scope, high, origin, dir, tMax, hit);
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
        // Accumulators and votes per texel, the band buffers (bandRows' share until they exist), and room for one
        // texture's dilation and read back.
        const uint64_t band = mSlotCap ? mSlotCap * kSlotBytes : std::clamp<uint64_t>(free / 4, 64ull << 20, 1536ull << 20);
        const uint64_t need = uint64_t(texels) * 32 + band + (256ull << 20);
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
    void reserveBand(size_t slots, size_t items, size_t blocks);
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
    /**
     * Accumulators of finished textures, kept for the next ones of the same size: allocating and freeing them costs more
     * than clearing. A texture that matches none frees one instead, so the spares never add to what a batch planned.
     */
    std::vector<DevTex> mSpareTex;
    uint32_t mNextTexId = 1;
    std::map<std::string, GpuTextureRef> mTextures;
    std::map<size_t, uint64_t> mNormalMapTables, mCurvatureTables;
    std::map<std::pair<size_t, std::string>, uint64_t> mMaterialTables, mColorTables;
    /** Buffers made for this bake: tables, textures. */
    std::vector<GpuBuffer> mOwned;

    // Band buffers, grown as needed.
    GpuBuffer mClaim, mSamples, mHits, mOutBuf, mValid, mItems, mBlocks, mStats, mStatsRead;
    size_t mSlotCap = 0, mItemCap = 0, mBlockCap = 0;
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
} // namespace lila::render::detail
