#pragma once

#include "core/Export.h"

#include <functional>
#include <string>

class QWidget;

namespace lila::ui::texturePicker
{
/**
 * A searchable dropdown under anchor listing the project's textures (projectTextures, filtered by textureFolders) with
 * thumbnails, plus None, Browse... and the Texture Folders settings. `current` is the slot's stored path, highlighted
 * when listed. pick gets the chosen file as an absolute path, or "" for None; it is not called when the popup closes
 * without a choice.
 */
LILA_UI_API void open(QWidget* anchor, const std::string& current, std::function<void(const std::string&)> pick);

/** Edits the workspace's include and exclude globs and the open scene's overrides, through textureFolders. */
LILA_UI_API void showFoldersDialog(QWidget* parent);
} // namespace lila::ui::texturePicker
