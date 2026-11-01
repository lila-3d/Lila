#pragma once

#include "render/VulkanContext.h"

#ifdef LILA_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <optional>

/** The render queue's GPU zones in a performance trace (Help > Record Performance Trace). */
namespace lila::render::gpuProfile
{
/** Needs VulkanContext::calibratedTimestamps; without it traces show CPU zones only. */
void start(VulkanContext& vk);
void stop();
/** Reads the zones the GPU has finished. Render thread, once a frame. */
void collect();

#ifdef LILA_TRACY
tracy::VkCtx* context();
#endif

/** Consecutive GPU zones over one command buffer: each begin ends the one before. end() before the buffer ends. */
class Sections
{
public:
    explicit Sections(VkCommandBuffer cmd) : mCmd(cmd) {}
#ifdef LILA_TRACY
    void begin(const tracy::SourceLocationData* where)
    {
        mZone.reset();
        if (tracy::VkCtx* ctx = context())
            mZone.emplace(ctx, where, mCmd, true);
    }
    void end() { mZone.reset(); }

private:
    std::optional<tracy::VkCtxScope> mZone;
#else
    void end() {}

private:
#endif
    [[maybe_unused]] VkCommandBuffer mCmd;
};
} // namespace lila::render::gpuProfile

#ifdef LILA_TRACY
/** name is a string literal. */
#define LILA_GPU_SECTION(sections, name)                                                                                       \
    do                                                                                                                         \
    {                                                                                                                          \
        static constexpr tracy::SourceLocationData lilaGpuWhere{name, TracyFunction, TracyFile, uint32_t(TracyLine), 0};    \
        (sections).begin(&lilaGpuWhere);                                                                                       \
    } while (0)
#else
#define LILA_GPU_SECTION(sections, name) ((void)(sections))
#endif
