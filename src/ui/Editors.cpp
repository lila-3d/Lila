#include "ui/Editors.h"

#include "ui/Actions.h"
#include "ui/HotkeyCheatSheet.h"
#include "ui/HotkeyEditor.h"
#include "ui/MarkingMenuEditor.h"

#include <QApplication>
#include <QMainWindow>
#include <QPointer>

namespace lila::ui
{
namespace
{
QWidget* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (qobject_cast<QMainWindow*>(w))
            return w;
    return nullptr;
}

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
    actions::add({"window.hotkeyEditor", "Hotkey Editor...", "Windows", [] { openHotkeyEditor(); }, nullptr,
                  "Browse, search and rebind hotkeys"});
    actions::add({"window.markingMenuEditor", "Marking Menu Editor...", "Windows", [] { openMarkingMenuEditor(); },
                  nullptr, "Edit marking menu slots, labels and targets"});
    actions::add({"help.hotkeyCheatSheet", "Hotkey Cheat Sheet", "Help", [] { HotkeyCheatSheet::open(); }, nullptr,
                  "Show the hotkeys live in the focused panel"});
    actions::add({"help.hotkeyCheatSheetClose", "Close Hotkey Cheat Sheet", "Help", [] { HotkeyCheatSheet::dismiss(); },
                  nullptr, "Hide the hotkey cheat sheet"});
    installMenuHotkeyHook();
}
} // namespace lila::ui
