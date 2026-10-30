#pragma once

#include "core/Achievements.h"
#include "core/Export.h"

/**
 * Lila's own Steam achievement popup, bottom right of the main window, because the Steam overlay doesn't draw over
 * Lila. Banners queue and show one at a time.
 */
namespace lila::ui::achievementBanner
{
LILA_UI_API void show(const achievements::Shown& shown);
} // namespace lila::ui::achievementBanner
