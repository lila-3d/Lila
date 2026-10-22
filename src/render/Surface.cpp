#include "render/Surface.h"

#include "core/Log.h"

#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

namespace lila::render
{
VkSurfaceKHR createSurface(VkInstance instance, uintptr_t nativeWindow)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
#ifdef _WIN32
    auto create = PFN_vkCreateWin32SurfaceKHR(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd = HWND(nativeWindow);
    if (!create || create(instance, &ci, nullptr, &surface) != VK_SUCCESS)
        log::error("vulkan: failed to create a window surface");
#else
    (void)instance;
    (void)nativeWindow;
    log::error("vulkan: window surfaces are not implemented on this platform yet");
#endif
    return surface;
}
} // namespace lila::render
