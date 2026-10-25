#pragma once

#include "core/Export.h"

namespace lila::render
{
/**
 * Registers the renderer's GPU deformer backend (scene/DeformEval.h): each GPU-capable deformer runs as a compute
 * kernel (shaders/deform.slang) in a render-thread task, its bind data kept resident on the device by key. The app
 * calls it once the renderer is running; lilabatch never does, so batch work stays on the CPU reference.
 */
LILA_UI_API void registerGpuDeformers();
/** Removes it again (tests). */
LILA_UI_API void unregisterGpuDeformers();
} // namespace lila::render
