#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/AttrComponents.h"
#include "scene/Graph.h"
#include "core/PrefStore.h"
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
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>

#include <set>

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

TEST_CASE("Extra Attributes: schemas as indented headings, no schema list, hover keeps fields still")
{
    cmd("newScene");
    cmd("spaceLocator", {{"name", "Prim"}});
    cmd("addAttr", {{"args", {"Prim"}}, {"longName", "usdApiSchemas"}, {"dataType", "stringArray"}});
    cmd("setAttr", {{"args", {"Prim.usdApiSchemas", json::array({"SpeedAPI", "EmptyAPI"})}}});
    cmd("addAttr", {{"args", {"Prim"}}, {"longName", "speed"}, {"category", "SpeedAPI"}});
    cmd("select", {{"args", {"Prim"}}});
    ui::AttributeEditor ae;
    ae.resize(400, 900);
    ae.show();
    settle();

    QStringList headings;
    for (QLabel* l : ae.findChildren<QLabel*>("componentHeading"))
        headings << l->text();
    CHECK(headings == QStringList{"SpeedAPI", "EmptyAPI"});
    for (QLabel* l : ae.findChildren<QLabel*>())
        CHECK(l->text() != "Usd Api Schemas");

    QToolButton* pencil = ae.findChild<QToolButton*>("editAttr");
    REQUIRE(pencil);
    std::vector<QRect> before;
    for (QWidget* w : ae.findChildren<QLineEdit*>())
        before.push_back(w->geometry());
    QEvent enter(QEvent::Enter);
    QApplication::sendEvent(pencil->parentWidget(), &enter);
    settle();
    CHECK(pencil->isVisible());
    std::vector<QRect> after;
    for (QWidget* w : ae.findChildren<QLineEdit*>())
        after.push_back(w->geometry());
    CHECK(after == before);
}


void mouse(QWidget* w, QEvent::Type type, QPoint at, Qt::MouseButton button = Qt::LeftButton)
{
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
    QMouseEvent e(type, QPointF(at), QPointF(w->mapToGlobal(at)), type == QEvent::MouseMove ? Qt::NoButton : button, held, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

TEST_CASE("Attribute Editor: one label edge, dragged and reset; number fields drag and click to type")
{
    cmd("newScene");
    cmd("spaceLocator", {{"name", "Edge"}});
    cmd("addAttr", {{"args", {"Edge"}}, {"longName", "gain"}, {"softMinValue", 0.0}, {"softMaxValue", 10.0}, {"defaultValue", 5.0}});
    cmd("select", {{"args", {"Edge"}}});
    PrefStore::get().remove("attributeEditor", "/labelFraction");
    ui::AttributeEditor ae;
    ae.resize(420, 900);
    ae.show();
    settle();

    QWidget* body = ae.findChild<QWidget*>("aeBody");
    REQUIRE(body);
    auto edges = [&] {
        std::set<int> out;
        for (QLabel* l : body->findChildren<QLabel*>())
            if (QFormLayout* form = l->parentWidget() ? qobject_cast<QFormLayout*>(l->parentWidget()->layout()) : nullptr)
                if (int row = -1; (form->getWidgetPosition(l, &row, nullptr), row >= 0) && form->itemAt(row, QFormLayout::LabelRole) &&
                                  form->itemAt(row, QFormLayout::LabelRole)->widget() == l && l->isVisibleTo(body))
                    out.insert(l->mapTo(body, QPoint(l->width(), 0)).x());
        return out;
    };
    REQUIRE(edges().size() == 1);
    const int autoEdge = *edges().begin();

    QLabel* gain = nullptr;
    for (QLabel* l : body->findChildren<QLabel*>())
        if (l->text() == "Gain")
            gain = l;
    REQUIRE(gain);
    const QPoint grab(gain->width() - 2, gain->height() / 2);
    mouse(gain, QEvent::MouseButtonPress, grab);
    mouse(gain, QEvent::MouseMove, grab + QPoint(40, 0));
    mouse(gain, QEvent::MouseButtonRelease, grab + QPoint(40, 0));
    settle();
    CHECK(edges().size() == 1);
    CHECK(*edges().begin() == autoEdge + 40);
    CHECK(PrefStore::get().value("attributeEditor", "/labelFraction", 0.0) > 0.0);
    mouse(gain, QEvent::MouseButtonDblClick, QPoint(gain->width() - 2, gain->height() / 2));
    settle();
    CHECK(*edges().begin() == autoEdge);
    CHECK(PrefStore::get().value("attributeEditor", "/labelFraction", 0.0) == 0.0);

    QLineEdit* field = nullptr;
    for (QLineEdit* e : body->findChildren<QLineEdit*>())
        if (e->text() == "5")
            field = e;
    REQUIRE(field);
    // A click types: focus with the value selected, nothing changed.
    mouse(field, QEvent::MouseButtonPress, field->rect().center());
    mouse(field, QEvent::MouseButtonRelease, field->rect().center());
    settle();
    CHECK(field->hasFocus());
    CHECK(field->selectedText() == "5");
    field->clearFocus();
    // A drag across half the field steps half the 0 to 10 range.
    const QPoint at = field->rect().center();
    mouse(field, QEvent::MouseButtonPress, at);
    for (int dx = 4; dx <= field->width() / 2; dx += 4)
        mouse(field, QEvent::MouseMove, at + QPoint(dx, 0));
    mouse(field, QEvent::MouseButtonRelease, at + QPoint(field->width() / 2, 0));
    settle();
    CHECK(cmd("getAttr", {{"args", {"Edge.gain"}}}).get<double>() == doctest::Approx(10.0).epsilon(0.05));
    CHECK_FALSE(field->hasFocus());
}
