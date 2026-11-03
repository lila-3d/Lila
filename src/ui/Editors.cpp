#include "ui/Editors.h"

#include "ui/Actions.h"
#include "ui/LocateFlash.h"
#include "ui/DockSidebar.h"
#include "ui/HotkeyCheatSheet.h"
#include "ui/HotkeyEditor.h"
#include "ui/MarkingMenuEditor.h"
#include "ui/UiCommon.h"

#include <QApplication>
#include <QMainWindow>
#include <QPointer>

namespace lila::ui
{
namespace
{
/** One instance per editor, as a tool window over the main window. */
template <class T> void openSingle(QPointer<T>& slot)
{
    if (!slot)
    {
        slot = new T(mainWindow());
        slot->setWindowFlag(Qt::Window);
        slot->setAttribute(Qt::WA_DeleteOnClose);
    }
    slot->show();
    slot->raise();
    slot->activateWindow();
    flashLocate(slot);
}

QPointer<HotkeyEditor> gHotkeyEditor;
QPointer<MarkingMenuEditor> gMarkingMenuEditor;
} // namespace

void openHotkeyEditor()
{
    openSingle(gHotkeyEditor);
}

void openMarkingMenuEditor()
{
    openSingle(gMarkingMenuEditor);
}

void registerEditorActions()
{
    actions::add({"window.hotkeyEditor", "Hotkey Editor...", "Windows", [] {
                      if (!closeShownWindow<HotkeyEditor>())
                          openHotkeyEditor();
                  }, nullptr,
                  "Browse, search and rebind hotkeys"});
    actions::add({"window.markingMenuEditor", "Marking Menu Editor...", "Windows", [] {
                      if (!closeShownWindow<MarkingMenuEditor>())
                          openMarkingMenuEditor();
                  },
                  nullptr, "Edit marking menu slots, labels and targets"});
    actions::add({"help.hotkeyCheatSheet", "Hotkey Cheat Sheet", "Help", [] { HotkeyCheatSheet::open(); }, nullptr,
                  "Show the hotkeys live in the focused panel: hold to peek, tap to keep it open and filter"});
    actions::add({"help.hotkeyCheatSheetClose", "Close Hotkey Cheat Sheet", "Help", [] { HotkeyCheatSheet::released(); },
                  nullptr, "Hide the hotkey cheat sheet"});
    installMenuHotkeyHook();
}
} // namespace lila::ui
