#pragma once

#include "render/FrameState.h"

namespace lila::ui::viewportDisplay
{
/**
 * Display prefs (file "general") shared by the Display menu, the viewport toolbar and the Preferences dialog. Every
 * viewport reads them each frame, so changing one redraws all views.
 */
inline constexpr const char* kAaEnabled = "/viewport/aa/enabled";
inline constexpr const char* kAaSamples = "/viewport/aa/samples"; // 1, 2, 4 or 8
inline constexpr const char* kGrid = "/viewport/grid";
inline constexpr const char* kBackfaceCulling = "/viewport/backfaceCulling";
inline constexpr const char* kXray = "/viewport/xray";
inline constexpr const char* kLighting = "/viewport/lighting"; // "environment", "studio" or "flat"
inline constexpr const char* kShadows = "/viewport/shadows";
inline constexpr const char* kDefaultMaterial = "/viewport/defaultMaterial";
inline constexpr const char* kExposure = "/viewport/exposure"; // stops
inline constexpr const char* kGamma = "/viewport/gamma";
inline constexpr const char* kToolbar = "/viewport/toolbar";
inline constexpr const char* kVertexColors = "/viewport/vertexColors"; // "off", "shaded" (multiplied in) or "unlit"
inline constexpr const char* kColorChannel = "/viewport/colorChannel"; // "rgb", "r", "g", "b" or "a" (grayscale)
/** Weight displays (skin weights, soft select falloff) as a grayscale ramp instead of the colour ramp. */
inline constexpr const char* kGrayscaleWeights = "/viewport/grayscaleWeights";
inline constexpr int kDefaultSamples = 8;

/** Copies the prefs above onto a frame. */
void apply(render::FrameState& s);

/** Shaded meshes ignore their materials and draw the plain default one. */
bool defaultMaterial();

render::VertexColorMode vertexColorMode();
render::ColorChannel colorChannel();

bool grayscaleWeights();
/**
 * Linear colour for a weight of 0..1: black through blue, cyan, green, yellow and red to white (Maya's paint ramp), or a
 * display-encoded gray ramp (0.5 reads mid gray) when grayscale.
 */
glm::vec4 weightColor(float w, bool grayscale);
/** Soft select falloff: dark red at the rim through orange to yellow at full weight (Maya's), or gray. Linear. */
glm::vec4 falloffColor(float w, bool grayscale);
} // namespace lila::ui::viewportDisplay
