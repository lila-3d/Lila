#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/MorphPanel.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QMenu>
#include <QMouseEvent>
#include <QSlider>
#include <QTreeWidget>

using namespace lila;
using json = nlohmann::json;

namespace
{
json runMorphCmd(const char* name, const json& call = json::object())
{
    return ops::execute(name, call);
}

void settle()
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 120)
        QApplication::processEvents();
}

void middleClick(QWidget* w)
{
    const QPoint at = w->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, at, w->mapToGlobal(at), Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, w->mapToGlobal(at), Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

QAction* find(QMenu* menu, const QString& text)
{
    for (QAction* a : menu->actions())
    {
        if (a->text() == text)
            return a;
        if (a->menu())
            if (QAction* sub = find(a->menu(), text))
                return sub;
    }
    return nullptr;
}
} // namespace

TEST_CASE("morph panel: middle click resets a weight, and each target's slider range is its own, both undoable")
{
    static bool registered = false;
    if (!std::exchange(registered, true))
    {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        ui::actions::registerBuiltins();
    }
    runMorphCmd("newScene");
    std::string base = runMorphCmd("polyCube", {{"name", "Base"}})[0];
    std::string smile = runMorphCmd("polyCube", {{"name", "Smile"}})[0];
    std::string morph = runMorphCmd("morph", {{"args", {smile, base}}});
    std::string target = runMorphCmd("morphTargets", {{"args", {morph}}})[0]["node"];
    runMorphCmd("select", {{"args", {base}}});
    runMorphCmd("setAttr", {{"args", {target + ".weight", 0.6}}});
    UndoStack::get().clear();

    ui::MorphTargetsPanel panel;
    panel.resize(500, 300);
    panel.show();
    settle();
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE(tree);
    REQUIRE(tree->topLevelItemCount() == 1);
    auto weight = [&] { return runMorphCmd("getAttr", {{"args", {target + ".weight"}}}).get<double>(); };
    auto slider = [&] { return panel.findChild<QSlider*>(); };
    REQUIRE(slider());
    CHECK(slider()->minimum() == 0);
    CHECK(slider()->maximum() == 1000);
    CHECK(slider()->value() == 600);

    middleClick(slider());
    CHECK(weight() == doctest::Approx(0.0));
    UndoStack::get().undo();
    CHECK(weight() == doctest::Approx(0.6));
    settle();

    QMenu menu;
    panel.fillTargetMenu(&menu, tree->topLevelItem(0));
    REQUIRE(find(&menu, "0 to 1"));
    CHECK(find(&menu, "0 to 1")->isChecked());
    REQUIRE(find(&menu, "-1 to 1"));
    find(&menu, "-1 to 1")->trigger();
    settle();
    CHECK(runMorphCmd("getAttr", {{"args", {target + ".sliderMin"}}}).get<double>() == -1.0);
    CHECK(slider()->minimum() == -1000);
    CHECK(slider()->maximum() == 1000);
    UndoStack::get().undo();
    settle();
    CHECK(runMorphCmd("getAttr", {{"args", {target + ".sliderMin"}}}).get<double>() == 0.0);
    CHECK(slider()->minimum() == 0);

    QMenu again;
    panel.fillTargetMenu(&again, tree->topLevelItem(0));
    REQUIRE(find(&again, "Reset Weight"));
    find(&again, "Reset Weight")->trigger();
    CHECK(weight() == doctest::Approx(0.0));
    runMorphCmd("newScene");
    UndoStack::get().clear();
}
