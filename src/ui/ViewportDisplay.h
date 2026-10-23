#pragma once

#include "core/Export.h"

#include "render/FrameState.h"

#include <algorithm>

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

/** Full monotone: no hue anywhere, overriding every accent toggle below. Off by default. */
inline constexpr const char* kMonotone = "/ui/monotone";
/** Selective colour: the few displays that read better with hue, each on by default. */
inline constexpr const char* kViewAxisColors = "/viewport/viewAxisColors";
inline constexpr const char* kManipulatorAxisColors = "/manipulator/axisColors";
inline constexpr const char* kTimeKeyColors = "/timeline/keyColors";
inline constexpr const char* kGraphAxisColors = "/graphEditor/axisColors";
inline constexpr const char* kGhostColors = "/anim/ghost/colors";
inline constexpr const char* kComponentColors = "/viewport/componentColors";

/** Component thickness in logical pixels: edge line widths and vertex square sizes, unselected, hovered and selected. */
inline constexpr const char* kEdgeThickness = "/viewport/components/edgeThickness";
inline constexpr const char* kEdgeThicknessHover = "/viewport/components/edgeThicknessHover";
inline constexpr const char* kEdgeThicknessSelected = "/viewport/components/edgeThicknessSelected";
inline constexpr const char* kVertexThickness = "/viewport/components/vertexThickness";
inline constexpr const char* kVertexThicknessHover = "/viewport/components/vertexThicknessHover";
inline constexpr const char* kVertexThicknessSelected = "/viewport/components/vertexThicknessSelected";
/** UV seams drawn over meshes while the UV editor shows them. */
inline constexpr const char* kUvSeamThickness = "/viewport/components/uvSeamThickness";

struct ComponentThickness
{
    double edge = 2.0;
    double edgeHover = 3.5;
    double edgeSelected = 3.0;
    double vertex = 4.0;
    double vertexHover = 8.0;
    double vertexSelected = 6.0;
    double uvSeam = 3.0;
};

LILA_UI_API ComponentThickness componentThickness();

/**
 * Device-pixel width of a hardware line for a logical width. 1 stays a one-pixel hairline at any display scale; the
 * width beyond it scales with the display like the drawn components do.
 */
inline float hairlineWidth(double logical, double dpr) { return float(std::max(1.0, 1.0 + (logical - 1.0) * dpr)); }

inline bool accentColors(bool monotone, bool selective) { return selective && !monotone; }
/** Whether the display behind one of the accent toggles above draws in colour right now. */
LILA_UI_API bool accentColors(const char* key);
/** The theme's X, Y and Z colours (Monotone: muted red, green and blue that read on the dark UI greys). sRGB 0..1. */
LILA_UI_API glm::vec3 axisColor(int axis);
/** Key ticks on the time slider, the theme's keyTick (Monotone: Maya's red). sRGB 0..1. */
LILA_UI_API glm::vec3 keyTickColor();

/** Copies the prefs above onto a frame. */
LILA_UI_API void apply(render::FrameState& s);

/** Shaded meshes ignore their materials and draw the plain default one. */
LILA_UI_API bool defaultMaterial();

LILA_UI_API render::VertexColorMode vertexColorMode();
LILA_UI_API render::ColorChannel colorChannel();

LILA_UI_API bool grayscaleWeights();
/**
 * Linear colour for a weight of 0..1: black through blue, cyan, green, yellow and red to white (Maya's paint ramp), or a
 * display-encoded gray ramp (0.5 reads mid gray) when grayscale.
 */
LILA_UI_API glm::vec4 weightColor(float w, bool grayscale);
/** Soft select falloff: dark red at the rim through orange to yellow at full weight (Maya's), or gray. Linear. */
LILA_UI_API glm::vec4 falloffColor(float w, bool grayscale);
} // namespace lila::ui::viewportDisplay
