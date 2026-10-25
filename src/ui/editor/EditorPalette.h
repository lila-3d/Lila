#pragma once

#include "core/Export.h"

#include <QColor>

#include <functional>

class QObject;

/**
 * The script editor's colours. Monotone tokens differ by lightness, weight and slant only, never hue; the colour look
 * adds syntax hues. Neutrals go through the app theme's grey ramp either way, so a light theme gets a light editor.
 */
namespace lila::ui::palette
{
/** scriptEditor prefs: grey tokens, and whether the app being Monotone forces them. */
inline constexpr const char* kMonotonePref = "/monotone";
inline constexpr const char* kFollowAppMonotonePref = "/followAppMonotone";

struct Look
{
    bool monotone = true;
    bool light = false;

    bool operator==(const Look&) const = default;
};

/** Grey when the editor's own pref asks, or when it follows the app's Monotone and the app is Monotone. */
LILA_UI_API Look resolveLook(bool editorMonotone, bool followAppMonotone, bool appMonotone, bool lightTheme);
/** The Monotone theme, or the general "/ui/monotone" colour override. */
LILA_UI_API bool appMonotone();
/** True while the app's Monotone is what keeps the editor grey, so the editor's own toggle has no effect. */
LILA_UI_API bool monotoneForcedByApp();
/** The look the prefs and theme give right now. */
LILA_UI_API Look currentLook();

/** Runs `fn` whenever the look changes, until `owner` is destroyed. */
LILA_UI_API void onLookChanged(QObject* owner, std::function<void()> fn);

/** A grey level of the Monotone design mapped through the app theme. */
LILA_UI_API QColor gray(int v, int alpha = 255);

/** Named colours for the current look; updated in place when it changes. */
struct Scheme
{
    QColor background, text, currentLine, selection, selectedText;
    QColor gutterBg, gutterText, gutterCurrent;
    QColor bracketMatch, searchMatch, searchCurrent, occurrence, cellRule;
    QColor squiggleError, squiggleWarning, squiggleInfo, unnecessaryText;
    QColor outputBg, outputText, outputError, outputWarning, outputInfo, outputEcho, outputLink;
};

/** Computed from the prefs on first use. */
LILA_UI_API const Scheme& scheme();

namespace detail
{
/** The storage scheme() fills, without filling it: safe during static initialisation. */
LILA_UI_API const Scheme& storage();
} // namespace detail

inline const QColor& background = detail::storage().background;
inline const QColor& text = detail::storage().text;
inline const QColor& currentLine = detail::storage().currentLine;
inline const QColor& selection = detail::storage().selection;
inline const QColor& gutterBg = detail::storage().gutterBg;
inline const QColor& gutterText = detail::storage().gutterText;
inline const QColor& gutterCurrent = detail::storage().gutterCurrent;
inline const QColor& bracketMatch = detail::storage().bracketMatch;
inline const QColor& searchMatch = detail::storage().searchMatch;
inline const QColor& searchCurrent = detail::storage().searchCurrent;
inline const QColor& occurrence = detail::storage().occurrence;
inline const QColor& cellRule = detail::storage().cellRule;
inline const QColor& squiggleError = detail::storage().squiggleError;
inline const QColor& squiggleWarning = detail::storage().squiggleWarning;
inline const QColor& squiggleInfo = detail::storage().squiggleInfo;
inline const QColor& unnecessaryText = detail::storage().unnecessaryText;
} // namespace lila::ui::palette
