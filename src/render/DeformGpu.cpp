#include "render/DeformGpu.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "render/Compute.h"
#include "render/Renderer.h"
#include "scene/DeformEval.h"

#include <tbb/parallel_for.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace lila::render
{
namespace
{
using scene::deformEval::Kernel;
using scene::deformEval::Program;

struct DeformPush
{
    VkDeviceAddress points, out, params;
    VkDeviceAddress blobs[4];
    VkDeviceAddress src, dst;
    uint32_t count, first;
};
static_assert(sizeof(DeformPush) == 80);

constexpr VkShaderStageFlags kDeformStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr uint32_t kDeformGroup = 64;
/** Bind data kept on the device; the least recently used goes first past it. */
constexpr VkDeviceSize kResidentBudget = VkDeviceSize(1536) << 20;

const char* entryOf(Kernel k)
{
    switch (k)
    {
    case Kernel::Nonlinear:
        return "nonlinearCS";
    case Kernel::Ffd:
        return "ffdCS";
    case Kernel::Kelvinlet:
        return "kelvinletCS";
    case Kernel::CageLinear:
        return "cageLinearCS";
    case Kernel::CageSomigliana:
        return "cageSomiglianaCS";
    case Kernel::Wrap:
        return "wrapCS";
    case Kernel::DeltaMush:
        return "deltaMushApplyCS";
    }
    return "";
}

class GpuDeformers final : public scene::deformEval::Backend
{
public:
    bool run(const Program& program, std::vector<glm::dvec3>& points) override
    {
        LILA_ZONE();

        Renderer& r = Renderer::get();
        // Evaluating on the render thread itself would wait on its own task.
        if (!r.running() || points.empty() || points.size() > size_t(UINT32_MAX / 3) || std::this_thread::get_id() == mRenderThread.load())
            return false;
        std::vector<float> in(points.size() * 3);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, points.size(), 4096), [&](const tbb::blocked_range<size_t>& b) {
            for (size_t i = b.begin(); i != b.end(); ++i)
                for (int k = 0; k < 3; ++k)
                    in[i * 3 + k] = float(points[i][k]);
        });
        std::vector<float> out(in.size());

        // One deformer at a time: the scratch buffers and the resident set belong to whoever holds this.
        std::scoped_lock lock(mMutex);
        try
        {
            r.runTask([&](VulkanContext& vk) {
                 evaluate(vk, program, in, out);
                 return true;
             })
                .get();
        }
        catch (const std::exception& e)
        {
            if (!mWarned)
                log::warn("GPU deformers: {}; deforming on the CPU", e.what());
            mWarned = true;
            return false;
        }
        tbb::parallel_for(tbb::blocked_range<size_t>(0, points.size(), 4096), [&](const tbb::blocked_range<size_t>& b) {
            for (size_t i = b.begin(); i != b.end(); ++i)
                for (int k = 0; k < 3; ++k)
                    points[i][k] += double(out[i * 3 + k]);
        });
        return true;
    }

    void clear() override
    {
        std::scoped_lock lock(mMutex);
        mDropResident = true;
    }

    /** At renderer stop: everything on the device goes. */
    void release(VulkanContext& vk)
    {
        std::scoped_lock lock(mMutex);
        releaseLocked(vk);
    }

private:
    struct Resident
    {
        compute::Buffer buffer;
        uint64_t lastUse = 0;
    };

    void releaseLocked(VulkanContext& vk)
    {
        for (auto& [key, res] : mResident)
            compute::destroy(vk, res.buffer);
        mResident.clear();
        mResidentBytes = 0;
        for (compute::Buffer* b : {&mHostIn, &mHostOut, &mPoints, &mOut, &mParams, &mScratch[0], &mScratch[1]})
            compute::destroy(vk, *b);
        mSubmit.destroy(vk);
        compute::destroy(vk, mLayout);
        mLayout = VK_NULL_HANDLE;
        mCode.clear();
    }

    /** b sized for at least bytes, recreated (contents lost) when it is smaller. */
    static void fit(VulkanContext& vk, compute::Buffer& b, VkDeviceSize bytes, compute::Host host)
    {
        if (b.buffer && b.size >= bytes)
            return;
        compute::destroy(vk, b);
        b = compute::makeBuffer(vk, std::max<VkDeviceSize>(bytes + bytes / 4, 1 << 16), 0, host);
    }

    VkPipeline pipeline(VulkanContext& vk, const char* entry)
    {
        if (mCode.empty())
        {
            std::string err;
            mCode = compute::loadSpirv("deform.spv", &err);
            if (mCode.empty())
                throw std::runtime_error(err);
            mHash = compute::spirvHash(mCode);
        }
        if (!mLayout)
            mLayout = compute::pipelineLayout(vk, sizeof(DeformPush), kDeformStage);
        return compute::sharedPipelines().get(vk, mLayout, compute::layoutKey(sizeof(DeformPush), kDeformStage), mCode, mHash, entry);
    }

    /** The device copy of a blob, uploaded through staging recorded into cmd when it is not resident yet. */
    VkDeviceAddress blobAddress(VulkanContext& vk, VkCommandBuffer cmd, const scene::deformEval::Blob& b, std::vector<compute::Buffer>& staging)
    {
        if (b.empty())
            return 0;
        auto it = mResident.find(b.key);
        if (it == mResident.end())
        {
            LILA_ZONE_N("deform upload blob");
            Resident res;
            res.buffer = compute::makeBuffer(vk, b.bytes, 0, compute::Host::None);
            staging.push_back(compute::hostBuffer(vk, b.data, b.bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
            compute::copy(cmd, staging.back(), res.buffer, b.bytes);
            mResidentBytes += res.buffer.size;
            it = mResident.emplace(b.key, res).first;
        }
        it->second.lastUse = mTick;
        return it->second.buffer.address;
    }

    void evict(VulkanContext& vk)
    {
        while (mResidentBytes > kResidentBudget && mResident.size() > 1)
        {
            auto oldest = mResident.begin();
            for (auto it = mResident.begin(); it != mResident.end(); ++it)
                if (it->second.lastUse < oldest->second.lastUse)
                    oldest = it;
            if (oldest->second.lastUse == mTick)
                break;
            mResidentBytes -= oldest->second.buffer.size;
            compute::destroy(vk, oldest->second.buffer);
            mResident.erase(oldest);
        }
    }

    /** Render thread. */
    void evaluate(VulkanContext& vk, const Program& p, const std::vector<float>& in, std::vector<float>& out)
    {
        LILA_ZONE();
        mRenderThread = std::this_thread::get_id();

        if (mDropResident)
        {
            for (auto& [key, res] : mResident)
                compute::destroy(vk, res.buffer);
            mResident.clear();
            mResidentBytes = 0;
            mDropResident = false;
        }
        ++mTick;
        mSubmit.init(vk);
        const uint32_t n = uint32_t(in.size() / 3);
        const VkDeviceSize bytes = in.size() * sizeof(float);
        const VkDeviceSize paramBytes = std::max<size_t>(p.params.size(), 1) * 4;
        // Kernels read and write device memory only: every read of host memory from a shader crosses the bus.
        fit(vk, mHostIn, bytes + paramBytes, compute::Host::Write);
        fit(vk, mHostOut, bytes, compute::Host::Read);
        fit(vk, mPoints, bytes, compute::Host::None);
        fit(vk, mOut, bytes, compute::Host::None);
        fit(vk, mParams, paramBytes, compute::Host::None);
        std::memcpy(mHostIn.mapped, in.data(), bytes);
        if (!p.params.empty())
            std::memcpy(static_cast<char*>(mHostIn.mapped) + bytes, p.params.data(), p.params.size() * 4);
        vmaFlushAllocation(vk.allocator, mHostIn.alloc, 0, VK_WHOLE_SIZE);
        const bool mush = p.kernel == Kernel::DeltaMush;
        if (mush)
        {
            fit(vk, mScratch[0], bytes, compute::Host::None);
            fit(vk, mScratch[1], bytes, compute::Host::None);
        }
        const VkPipeline main = pipeline(vk, entryOf(p.kernel));
        const VkPipeline smooth = mush ? pipeline(vk, "deltaMushSmoothCS") : VK_NULL_HANDLE;

        std::vector<compute::Buffer> staging;
        mSubmit.run(vk, [&](VkCommandBuffer cmd) {
            const VkBufferCopy toPoints{0, 0, bytes}, toParams{bytes, 0, paramBytes}, toHost{0, 0, bytes};
            vkCmdCopyBuffer(cmd, mHostIn.buffer, mPoints.buffer, 1, &toPoints);
            vkCmdCopyBuffer(cmd, mHostIn.buffer, mParams.buffer, 1, &toParams);
            DeformPush push{};
            push.points = mPoints.address;
            push.out = mOut.address;
            push.params = mParams.address;
            for (int k = 0; k < 4; ++k)
                push.blobs[k] = blobAddress(vk, cmd, p.blobs[size_t(k)], staging);
            compute::memoryBarrier(cmd);
            auto chunk = [](DeformPush& d, uint32_t first, uint32_t count) {
                d.first = first;
                d.count = count;
            };
            push.src = mPoints.address;
            if (mush)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, smooth);
                for (uint32_t it = 0; it < p.iterations; ++it)
                {
                    push.dst = mScratch[it & 1].address;
                    compute::dispatchChunks(cmd, mLayout, push, 0, n, kDeformGroup, chunk);
                    compute::memoryBarrier(cmd);
                    push.src = push.dst;
                }
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, main);
            compute::dispatchChunks(cmd, mLayout, push, 0, n, kDeformGroup, chunk);
            compute::memoryBarrier(cmd);
            vkCmdCopyBuffer(cmd, mOut.buffer, mHostOut.buffer, 1, &toHost);
        });
        for (compute::Buffer& b : staging)
            compute::destroy(vk, b);
        evict(vk);
        vmaInvalidateAllocation(vk.allocator, mHostOut.alloc, 0, VK_WHOLE_SIZE);
        std::memcpy(out.data(), mHostOut.mapped, bytes);
    }

    std::mutex mMutex;
    std::atomic<std::thread::id> mRenderThread;
    bool mWarned = false;
    bool mDropResident = false;

    // Render thread, under mMutex.
    std::unordered_map<uint64_t, Resident> mResident;
    VkDeviceSize mResidentBytes = 0;
    uint64_t mTick = 0;
    compute::Buffer mHostIn, mHostOut, mPoints, mOut, mParams, mScratch[2];
    compute::Submitter mSubmit;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    std::vector<uint32_t> mCode;
    uint64_t mHash = 0;
};

std::once_flag gDeformStopHook;
std::shared_ptr<GpuDeformers> gBackend;
} // namespace

void registerGpuDeformers()
{
    if (!gBackend)
        gBackend = std::make_shared<GpuDeformers>();
    std::call_once(gDeformStopHook, [] {
        Renderer::get().addStopHook([](VulkanContext& vk) {
            if (gBackend)
                gBackend->release(vk);
        });
    });
    scene::deformEval::setBackend(gBackend);
}

void unregisterGpuDeformers() { scene::deformEval::setBackend(nullptr); }
} // namespace lila::render
