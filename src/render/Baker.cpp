#include "render/Baker.h"
#include "render/BakerInternal.h"

namespace lila::render
{
namespace detail
{
namespace
{
std::vector<uint32_t> loadSpirv(const char* name)
{
    std::string err;
    std::vector<uint32_t> code = compute::loadSpirv(name, &err);
    if (code.empty())
        throw std::runtime_error("GPU bake: " + err);
    return code;
}

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
} // namespace

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
    for (GpuBuffer& b : mLinks)
        destroy(vk, b);
    mLinks.clear();
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
            // The build is parallel; isolated, this thread never runs another band that would wait on this lock.
            tbb::this_task_arena::isolate([&] { c->prepare(*mInputs); });
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

void GpuBackend::useLinks(GpuBakePush& p, size_t g, bool all) const
{
    if (g >= mLinks.size() || !mLinks[g].address)
        return;
    p.flags |= kFlagLinks;
    p.links = mLinks[g].address;
    p.linkWords = mInputs->groups[g].linkWords;
    p.highBase = all ? 0u : uint32_t(mGroupBase[g]);
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
        for (const bake::GroupInputs& gi : in.groups)
            mLinks.push_back(gi.links.empty() ? GpuBuffer{} : hostBuffer(vk, gi.links.data(), gi.links.size() * sizeof(uint32_t)));
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
} // namespace detail

std::unique_ptr<DeviceBake> beginDeviceBake(bake::Backend& backend, const bake::BakeInputs& inputs, std::vector<bake::ResidentMap> maps,
                                            const bake::OutputSettings& output, double sliceMs)
{
    auto* gpu = dynamic_cast<detail::GpuBackend*>(&backend);
    if (!gpu || maps.empty())
        return nullptr;
    gpu->setSliceMs(std::clamp(sliceMs, 2.0, detail::kSliceMs));
    std::unique_ptr<bake::ResidentBake> r = gpu->beginResident(inputs, maps, output);
    if (!r)
        return nullptr;
    return std::unique_ptr<DeviceBake>(static_cast<detail::GpuResident*>(r.release()));
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
        r.addStopHook([](VulkanContext& ctx) { detail::highCache().trim(ctx, 0); });
        bake::onClearBakeCaches([] {
            Renderer& renderer = Renderer::get();
            if (renderer.running())
                renderer
                    .runTask([](VulkanContext& ctx) {
                        detail::highCache().trim(ctx, 0);
                        return true;
                    })
                    .get();
        });
    });
    return std::make_unique<detail::GpuBackend>(r, hw);
}

void registerGpuBakeBackend()
{
    bake::registerBackend("gpu", [] { return createGpuBakeBackend(GpuTrace::Auto); });
    static std::once_flag commands;
    std::call_once(commands, [] { registerLiveMapCommands(); });
}

std::string gpuBakeMode(const bake::Backend& backend)
{
    const auto* g = dynamic_cast<const detail::GpuBackend*>(&backend);
    return g ? (g->hardware() ? "rt" : "compute") : std::string();
}
} // namespace lila::render
