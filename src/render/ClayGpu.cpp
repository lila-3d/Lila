#include "render/ClayGpu.h"

#include "clay/Clay.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "render/Compute.h"
#include "render/Renderer.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace lila::render
{
namespace
{
struct ClayPush
{
    VkDeviceAddress prims, code, bricks, localCode, localStart, localCount, out;
    uint32_t first, count;
    float h;
    uint32_t symmetric;
};
static_assert(sizeof(ClayPush) == 72);

constexpr VkShaderStageFlags kClayStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr uint32_t kClayGroup = 64;
constexpr size_t kPerBrick = size_t(clay::kBrickSamples) * clay::kBrickSamples * clay::kBrickSamples;

class GpuClay final : public clay::SampleBackend
{
public:
    bool sample(std::span<const float> prims, std::span<const int32_t> code, bool symmetric, std::span<const glm::ivec3> bricks,
                std::span<const int32_t> localCode, std::span<const uint32_t> localStart, std::span<const uint32_t> localCount, double voxelSize,
                std::vector<float>& out) override
    {
        LILA_ZONE();

        Renderer& r = Renderer::get();
        // Sampling on the render thread itself would wait on its own task.
        if (!r.running() || bricks.empty() || bricks.size() * kPerBrick > size_t(UINT32_MAX) || std::this_thread::get_id() == mRenderThread.load())
            return false;
        out.resize(bricks.size() * kPerBrick);
        std::scoped_lock lock(mMutex);
        try
        {
            r.runTask([&](VulkanContext& vk) {
                 evaluate(vk, prims, code, symmetric, bricks, localCode, localStart, localCount, float(voxelSize), out);
                 return true;
             })
                .get();
        }
        catch (const std::exception& e)
        {
            if (!mWarned)
                log::warn("GPU clay: {}; meshing on the CPU", e.what());
            mWarned = true;
            return false;
        }
        return true;
    }

    /** At renderer stop: everything on the device goes. */
    void release(VulkanContext& vk)
    {
        std::scoped_lock lock(mMutex);
        for (compute::Buffer* b : {&mHostIn, &mHostOut, &mIn, &mOut})
            compute::destroy(vk, *b);
        mSubmit.destroy(vk);
        compute::destroy(vk, mLayout);
        mLayout = VK_NULL_HANDLE;
        mCode.clear();
    }

private:
    static void fit(VulkanContext& vk, compute::Buffer& b, VkDeviceSize bytes, compute::Host host)
    {
        if (b.buffer && b.size >= bytes)
            return;
        compute::destroy(vk, b);
        b = compute::makeBuffer(vk, std::max<VkDeviceSize>(bytes + bytes / 4, 1 << 16), 0, host);
    }

    /** Render thread. */
    void evaluate(VulkanContext& vk, std::span<const float> prims, std::span<const int32_t> code, bool symmetric, std::span<const glm::ivec3> bricks,
                  std::span<const int32_t> localCode, std::span<const uint32_t> localStart, std::span<const uint32_t> localCount, float h,
                  std::vector<float>& out)
    {
        LILA_ZONE();
        mRenderThread = std::this_thread::get_id();
        mSubmit.init(vk);
        if (mCode.empty())
        {
            std::string err;
            mCode = compute::loadSpirv("clay.spv", &err);
            if (mCode.empty())
                throw std::runtime_error(err);
            mHash = compute::spirvHash(mCode);
        }
        if (!mLayout)
            mLayout = compute::pipelineLayout(vk, sizeof(ClayPush), kClayStage);
        const VkPipeline pipeline =
            compute::sharedPipelines().get(vk, mLayout, compute::layoutKey(sizeof(ClayPush), kClayStage), mCode, mHash, "sampleCS");

        // Every input in one upload, each part 16-byte aligned so its address suits any load.
        std::vector<std::pair<const void*, size_t>> parts = {{prims.data(), prims.size_bytes()},           {code.data(), code.size_bytes()},
                                                             {bricks.data(), bricks.size_bytes()},         {localCode.data(), localCode.size_bytes()},
                                                             {localStart.data(), localStart.size_bytes()}, {localCount.data(), localCount.size_bytes()}};
        std::vector<VkDeviceSize> offsets;
        VkDeviceSize inBytes = 0;
        for (const auto& [data, bytes] : parts)
        {
            offsets.push_back(inBytes);
            inBytes += (std::max<size_t>(bytes, 4) + 15) & ~VkDeviceSize(15);
        }
        const VkDeviceSize outBytes = out.size() * sizeof(float);
        fit(vk, mHostIn, inBytes, compute::Host::Write);
        fit(vk, mIn, inBytes, compute::Host::None);
        fit(vk, mOut, outBytes, compute::Host::None);
        fit(vk, mHostOut, outBytes, compute::Host::Read);
        for (size_t k = 0; k < parts.size(); ++k)
            if (parts[k].second)
                std::memcpy(static_cast<char*>(mHostIn.mapped) + offsets[k], parts[k].first, parts[k].second);
        vmaFlushAllocation(vk.allocator, mHostIn.alloc, 0, VK_WHOLE_SIZE);

        mSubmit.run(vk, [&](VkCommandBuffer cmd) {
            const VkBufferCopy up{0, 0, inBytes}, down{0, 0, outBytes};
            vkCmdCopyBuffer(cmd, mHostIn.buffer, mIn.buffer, 1, &up);
            compute::memoryBarrier(cmd);
            ClayPush push{};
            push.prims = mIn.address + offsets[0];
            push.code = mIn.address + offsets[1];
            push.bricks = mIn.address + offsets[2];
            push.localCode = mIn.address + offsets[3];
            push.localStart = mIn.address + offsets[4];
            push.localCount = mIn.address + offsets[5];
            push.out = mOut.address;
            push.h = h;
            push.symmetric = symmetric ? 1u : 0u;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            compute::dispatchChunks(cmd, mLayout, push, 0, uint32_t(out.size()), kClayGroup, [](ClayPush& p, uint32_t first, uint32_t count) {
                p.first = first;
                p.count = count;
            });
            compute::memoryBarrier(cmd);
            vkCmdCopyBuffer(cmd, mOut.buffer, mHostOut.buffer, 1, &down);
        });
        vmaInvalidateAllocation(vk.allocator, mHostOut.alloc, 0, VK_WHOLE_SIZE);
        std::memcpy(out.data(), mHostOut.mapped, outBytes);
    }

    std::mutex mMutex;
    std::atomic<std::thread::id> mRenderThread;
    bool mWarned = false;

    // Render thread, under mMutex.
    compute::Buffer mHostIn, mHostOut, mIn, mOut;
    compute::Submitter mSubmit;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    std::vector<uint32_t> mCode;
    uint64_t mHash = 0;
};

std::once_flag gClayStopHook;
std::shared_ptr<GpuClay> gClay;
} // namespace

void registerGpuClay()
{
    if (!gClay)
        gClay = std::make_shared<GpuClay>();
    std::call_once(gClayStopHook, [] {
        Renderer::get().addStopHook([](VulkanContext& vk) {
            if (gClay)
                gClay->release(vk);
        });
    });
    clay::setSampleBackend(gClay);
}

void unregisterGpuClay() { clay::setSampleBackend(nullptr); }
} // namespace lila::render
