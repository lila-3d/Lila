#pragma once

#include "core/Export.h"

#include <filesystem>
#include <vector>

class QMimeData;

/** Files dropped on the main window from Explorer or Lila's file dialog: scenes open, everything else imports. */
namespace lila::ui::fileDrop
{
struct Plan
{
    /** Replaces the scene first; empty when nothing dropped opens natively. */
    std::filesystem::path open;
    std::vector<std::filesystem::path> imports;
    /** No translator imports these (a second .lila, an unknown extension). */
    std::vector<std::filesystem::path> skipped;

    bool empty() const { return open.empty() && imports.empty(); }
};

/** The local files among a drag's urls. */
LILA_UI_API std::vector<std::filesystem::path> localFiles(const QMimeData* mime);

/**
 * The first file that opens natively (.lila, .ma) opens unless importOnly; the rest import into the scene, in the
 * order dropped.
 */
LILA_UI_API Plan plan(const std::vector<std::filesystem::path>& files, bool importOnly);
} // namespace lila::ui::fileDrop
