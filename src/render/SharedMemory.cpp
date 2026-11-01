#include "render/SharedMemory.h"

#include "core/Log.h"

#ifdef _WIN32
#include <windows.h>

#include <vulkan/vulkan_win32.h>
#endif

namespace lila::render::sharedMemory
{
namespace
{
uint32_t deviceLocalType(VulkanContext& vk, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    return UINT32_MAX;
}
} // namespace

bool allocate(VulkanContext& vk, VkImage image, VkDeviceMemory& memory, void*& handle, uint64_t& size)
{
#ifdef _WIN32
    if (!vk.externalMemory)
        return false;
    // Not in volk's table unless it was built for Win32, so fetched here.
    static const auto getHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(vk.device, "vkGetMemoryWin32HandleKHR"));
    if (!getHandle)
        return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, image, &req);
    const uint32_t type = deviceLocalType(vk, req.memoryTypeBits);
    if (type == UINT32_MAX)
        return false;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image;
    VkExportMemoryAllocateInfo exported{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated};
    exported.handleTypes = kHandleType;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exported};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (VkResult r = vkAllocateMemory(vk.device, &ai, nullptr, &memory); r != VK_SUCCESS)
    {
        log::warn("render: cannot allocate shared image memory ({})", vkResultName(r));
        memory = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryGetWin32HandleInfoKHR hi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    hi.memory = memory;
    hi.handleType = kHandleType;
    HANDLE h = nullptr;
    if (vkBindImageMemory(vk.device, image, memory, 0) != VK_SUCCESS || getHandle(vk.device, &hi, &h) != VK_SUCCESS || !h)
    {
        vkFreeMemory(vk.device, memory, nullptr);
        memory = VK_NULL_HANDLE;
        return false;
    }
    handle = h;
    size = req.size;
    return true;
#else
    (void)vk, (void)image, (void)memory, (void)handle, (void)size;
    return false;
#endif
}

void release(VulkanContext& vk, VkDeviceMemory memory, void* handle)
{
#ifdef _WIN32
    if (handle)
        CloseHandle(handle);
#endif
    (void)handle;
    if (memory)
        vkFreeMemory(vk.device, memory, nullptr);
}
} // namespace lila::render::sharedMemory
