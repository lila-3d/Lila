#pragma once

#include "core/Export.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <string>
#include <vector>

namespace lila::render
{
/** Instance, device and allocator shared by every viewport. Created once, used from the render thread. */
class LILA_UI_API VulkanContext
{
public:
    bool init(bool validation);
    void shutdown();

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkSampleCountFlagBits msaa = VK_SAMPLE_COUNT_4_BIT; // what the viewport targets and pipelines use right now
    VkSampleCountFlags msaaSupported = VK_SAMPLE_COUNT_1_BIT;
    bool anisotropy = false; // samplerAnisotropy enabled
    std::string deviceName;

    // Optional features, each enabled only when the device supports it.
    /** VK_KHR_acceleration_structure, VK_KHR_ray_query and VK_KHR_deferred_host_operations. */
    bool rayQuery = false;
    /** Runtime-sized, partially bound, update-after-bind (also while pending) descriptor arrays (bindless). LILA_VK_NO_BINDLESS turns it off. */
    bool descriptorIndexing = false;
    bool shaderFloat16 = false;
    bool shaderInt64 = false;
    /** Double precision in shaders (the baker's raster matches the CPU's coverage tests with it). */
    bool shaderFloat64 = false;
    bool int64Atomics = false;
    /** Fragment shaders writing storage buffers (virtual texture feedback). */
    bool fragmentStores = false;
    /** VK_EXT_memory_budget: the allocator tracks per-heap budgets (vmaGetHeapBudgets). */
    bool memoryBudget = false;
    uint32_t asScratchAlignment = 256;

private:
    VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
};

LILA_UI_API const char* vkResultName(VkResult r);
} // namespace lila::render
