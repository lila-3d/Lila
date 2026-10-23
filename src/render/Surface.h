#pragma once

#include "core/Export.h"

#include <volk.h>

#include <cstdint>

namespace lila::render
{
/** Platform seam: HWND on Windows; xcb/wayland and CAMetalLayer later. */
LILA_UI_API VkSurfaceKHR createSurface(VkInstance instance, uintptr_t nativeWindow);
} // namespace lila::render
