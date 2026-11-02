#pragma once

#include "core/Export.h"

namespace lila::ui
{
inline constexpr const char* kRetargeterDock = "retargeter";

/** The Retarget Animation dialog, the Retargeter panel and their actions (anim.retarget, anim.characterize, window.retargeter). */
LILA_UI_API void registerRetargetUi();
/** Shows the Retarget Animation dialog, the one-button path. */
LILA_UI_API void showRetargetDialog();
} // namespace lila::ui
