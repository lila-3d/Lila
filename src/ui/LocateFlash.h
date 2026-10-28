#pragma once

#include "core/Export.h"

class QWidget;

namespace lila::ui
{
/**
 * Prefs ("general" file): whether windows flash as they open, how many pulses, each pulse's length in seconds, how
 * strongly (0 to 1), its colour ("#rrggbb"; whether it shows is an accent toggle that Monotone overrides), and how far the glow reaches in from the edges (0 for the outline alone, 1 to the centre).
 */
inline constexpr const char* kLocateFlashPref = "/ui/locateFlash";
inline constexpr const char* kLocateFlashPulsesPref = "/ui/locateFlashPulses";
inline constexpr const char* kLocateFlashStrengthPref = "/ui/locateFlashStrength";
inline constexpr const char* kLocateFlashPulseSecondsPref = "/ui/locateFlashPulseSeconds";
inline constexpr const char* kLocateFlashReachPref = "/ui/locateFlashReach";
inline constexpr const char* kLocateFlashColorPref = "/ui/locateFlashColor";
inline constexpr const char* kLocateFlashColorsPref = "/ui/locateFlashColors";
inline constexpr const char* kLocateFlashDefaultColor = "#40a0ff";

/** Pulses an accent outline around the widget per the prefs above, so a window just opened or raised can be found on screen. */
LILA_UI_API void flashLocate(QWidget* target);
} // namespace lila::ui
