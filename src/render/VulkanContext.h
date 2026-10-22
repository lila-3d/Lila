#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <string>
#include <vector>

namespace lila::render
{
/** Instance, device and allocator shared by every viewport. Created once, used from the render thread. */
class VulkanContext
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

private:
    VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
};

const char* vkResultName(VkResult r);
} // namespace lila::render
