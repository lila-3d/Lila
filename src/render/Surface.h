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
/** Fills the window with an sRGB colour (0..1) through the OS, for the moments before the first frame presents. */
LILA_UI_API void fillWindow(uintptr_t nativeWindow, float r, float g, float b);
} // namespace lila::render
