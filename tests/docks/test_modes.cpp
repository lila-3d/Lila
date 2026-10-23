#include <doctest/doctest.h>

#include "core/Events.h"
#include "core/ExtensionContext.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Hotkeys.h"
#include "ui/MarkingMenu.h"
#include "ui/Menus.h"
#include "ui/Modes.h"
#include "ui/PreferencesDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTabWidget>

using namespace lila;
using namespace lila::ui;

TEST_CASE("modes switch their hotkey layer, marking-menu set and callbacks, and send modeChanged")
{
    modes::registerBuiltins();
    modes::activate(modes::kDefaultMode, true);
    std::vector<std::string> calls, changed;
    events::SubscriptionId sub = events::subscribe(events::Event::ModeChanged, [&](const events::EventData& e) { changed.push_back(e.name); });
    {
        ext::Context ctx("modeTestExtension");
        CHECK(modes::add(ctx, {.id = "testMode",
                               .label = "Test",
                               .hotkeys = {{"viewport", {{"Space", {{"press", "test.tap"}}}}}},
                               .markingMenuSet = "testSet",
                               .gizmoProvider = "testGizmo",
                               .onEnter = [&] { calls.push_back("enter"); },
                               .onExit = [&] { calls.push_back("exit"); }}));
        CHECK(actions::find("mode.testMode"));
        CHECK(actions::run(nlohmann::json("mode.testMode")));
        CHECK(modes::current() == "testMode");
        CHECK(modes::currentMode()->gizmoProvider == "testGizmo");
        CHECK(Hotkeys::get().mode() == "testMode");
        CHECK(markingMenus::activeSet() == "testSet");
        CHECK(PrefStore::get().value<std::string>("general", "/ui/mode", "") == "testMode");

        QWidget host;
        auto* box = qobject_cast<QComboBox*>(modes::createSwitcher(&host));
        REQUIRE(box);
        CHECK(box->count() == 2);
        CHECK(box->currentText() == "Test");

        // Unloading the extension leaves its mode first, running onExit.
        ctx.unloadAll();
        CHECK(modes::current() == modes::kDefaultMode);
        CHECK(box->count() == 1);
        CHECK(box->currentText() == "Lila");
    }
    CHECK(calls == std::vector<std::string>{"enter", "exit"});
    CHECK(changed == std::vector<std::string>{"testMode", "lila"});
    CHECK(markingMenus::activeSet().empty());
    CHECK_FALSE(actions::find("mode.testMode"));
    CHECK_FALSE(modes::activate("noSuchMode"));
    events::unsubscribe(sub);
}

TEST_CASE("the menu builder keeps QMenus in step with the registry")
{
    QWidget host;
    menus::MenuBuilder builder(&host, {});
    menus::addMenu({.id = "BuilderTest"});
    menus::addActions("BuilderTest", {"edit.undo", "", "edit.redo"});
    menus::addSubmenu("BuilderTest", "Sub", [](QMenu* m) { m->addAction("static row"); });
    menus::addActions("BuilderTest/Sub", {"edit.undo"});
    builder.rebuild();
    QMenu* top = builder.menu("BuilderTest");
    REQUIRE(top);
    QMenu* sub = builder.menu("BuilderTest/Sub");
    REQUIRE(sub);
    CHECK(top->actions().size() == 4);
    CHECK(sub->actions().size() == 2);

    std::string id = menus::addItem({.menu = "BuilderTest", .action = "edit.selectAll", .after = "BuilderTest/edit.undo"}, "someExtension");
    builder.rebuild();
    CHECK(builder.menu("BuilderTest") == top);
    CHECK(builder.menu("BuilderTest/Sub") == sub);
    REQUIRE(top->actions().size() == 5);
    CHECK(top->actions()[1]->data().toString() == "edit.selectAll");
    // The submenu's own static row survives rebuilds.
    CHECK(sub->actions().size() == 2);
    CHECK(sub->actions()[0]->text() == "static row");

    menus::removeItem(id);
    menus::removeMenu("BuilderTest");
    builder.rebuild();
    CHECK_FALSE(builder.menu("BuilderTest"));
}

TEST_CASE("registered preference pages get a tab and their rows are searchable")
{
    ext::Context ctx("prefsTestExtension");
    prefPages::add(ctx, {.id = "prefsTest", .title = "Prefs Test", .file = "prefsTest", .build = [](prefPages::PageBuilder& b) {
                             b.section("Things");
                             b.toggle("Frobnicate the widgets", "/frob", true);
                             b.row("Custom", new QLabel("x"), "/custom");
                         }});
    {
        PreferencesDialog dialog;
        auto* tabs = dialog.findChild<QTabWidget*>();
        REQUIRE(tabs);
        CHECK(tabs->tabText(0) == "General");
        CHECK(tabs->tabText(tabs->count() - 1) == "Prefs Test");
        auto* search = dialog.findChild<QLineEdit*>();
        REQUIRE(search);
        search->setText("frobnicate");
        CHECK_FALSE(tabs->isVisible());
        bool found = false;
        for (QLabel* l : dialog.findChildren<QLabel*>())
            found |= l->text() == "Frobnicate the widgets" && l->isVisibleTo(&dialog);
        CHECK(found);
    }
    ctx.unloadAll();
    PreferencesDialog dialog;
    auto* tabs = dialog.findChild<QTabWidget*>();
    CHECK(tabs->tabText(tabs->count() - 1) != "Prefs Test");
}
