#pragma once

#include "core/Export.h"

#include <volk.h>

#include <cstdint>

namespace lila::render
{
/** Platform seam: HWND on Windows; xcb/wayland and CAMetalLayer later. */
LILA_UI_API VkSurfaceKHR createSurface(VkInstance instance, uintptr_t nativeWindow);
/** Refresh rate of the display showing the window; 60 when unknown. */
LILA_UI_API double refreshHz(uintptr_t nativeWindow);
} // namespace lila::render
