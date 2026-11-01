#include "render/GpuProfile.h"

#include "core/Log.h"

#include <algorithm>
#include <vector>

namespace lila::render::gpuProfile
{
#ifdef LILA_TRACY
namespace
{
tracy::VkCtx* gContext = nullptr;

/** Tracy lines the GPU up against the CPU through this host clock. */
bool hasHostDomain(VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT domains)
{
#ifdef _WIN32
    constexpr VkTimeDomainEXT host = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT;
#else
    constexpr VkTimeDomainEXT host = VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
#endif
    uint32_t n = 0;
    if (!domains || domains(physical, &n, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkTimeDomainEXT> list(n);
    domains(physical, &n, list.data());
    return std::ranges::find(list, host) != list.end() && std::ranges::find(list, VK_TIME_DOMAIN_DEVICE_EXT) != list.end();
}
} // namespace

tracy::VkCtx* context() { return gContext; }

void start(VulkanContext& vk)
{
    if (gContext)
        return;
    if (!vk.calibratedTimestamps)
    {
        log::info("profiler: the GPU has no calibrated timestamps; traces show CPU zones only");
        return;
    }
    const bool khr = std::string_view(vk.calibratedTimestamps) == VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    auto domains = khr ? reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)
                       : vkGetPhysicalDeviceCalibrateableTimeDomainsEXT;
    auto calibrated = khr ? reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(vkGetCalibratedTimestampsKHR) : vkGetCalibratedTimestampsEXT;
    if (!calibrated || !vkResetQueryPool || !hasHostDomain(vk.physical, domains))
    {
        log::info("profiler: no calibrated GPU clock; traces show CPU zones only");
        return;
    }
    gContext = TracyVkContextHostCalibrated(vk.physical, vk.device, vkResetQueryPool, domains, calibrated);
    const std::string name = "Viewport (" + vk.deviceName + ")";
    TracyVkContextName(gContext, name.c_str(), uint16_t(name.size()));
    log::info("profiler: GPU zones through {}", vk.calibratedTimestamps);
}

void stop()
{
    if (gContext)
        TracyVkDestroy(gContext);
    gContext = nullptr;
}

void collect()
{
    if (gContext)
        TracyVkCollectHost(gContext);
}
#else
void start(VulkanContext&) {}
void stop() {}
void collect() {}
#endif
} // namespace lila::render::gpuProfile
