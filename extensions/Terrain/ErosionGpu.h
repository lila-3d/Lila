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

/**
 * drain() with the depression fill on the GPU: the same result (the fill is exact; receivers, order and area come from
 * drainFilled). False with err when there is no GPU or it failed.
 */
bool drainGpu(const Heightfield& h, Drainage& out, std::string* err = nullptr);

/** drainGpu() while the renderer runs, else drain(): the result is the same either way. */
Drainage drainAuto(const Heightfield& h);

/** GPU time of the last erodeGpu or drainGpu, ms. */
double lastGpuErosionMs();
} // namespace terrain
