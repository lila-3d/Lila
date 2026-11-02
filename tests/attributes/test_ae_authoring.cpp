#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/AttrComponents.h"
#include "scene/Graph.h"
#include "ui/AttrAuthoring.h"
#include "ui/AttributeEditor.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>

using namespace lila;
using namespace lila::scene;
namespace aa = lila::ui::attrAuthoring;

namespace
{
json cmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
json query(const std::string& plug) { return cmd("editAttr", {{"args", {plug}}, {"query", true}}); }
void settle()
{
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
}
NodeId idOf(const char* name) { return Graph::current().find(name)->id(); }

QListWidgetItem* itemStarting(QListWidget* list, const QString& prefix)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text().startsWith(prefix))
            return list->item(i);
    return nullptr;
}
} // namespace

TEST_CASE("Add popover: a typed name becomes a new attribute with type, default, range and heading")
{
    cmd("newScene");
    cmd("spaceLocator", {{"name", "Hero"}});
    aa::AddPopover pop({idOf("Hero")});
    auto* search = pop.findChild<QLineEdit*>("search");
    auto* list = pop.findChild<QListWidget*>("list");
    search->setText("maxHealth");
    REQUIRE(list->currentItem());
    CHECK(list->currentItem()->text() == "New attribute \"maxHealth\"");
    CHECK(pop.activate(list->currentItem())); // opens the form
    CHECK_FALSE(pop.findChild<QWidget*>("form")->isHidden());
    pop.findChild<QComboBox*>("type")->setCurrentText("int");
    pop.findChild<QLineEdit*>("default")->setText("12.6");
    pop.findChild<QLineEdit*>("min")->setText("0");
    pop.findChild<QComboBox*>("category")->setCurrentText("Stats");
    CHECK(pop.commitNew());
    json q = query("Hero.maxHealth");
    CHECK(q["type"] == "int");
    CHECK(q["default"] == 13);
    CHECK(q["min"] == 0.0);
    CHECK(q["category"] == "Stats");

    // A taken name offers no new attribute; a bad default is reported, not added.
    aa::AddPopover again({idOf("Hero")});
    again.findChild<QLineEdit*>("search")->setText("maxHealth");
    CHECK_FALSE(itemStarting(again.findChild<QListWidget*>("list"), "New attribute"));
    again.findChild<QLineEdit*>("search")->setText("speed");
    again.findChild<QComboBox*>("type")->setCurrentText("vector");
    again.findChild<QLineEdit*>("default")->setText("1 2");
    CHECK_FALSE(again.commitNew());
    CHECK_FALSE(again.findChild<QLabel*>("status")->isHidden());
    again.findChild<QLineEdit*>("default")->setText("1, 2, 3");
    CHECK(again.commitNew());
    CHECK(cmd("getAttr", {{"args", {"Hero.speed"}}}) == json::array({1.0, 2.0, 3.0}));

    aa::AddPopover text({idOf("Hero")});
    text.findChild<QLineEdit*>("search")->setText("notes");
    text.findChild<QComboBox*>("type")->setCurrentText("text");
    CHECK(text.commitNew());
    CHECK(query("Hero.notes")["type"] == "text");
    cmd("undo");
    CHECK_FALSE(cmd("objExists", {{"args", {"Hero.notes"}}}).get<bool>());
}

TEST_CASE("Add popover lists saved components and objects to copy from")
{
    cmd("newScene");
    cmd("spaceLocator", {{"name", "Src"}});
    cmd("spaceLocator", {{"name", "Dst"}});
    cmd("addAttr", {{"args", {"Src"}}, {"longName", "ammo"}, {"attributeType", "long"}, {"defaultValue", 30}, {"category", "UiTestWeapon"}});
    cmd("saveComponent", {{"args", {"Src"}}, {"category", "UiTestWeapon"}});

    aa::AddPopover pop({idOf("Dst")});
    auto* list = pop.findChild<QListWidget*>("list");
    pop.findChild<QLineEdit*>("search")->setText("UiTestWea");
    QListWidgetItem* comp = itemStarting(list, "UiTestWeapon");
    REQUIRE(comp);
    CHECK(pop.activate(comp));
    CHECK(cmd("getAttr", {{"args", {"Dst.ammo"}}}).get<int>() == 30);

    cmd("spaceLocator", {{"name", "Third"}});
    aa::AddPopover copy({idOf("Third")});
    copy.findChild<QLineEdit*>("search")->setText("Src");
    QListWidgetItem* from = itemStarting(copy.findChild<QListWidget*>("list"), "Src");
    REQUIRE(from);
    CHECK(copy.activate(from));
    CHECK(query("Third.ammo")["category"] == "UiTestWeapon");
    cmd("deleteComponent", {{"name", "UiTestWeapon"}});
}

