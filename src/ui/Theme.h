#pragma once

#include "core/Export.h"

#include <glm/vec4.hpp>

#include <QColor>
#include <QString>

#include <string_view>

class QApplication;
class QFont;
class QWidget;

namespace lila::ui
{
/** Fonts, style and the current theme's colours; follows theme changes live. */
LILA_UI_API void applyTheme(QApplication& app);
/** Code/output font from prefs (default JetBrains Mono). */
LILA_UI_API QFont monoFont();

/** The current theme's colour for a token (see themes::tokens). */
LILA_UI_API QColor themeColor(std::string_view token);
/** A grey level of the Monotone design mapped through the current theme: Monotone returns QColor(v, v, v, alpha). */
LILA_UI_API QColor themeShade(int level, int alpha = 255);
/** A token as sRGB 0..1, or `legacy` exactly while the theme leaves the token at Monotone's value. */
LILA_UI_API glm::vec4 themeSrgb(std::string_view token, const glm::vec4& legacy);
/** themeSrgb converted to linear, alpha kept; `legacyLinear` exactly while the token is Monotone's. */
LILA_UI_API glm::vec4 themeLinear(std::string_view token, const glm::vec4& legacyLinear);
/**
 * A style sheet with its neutral greys (#rrggbb and rgb(v, v, v) with equal channels) mapped through themeShade.
 * Widget style sheets are rewritten this way automatically; a widget with the property "lilaRawColors" set is left
 * alone, for swatches that show a real colour.
 */
LILA_UI_API QString themedStyleSheet(const QString& qss);

/** True on the first GUI start with fresh prefs, until a theme has been picked or the chooser dismissed. */
LILA_UI_API bool themeChooserPending();
LILA_UI_API void setThemeChooserDone();
/** The first-run picker: Monotone, Colour and Light as cards with a preview; picking one switches live. */
LILA_UI_API QWidget* makeThemeChooser(QWidget* parent);
/** Telemetry opt-ins, both off until chosen. */
inline constexpr const char* kTelemetryUsagePref = "/telemetry/usage";
inline constexpr const char* kTelemetryCrashPref = "/telemetry/crashReports";
/**
 * The first-run setup: theme, viewport quality and telemetry, application modal until Continue, which is the only
 * way out.
 */
LILA_UI_API void runFirstRunSetup(QWidget* parent);
/** The first-run dialog's telemetry choice on its own, from Help. */
LILA_UI_API void showTelemetrySettings(QWidget* parent);
} // namespace lila::ui
