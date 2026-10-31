#pragma once

#include "core/Export.h"

namespace lila::render
{
/**
 * Registers the renderer's clay sampler (clay/Clay.h SampleBackend): each update's bricks sampled by one compute
 * dispatch (shaders/clay.slang) in a render-thread task. The app calls it once the renderer is running; lilabatch never
 * does, so batch meshing stays on the CPU.
 */
LILA_UI_API void registerGpuClay();
/** Removes it again (tests). */
LILA_UI_API void unregisterGpuClay();
} // namespace lila::render
