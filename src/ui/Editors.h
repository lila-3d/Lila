#pragma once

namespace lila::ui
{
/**
 * Registers window.hotkeyEditor, window.markingMenuEditor, help.hotkeyCheatSheet and help.hotkeyCheatSheetClose,
 * and installs the "Assign Hotkey..." right-click on menus. Call after the QApplication exists.
 */
void registerEditorActions();

void openHotkeyEditor();
void openMarkingMenuEditor();
} // namespace lila::ui
