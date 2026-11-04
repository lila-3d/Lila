// Throwaway harness: builds the hotkey editor, cheat sheet, capture dialog and marking menu editor, drives them a
// little and grabs PNGs. Usage: lila_hk_demo <outdir>

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/Editors.h"
#include "ui/HotkeyBindings.h"
#include "ui/HotkeyCheatSheet.h"
#include "ui/HotkeyEditor.h"
#include "ui/Hotkeys.h"
#include "ui/MarkingMenuEditor.h"
#include "ui/Shelf.h"
#include "ui/Theme.h"
#include "ui/Tools.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>

#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace lila;

namespace
{
QString gOut;

void pump()
{
    for (int i = 0; i < 5; ++i)
    {
        mainq::drain();
        QApplication::processEvents();
    }
}

void save(QWidget* w, const char* name)
{
    pump();
    w->grab().save(gOut + "/" + name);
    std::printf("saved %s\n", name);
}

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

int main(int argc, char** argv)
{
    gOut = argc > 1 ? argv[1] : ".";
    // Keep the demo out of the real user prefs.
    auto cfg = std::filesystem::temp_directory_path() / "lila_hk_demo";
    std::filesystem::create_directories(cfg);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", cfg.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", cfg.string().c_str(), 1);
#endif
    mainq::init();
    PrefStore::get().start();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();

    QApplication app(argc, argv);
    ui::applyTheme(app);
    mainq::setWakeup([&app] { QMetaObject::invokeMethod(&app, [] { mainq::drain(); }, Qt::QueuedConnection); });

    ui::actions::registerBuiltins();
    ui::tools::registerActions();
    ui::registerShelfActions();
    ui::registerEditorActions();
    for (const char* id : {"file.open", "file.save", "file.saveAs", "file.openFolder", "file.exit", "file.closeFolder",
                           "window.toggleMaximizePanel", "viewport.frameSelected", "viewport.frameAll"})
        if (!ui::actions::find(id))
            ui::actions::add({id, id, "Stub", [] {}, nullptr, ""});
    ui::Hotkeys::get().install();

    // Host standing in for the main window, with a viewport-context child holding focus.
    QMainWindow host;
    host.resize(1400, 820);
    auto* viewport = new QWidget;
    viewport->setProperty("hotkeyContext", "viewport");
    viewport->setFocusPolicy(Qt::StrongFocus);
    viewport->setAutoFillBackground(true);
    QPalette vp = viewport->palette();
    vp.setColor(QPalette::Window, QColor(60, 60, 60));
    viewport->setPalette(vp);
    host.setCentralWidget(viewport);
    host.show();
    viewport->setFocus();
    pump();

    // Hotkey editor, untouched.
    ui::HotkeyEditor editor;
    editor.resize(900, 760);
    editor.show();
    save(&editor, "hk_editor.png");

    // A conflict: W in global under viewport's W.
    ui::hotkeys::assign("global", QKeyCombination(Qt::Key_W), ui::hotkeys::Slot::Press, "edit.group");
    ui::hotkeys::unbind("global", "Ctrl+G");
    pump();
    std::printf("keyFor(edit.group) after assign = %s\n", ui::Hotkeys::get().keyFor("edit.group").toStdString().c_str());
    save(&editor, "hk_conflict.png");

    // Heat map click on W filters to it.
    if (auto* map = editor.findChild<ui::KeyboardHeatMap*>())
        Q_EMIT map->keyClicked(Qt::Key_W);
    save(&editor, "hk_keyfilter.png");
    if (auto* map = editor.findChild<ui::KeyboardHeatMap*>())
        Q_EMIT map->keyClicked(Qt::Key_W);

    editor.findChild<QLineEdit*>()->setText("undo");
    save(&editor, "hk_search.png");
    editor.findChild<QLineEdit*>()->clear();

    // Capture dialog: synthesize Ctrl+Shift+G through the app filters like a real key press.
    {
        ui::ChordCaptureDialog dlg("edit.parent", "viewport", &host);
        dlg.show();
        pump();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::sendEvent(&dlg, &press);
        save(&dlg, "hk_capture.png");
        dlg.apply();
        pump();
        auto snap = ui::hotkeys::snapshot();
        auto* b = ui::hotkeys::find(snap, "viewport", QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_G));
        std::printf("captured binding: %s\n", b ? (b->keyText + " -> " + ui::hotkeys::describe(*b)).c_str() : "missing");
    }

    // Cheat sheet over the host, rendered on top of a grab of it.
    {
        ui::HotkeyCheatSheet sheet(&host, viewport);
        sheet.resize(host.size());
        QPixmap pm = host.grab();
        QPainter p(&pm);
        sheet.render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
        p.end();
        pm.save(gOut + "/hk_cheatsheet.png");
        std::printf("saved hk_cheatsheet.png\n");
    }

    // Menu hook: right-click an action entry in a QMenu.
    {
        QMenu menu;
        QAction* qa = menu.addAction("Undo");
        qa->setData("edit.undo");
        menu.addAction("Plain");
        menu.popup(host.mapToGlobal(QPoint(200, 200)));
        pump();
        QPoint at = menu.actionGeometry(qa).center();
        QMouseEvent pressEv(QEvent::MouseButtonPress, at, menu.mapToGlobal(at), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&menu, &pressEv);
        QMouseEvent releaseEv(QEvent::MouseButtonRelease, at, menu.mapToGlobal(at), Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&menu, &releaseEv);
        pump();
        QWidget* popup = QApplication::activePopupWidget();
        auto* pm = qobject_cast<QMenu*>(popup);
        std::printf("menu hook popup: %s\n",
                    pm && pm != &menu && !pm->actions().isEmpty() ? pm->actions().first()->text().toStdString().c_str() : "none");
        if (pm && pm != &menu)
            save(pm, "hk_menuhook.png");
        while (QApplication::activePopupWidget())
            QApplication::activePopupWidget()->close();
    }

    // Marking menu editor on a hotbox zone menu.
    {
        ui::MarkingMenuEditor mme;
        mme.show();
        mme.openMenu("hotbox_center_lmb");
        mme.selectPointer("/N");
        save(&mme, "mm_root.png");
        mme.selectPointer("/S/children/E");
        save(&mme, "mm_submenu.png");

        mme.selectPointer("/E");
        for (QPushButton* b : mme.findChildren<QPushButton*>())
            if (b->text() == "Duplicate as Python Item")
                b->click();
        save(&mme, "mm_python.png");
        mme.selectPointer("/W");
        for (QPushButton* b : mme.findChildren<QPushButton*>())
            if (b->text() == "Remove")
                b->click();
        for (QPushButton* b : mme.findChildren<QPushButton*>())
            if (b->text() == "Save")
                b->click();
        pump();
        PrefStore::get().flush();
        std::printf("user hotbox_center_lmb.json:\n%s\n", readFile(PrefStore::get().userDir() / "markingMenus" / "hotbox_center_lmb.json").c_str());
        json merged = PrefStore::get().merged("markingMenus/hotbox_center_lmb");
        std::printf("merged has W: %d, E python: %d\n", int(merged.contains("W")),
                    int(merged["E"]["action"].is_object() && merged["E"]["action"].contains("python")));
    }

    PrefStore::get().flush();
    std::printf("user hotkeys.json:\n%s\n", readFile(PrefStore::get().userDir() / "hotkeys.json").c_str());
    PrefStore::get().stop();
    return 0;
}
