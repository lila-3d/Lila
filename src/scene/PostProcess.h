#pragma once

#include "core/Export.h"
#include "scene/Node.h"

namespace lila::scene
{
class Graph;

/**
 * A post process: how the scene's light becomes the picture. Tonemapper, exposure, white balance and grading, bloom,
 * lens effects and a LUT, applied by the viewport (unless its post effects are off) and by renders. The first enabled
 * one in the scene is the one used. Attribute order matches the Attr enum.
 */
namespace postProcess
{
enum Attr
{
    kEnabled,
    kTonemapper, // neutral, agx, aces, linear
    kAgxLook,    // none, punchy, golden
    kExposure,   // stops
    kAutoExposure,
    kExposureCompensation,
    kExposureMin,
    kExposureMax,
    kExposureSpeed,
    kTemperature, // kelvin; 6500 leaves white alone
    kTint,
    kContrast,
    kSaturation,
    kLift,
    kGamma,
    kGain,
    kBloom,
    kBloomRadius,
    kVignette,
    kGrain,
    kChromaticAberration,
    kDistortion,
    kSharpen,
    kLut,
    kLutIntensity,
    kFog,
    kFogColor,
    kFogDensity,
    kFogHeight,
    kFogFalloff,
    kFogStart,
    kFogSunScatter,
    kFogAnisotropy,
    kCount
};

/** The node's own attribute indices start after the base node's; this is the first. */
LILA_SCENE_API int firstAttr();

/** The first enabled post process node, or kNoNode. */
LILA_SCENE_API NodeId active(const Graph& g);

/** Per-channel gains that turn a light of `kelvin` white, then shift green against magenta by tint (-1..1). */
LILA_SCENE_API glm::dvec3 whiteBalance(double kelvin, double tint);

void registerTypes();
void registerCommands();
} // namespace postProcess
} // namespace lila::scene
