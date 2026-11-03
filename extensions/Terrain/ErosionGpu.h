#pragma once

#include "Erosion.h"

#include <string>

namespace terrain
{
/** True when there is a GPU to erode on: Lila's renderer is running. */
bool gpuErosionAvailable();

/**
 * erode() on the GPU: the same fill, drainage, stream-power and thermal steps and the same masks, within float tolerance
 * of the CPU (the CPU result is the exact, deterministic one). False with err when there is no GPU or it failed; h is
 * unchanged then.
 */
bool erodeGpu(Heightfield& h, const ErosionParams& p, std::string* err = nullptr);

/** GPU time of the last erodeGpu, ms. */
double lastGpuErosionMs();
} // namespace terrain
