#include "render/Baker.h"
#include "render/BakerInternal.h"

namespace lila::render::detail
{
namespace
{
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
    bool thickness = false;
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
    if (def.id == "thickness" && onlyOptions(o, {"rays", "spread", "distance", "invert", "scope"}))
        return {.thickness = true};
    return {};
}

/** The slot each worker thread holds for its current band, per backend instance. */
thread_local std::unordered_map<uint64_t, BandSlot*> tHeld;
} // namespace

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
    // The band kernels don't read the high's normal maps; prepared is non-null when there are some to apply.
    const bool highNormalMaps = (k.map == kMapNormal || k.map == kMapObjectNormal) && ctx.prepared;
    if ((k.map < 0 && !k.ao && !k.thickness) || highNormalMaps || mFailed || !slot || !slot->resident || slot->key != samples.data() || slot->count != samples.size() ||
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
        if (k.ao || k.thickness)
        {
            const bake::GroupInputs& gi = ctx.group;
            const int rays = std::clamp(ctx.settings.options.value("rays", 64), 1, 4096);
            const float diag = glm::length(gi.highMax - gi.highMin);
            const double distOpt = ctx.settings.options.value("distance", 0.0);
            const float dist = distOpt > 0.0 ? float(distOpt) : std::max(diag, 1e-3f);
            const float eps = std::max(1e-4f * diag, 1e-4f);
            const bake::OccluderScope scope = bake::occluderScope(ctx.settings.options.value("scope", gi.settings.aoScope));
            p.rays = uint32_t(rays);
            if (k.thickness)
            {
                const float half = glm::radians(std::clamp(ctx.settings.options.value("spread", 180.0f), 0.0f, 180.0f)) * 0.5f;
                p.params = glm::vec4(dist, eps, half, 0.0f);
            }
            else
            {
                p.boundsMin.w = dist;
                p.boundsExt.w = eps;
            }
            if (scope == bake::OccluderScope::Self)
                p.flags |= kFlagScopeSelf;
            cost = double(rays) * (k.thickness ? 1.5 : 1.0);
            rate = &mRayRate;
            pipe = k.thickness ? (mHardware ? mThicknessHw : mThicknessSw) : (mHardware ? mAoHw : mAoSw);
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
        if ((k.ao || k.thickness) && mHardware && !set)
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
} // namespace lila::render::detail
