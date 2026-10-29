#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>

class QWidget;

/**
 * Help > Save Support Bundle: one zip with what a bug report needs. The session report (version, system, GPU, recent
 * log and commands), the newest crash reports with their minidumps, and the prefs with tokens and passwords
 * removed; the open scene only when the user ticks it and no studio has turned that off.
 */
namespace lila::ui::support
{
/** Studios set this false (and lock it) to keep scenes out of bundles. */
inline constexpr const char* kAllowScenePref = "/support/allowScene";

LILA_UI_API bool writeBundle(const std::filesystem::path& zip, bool includeScene, std::string* error = nullptr);
LILA_UI_API void showDialog(QWidget* parent);
} // namespace lila::ui::support
