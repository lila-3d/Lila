#pragma once

#include "render/HdrImage.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace lila::render
{
/**
 * Image-based lighting, precomputed on the CPU and ready to upload. The cube is in Vulkan face order with the
 * environment's own up along +Y. Mip m is GGX-prefiltered for roughness m / (mips - 1); mip 0 is the sharp image.
 */
struct EnvironmentData
{
    int size = 0; // mip 0 face size
    /** Per mip: six faces back to back, RGBA half floats. */
    std::vector<std::vector<uint16_t>> mips;
    /** Diffuse irradiance as SH9 (rgb), already cosine-convolved and divided by pi: albedo * sum(c * Y(n)). */
    std::array<glm::vec4, 9> sh{};
};

/** A neutral gray photo studio: gradient dome, key, fill, rim and top softboxes, dim floor. */
EnvironmentData buildStudioEnvironment();

/** From a latitude-longitude panorama (top row looks up). */
EnvironmentData buildEnvironment(const HdrImage& equirect);

/** Split-sum GGX lookup: u = n.v, v = roughness, texels are (scale, bias) on F0, RG half floats. */
std::vector<uint16_t> buildBrdfLut(int size);
} // namespace lila::render
