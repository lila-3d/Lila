#pragma once

#include "core/Export.h"

#include "render/Environment.h"
#include "render/FrameState.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

/** What the viewport background looks like on screen, estimated on the CPU so overlays can pick readable colors. */
namespace lila::render
{
/** The low mips of an environment's prefiltered cube as float rgb, Vulkan face order like EnvironmentData. */
struct EnvProbe
{
    int firstMip = 0;     // probe mip 0 is this environment mip
    int environmentMips = 1;
    std::vector<int> sizes;                     // face size per probe mip
    std::vector<std::vector<glm::vec3>> mips;   // six faces back to back
};

/** Keeps the mips whose faces are at most maxFace texels, or just the last one. */
LILA_UI_API std::shared_ptr<const EnvProbe> makeEnvProbe(const EnvironmentData& env, int maxFace = 32);

/** The probe's radiance along an environment-space direction at an environment lod; nearest texel, linear between mips. */
LILA_UI_API glm::vec3 sampleProbe(const EnvProbe& probe, glm::vec3 dir, float lod);

/**
 * Relative luminance (0 black, 1 white) of the background as the viewport displays it, one value per cell of a cols x
 * rows grid over the view, top row first. Follows the background shader: environment blur, intensity, saturation,
 * backdrop exposure, tonemap and display gamma, or the flat background color when no environment is drawn. A null
 * probe means the environment is not ready, which also draws the flat color.
 */
LILA_UI_API std::vector<float> backdropLuminance(const FrameState& s, const EnvProbe* probe, int cols, int rows);
} // namespace lila::render
