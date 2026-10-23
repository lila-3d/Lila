#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "core/MainQueue.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/Outliner.h"

#include <QApplication>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }

QTreeWidgetItem* row(ui::Outliner& o, const char* name)
{
    auto found = o.findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
    return found.isEmpty() ? nullptr : found.front();
}

QString style(QTreeWidgetItem* item) { return item->data(0, Qt::UserRole + 2).toString(); }
} // namespace

TEST_CASE("outliner rows: icons, hidden dimming, reveal and kept collapse state")
{
    mainq::init();
    cmds::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();

    run("newScene");
    run("group", {{"empty", true}, {"name", "grp"}});
    run("spaceLocator", {{"name", "loc"}});
    run("parent", {{"args", {"loc", "grp"}}});

    ui::Outliner o;
    QApplication::processEvents();
    REQUIRE(ui::actions::find("outliner.revealSelected"));
    QTreeWidgetItem* loc = row(o, "loc");
    REQUIRE(loc);
    CHECK(style(loc) == "crosshair");
    CHECK(style(row(o, "grp")) == "box-multiple");

    run("toggleVisibility", {{"args", {"grp"}}});
    QApplication::processEvents();
    loc = row(o, "loc");
    CHECK(style(loc) == "crosshair/h");
    CHECK(loc->foreground(0).color() == QColor(100, 100, 100));
    run("undo");
    QApplication::processEvents();
    CHECK(style(row(o, "loc")) == "crosshair");

    o.collapseEverything();
    CHECK_FALSE(row(o, "grp")->isExpanded());
    run("spaceLocator", {{"name", "other"}});
    QApplication::processEvents();
    CHECK_FALSE(row(o, "grp")->isExpanded());

    run("select", {{"args", {"loc"}}});
    QApplication::processEvents();
    o.revealSelected();
    CHECK(row(o, "grp")->isExpanded());
    CHECK(row(o, "loc")->isSelected());
    // The selection's ancestor chain is shaded, the selected row and unrelated rows are not.
    CHECK(row(o, "grp")->data(0, Qt::BackgroundRole).isValid());
    CHECK_FALSE(row(o, "loc")->data(0, Qt::BackgroundRole).isValid());
    CHECK_FALSE(row(o, "other")->data(0, Qt::BackgroundRole).isValid());
    run("select", {{"args", {"other"}}});
    QApplication::processEvents();
    CHECK_FALSE(row(o, "grp")->data(0, Qt::BackgroundRole).isValid());
}

TEST_CASE("outliner lists object sets after the hierarchy, members under them, click selects the members")
{
    run("newScene");
    run("spaceLocator", {{"name", "a"}});
    run("spaceLocator", {{"name", "b"}});
    run("createSet", {{"args", {"a"}}, {"name", "Props"}});

    ui::Outliner o;
    QApplication::processEvents();
    REQUIRE(o.topLevelItemCount() == 3);
    QTreeWidgetItem* set = o.topLevelItem(2);
    CHECK(set->text(0) == "Props");
    CHECK_FALSE(set->isExpanded());
    REQUIRE(set->childCount() == 1);
    CHECK(set->child(0)->text(0) == "a");

    run("addToSet", {{"args", {"Props", "b"}}});
    QApplication::processEvents();
    set = row(o, "Props");
    REQUIRE(set);
    CHECK(set->childCount() == 2);

    run("select", {{"clear", true}});
    emit o.itemClicked(set, 0);
    QApplication::processEvents();
    CHECK(run("ls", {{"selection", true}}) == json({"a", "b"}));
    CHECK_FALSE(set->isSelected());
}
