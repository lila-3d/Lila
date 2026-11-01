#pragma once

#include "render/VulkanContext.h"

#include <cstdint>

/** Device memory another Vulkan device in the process imports: dedicated to one image, exported as an opaque Win32 handle. */
namespace lila::render::sharedMemory
{
inline constexpr VkExternalMemoryHandleTypeFlagBits kHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

/** Allocates and binds image's memory, exportable; image must be created with VkExternalMemoryImageCreateInfo for kHandleType. */
bool allocate(VulkanContext& vk, VkImage image, VkDeviceMemory& memory, void*& handle, uint64_t& size);
/** Frees what allocate made, closing the handle. */
void release(VulkanContext& vk, VkDeviceMemory memory, void* handle);
} // namespace lila::render::sharedMemory
