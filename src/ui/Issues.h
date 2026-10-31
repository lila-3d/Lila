#pragma once

#include "core/Export.h"

#include "scene/Diagnostics.h"

#include <QColor>
#include <QIcon>
#include <QString>

#include <optional>
#include <vector>

/** How the UI shows scene::diagnostics issues: badges, colors, tooltips, and the Validation panel. */
namespace lila::ui::issues
{
/** General prefs, lockable by a studio like any other: badge info rows, and tint row names at or above a severity. */
inline constexpr const char* kBadgeInfoPref = "/issues/badgeInfo";
inline constexpr const char* kTintRowsPref = "/issues/tintRows";
inline constexpr const char* kValidationDock = "validation";

LILA_UI_API bool badgeInfo();
/** The least severity whose rows get tinted, or none ("off"). */
LILA_UI_API std::optional<scene::diagnostics::Severity> tintFrom();

LILA_UI_API QColor color(scene::diagnostics::Severity s);
/** A round badge, octagonal when blocking, with ! or i in it. */
LILA_UI_API QIcon icon(scene::diagnostics::Severity s, bool blocking = false);
LILA_UI_API QPixmap badge(scene::diagnostics::Severity s, bool blocking, int size);
/** The worst of the issues, and whether any of that severity blocks. */
struct Worst
{
    scene::diagnostics::Severity severity = scene::diagnostics::Severity::Info;
    bool blocking = false;
};
LILA_UI_API std::optional<Worst> worst(const std::vector<const scene::diagnostics::Issue*>& issues, bool includeInfo = true);
/** "error  health:max: must be above 0  (naming)", one line per issue. */
LILA_UI_API QString lines(const std::vector<const scene::diagnostics::Issue*>& issues);
/** "component › attribute" for the issue, or "" for the whole object. */
LILA_UI_API QString where(const scene::diagnostics::Issue& issue);

/** The Validation panel, its window toggle and the Validate Scene action. */
LILA_UI_API void registerUi();
} // namespace lila::ui::issues
