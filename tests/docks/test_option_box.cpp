#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "cmds/CommandOptions.h"
#include "ui/Actions.h"
#include "ui/OptionBox.h"

#include <QApplication>
#include <QDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QStyleFactory>

#include <cstdlib>

using namespace lila;

namespace
{
int probeRuns = 0;

QDialog* optionsDialog()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* d = qobject_cast<QDialog*>(w); d && d->isVisible() && d->windowTitle() == "Box Probe Options")
            return d;
    return nullptr;
}

void click(QMenu& menu, QPoint at, Qt::KeyboardModifiers mods)
{
    QMouseEvent press(QEvent::MouseButtonPress, at, menu.mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, mods);
    QApplication::sendEvent(&menu, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, menu.mapToGlobal(at), Qt::LeftButton, Qt::NoButton, mods);
    QApplication::sendEvent(&menu, &release);
}
} // namespace

TEST_CASE("menu rows of commands with flags get an option box that opens the generated dialog")
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    if (!cmds::find("boxProbe"))
        cmds::registerCommand({.name = "boxProbe",
                               .doc = "Probe.",
                               .flags = {{"segments", "", ArgType::Int, 2, "How many."}, {"mode", "", ArgType::String, "a", "a | b"}},
                               .undoable = false,
                               .run = [](const Args&) -> json {
                                   ++probeRuns;
                                   return nullptr;
                               }});
    ui::actions::add({.id = "test.boxProbe", .label = "Box Probe", .category = "Mesh", .doc = "Runs the probe.", .command = "boxProbe"});
    ui::actions::add({.id = "test.noBox", .label = "No Box", .category = "Mesh", .run = [] {}, .doc = "Plain action."});
    CHECK(ui::optionBox::available("test.boxProbe"));
    CHECK_FALSE(ui::optionBox::available("test.noBox"));

    QMenu menu;
    for (const char* id : {"test.noBox", "test.boxProbe"})
        menu.addAction(QString::fromStdString(ui::actions::find(id)->label))->setData(QString(id));
    ui::optionBox::decorateMenu(&menu);
    menu.popup(QPoint(100, 100));
    QApplication::processEvents();
    QAction* row = menu.actions()[1];
    CHECK(row->text().contains('\t'));
    CHECK_FALSE(menu.actions()[0]->text().contains('\t'));

    if (const char* shot = std::getenv("LILA_OPTIONBOX_SHOT"))
        menu.grab().save(QString::fromLocal8Bit(shot));

    cmds::options::reset("boxProbe");
    probeRuns = 0;
    CHECK(ui::actions::run(json("test.boxProbe")));
    CHECK(probeRuns == 1);

    // Shift+click opens the options instead of running.
    QRect r = menu.actionGeometry(row);
    click(menu, QPoint(r.left() + 20, r.center().y()), Qt::ShiftModifier);
    QApplication::processEvents();
    QDialog* d = optionsDialog();
    REQUIRE(d);
    CHECK(probeRuns == 1);
    if (const char* shot = std::getenv("LILA_OPTIONBOX_DIALOG_SHOT"))
        d->grab().save(QString::fromLocal8Bit(shot));
    d->close();
    QApplication::processEvents();

    // So does a click on the square at the row's right end.
    menu.popup(QPoint(100, 100));
    QApplication::processEvents();
    r = menu.actionGeometry(row);
    click(menu, QPoint(r.right() - 22, r.center().y()), Qt::NoModifier);
    QApplication::processEvents();
    CHECK(optionsDialog());
    CHECK(probeRuns == 1);
    if (QDialog* again = optionsDialog())
        again->close();
    QApplication::processEvents();
}
