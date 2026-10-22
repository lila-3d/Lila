#pragma once

#include <volk.h>

#include <cstdint>

namespace lila::render
{
/** Platform seam: HWND on Windows; xcb/wayland and CAMetalLayer later. */
VkSurfaceKHR createSurface(VkInstance instance, uintptr_t nativeWindow);
} // namespace lila::render
