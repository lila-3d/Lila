#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Registers window.hotkeyEditor, window.markingMenuEditor, help.hotkeyCheatSheet and help.hotkeyCheatSheetClose,
 * and installs the "Assign Hotkey..." right-click on menus. Call after the QApplication exists.
 */
LILA_UI_API void registerEditorActions();

LILA_UI_API void openHotkeyEditor();
LILA_UI_API void openMarkingMenuEditor();
} // namespace lila::ui
