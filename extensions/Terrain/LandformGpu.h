#pragma once

#include "ErosionFilter.h"
#include "Terrain.h"

#include <string>
#include <vector>

namespace terrain
{
/**
 * GPU versions of the per-sample landform steps, within float tolerance of the CPU ones (which stay the exact,
 * deterministic reference). Each returns false with err when there is no GPU or it failed, leaving h unchanged.
 */
bool erosionFilterGpu(Heightfield& h, const ErosionFilterParams& p, std::string* err = nullptr);
bool thermalMaterialGpu(Heightfield& h, const std::vector<float>& angles, int iterations, std::string* err = nullptr);
} // namespace terrain