TEST_CASE("Edit popover renames, retypes, edits enum options and ranges in one undo step")
{
    cmd("newScene");
    cmd("spaceLocator", {{"name", "Ctrl"}});
    cmd("addAttr", {{"args", {"Ctrl"}}, {"longName", "mode"}, {"attributeType", "enum"}, {"enumName", "a:b"}});
    aa::EditPopover pop({idOf("Ctrl")}, "mode");
    CHECK(pop.findChild<QLineEdit*>("name")->text() == "mode");
    auto* list = pop.findChild<QListWidget*>("enum");
    REQUIRE(list->count() == 2);
    list->item(1)->setText("bee");
    pop.findChild<QToolButton*>("enumAdd")->click();
    list->item(2)->setText("sea");
    pop.findChild<QLineEdit*>("name")->setText("style");
    pop.findChild<QLineEdit*>("doc")->setText("Look");
    pop.findChild<QCheckBox*>("keyable")->setChecked(true);
    CHECK(pop.apply());
    json q = query("Ctrl.style");
    CHECK(q["enum"] == json::array({"a", "bee", "sea"}));
    CHECK(q["doc"] == "Look");
    CHECK(q["keyable"] == true);
    cmd("undo");
    CHECK(query("Ctrl.mode")["enum"] == json::array({"a", "b"}));

    cmd("addAttr", {{"args", {"Ctrl"}}, {"longName", "gain"}, {"minValue", 0.0}});
    aa::EditPopover range({idOf("Ctrl")}, "gain");
    CHECK(range.findChild<QLineEdit*>("min")->text() == "0");
    range.findChild<QLineEdit*>("min")->clear();
    range.findChild<QLineEdit*>("softMax")->setText("5");
    CHECK(range.apply());
    q = query("Ctrl.gain");
    CHECK_FALSE(q.contains("min"));
    CHECK(q["softMax"] == 5.0);
    aa::EditPopover bad({idOf("Ctrl")}, "gain");
    bad.findChild<QLineEdit*>("name")->setText("translateX");
    CHECK_FALSE(bad.apply());
}

TEST_CASE("Attribute Editor: search filters rows and sections, added attributes get edit and add buttons")
{
    cmd("newScene");
    cmd("spaceLocator", {{"name", "Prop"}});
    cmd("addAttr", {{"args", {"Prop"}}, {"longName", "maxHealth"}, {"category", "Health"}});
    cmd("addAttr", {{"args", {"Prop"}}, {"longName", "armour"}, {"doc", "Damage soaked"}});
    cmd("select", {{"args", {"Prop"}}});
    ui::AttributeEditor ae;
    ae.resize(400, 900);
    ae.show();
    settle();
    CHECK(ae.findChild<QToolButton*>("attrAdd")->isEnabled());
    CHECK(ae.findChild<QToolButton*>("extraAdd"));
    CHECK(ae.findChild<QToolButton*>("headingAdd"));
    CHECK(ae.findChildren<QToolButton*>("editAttr").size() == 2);
    REQUIRE(ae.findChild<QLabel*>("componentHeading"));
    CHECK(ae.findChild<QLabel*>("componentHeading")->text() == "Health");

    auto rowVisible = [&](const QString& text) {
        for (QLabel* l : ae.findChildren<QLabel*>())
            if (l->text() == text)
                if (auto* form = qobject_cast<QFormLayout*>(l->parentWidget()->layout()))
                    return form->isRowVisible(l);
        return false;
    };
    CHECK(rowVisible("Max Health"));
    ae.findChild<QLineEdit*>("attrSearch")->setText("max health");
    settle();
    CHECK(rowVisible("Max Health"));
    CHECK_FALSE(rowVisible("Armour"));
    CHECK_FALSE(rowVisible("Translate"));
    ae.findChild<QLineEdit*>("attrSearch")->setText("soaked");
    settle();
    CHECK(rowVisible("Armour"));
    CHECK_FALSE(rowVisible("Max Health"));
    ae.findChild<QLineEdit*>("attrSearch")->clear();
    settle();
    CHECK(rowVisible("Translate"));

    // A rename through the command rebuilds the editor with the new label.
    cmd("renameAttr", {{"args", {"Prop.armour", "shield"}}});
    settle();
    CHECK(rowVisible("Shield"));
}
