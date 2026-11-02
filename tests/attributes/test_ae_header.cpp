#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/AttrView.h"
#include "ui/AttributeEditor.h"

#include <QApplication>
#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

using namespace lila;
using namespace lila::scene;
namespace av = lila::ui::attrView;

namespace aeHeaderTests
{
json hCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
void hSettle()
{
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
}

/** The field widget beside the row labelled text. */
QWidget* fieldOf(QWidget& ae, const QString& text)
{
    for (QLabel* l : ae.findChildren<QLabel*>())
        if (l->text() == text && l->isVisibleTo(&ae))
            if (auto* form = qobject_cast<QFormLayout*>(l->parentWidget()->layout()))
            {
                int row = -1;
                QFormLayout::ItemRole role;
                form->getWidgetPosition(l, &row, &role);
                if (QLayoutItem* item = form->itemAt(row, QFormLayout::FieldRole))
                    return item->widget();
            }
    return nullptr;
}

void typeInto(QLineEdit* e, const QString& text)
{
    e->setText(text);
    e->setModified(true);
    Q_EMIT e->editingFinished();
    hSettle();
}

bool hasLabel(QWidget& ae, const QString& text)
{
    for (QLabel* l : ae.findChildren<QLabel*>())
        if (l->text() == text && l->isVisibleTo(&ae))
            return true;
    return false;
}
} // namespace aeHeaderTests
using namespace aeHeaderTests;

TEST_CASE("Multi-object edits: the lead only with a hint, every holder with Alt, a count badge and mixed values")
{
    hCmd("newScene");
    hCmd("spaceLocator", {{"name", "A"}});
    hCmd("spaceLocator", {{"name", "B"}});
    hCmd("setAttr", {{"args", {"B.tx", 5.0}}});
    hCmd("select", {{"args", {"A", "B"}}});
    av::setHintEnabled(true);
    ui::AttributeEditor ae;
    ae.resize(420, 900);
    ae.show();
    hSettle();
    REQUIRE(av::lead() == Graph::current().find("B")->id());
    CHECK(ae.findChild<QLineEdit*>()->text() == "B");

    QWidget* translate = fieldOf(ae, "Translate");
    REQUIRE(translate);
    QLineEdit* tx = translate->findChild<QLineEdit*>();
    typeInto(tx, "2");
    CHECK(hCmd("getAttr", {{"args", {"B.tx"}}}).get<double>() == doctest::Approx(2.0));
    CHECK(hCmd("getAttr", {{"args", {"A.tx"}}}).get<double>() == doctest::Approx(0.0));
    CHECK_FALSE(ae.findChild<QWidget*>("multiEditHint")->isHidden());
    CHECK(ae.findChild<QLabel*>("multiEditHintText")->text() == "Changed B only. Hold Alt to change all 2.");

    av::setMultiEditForced(true);
    hSettle();
    CHECK_FALSE(ae.findChild<QLabel*>("multiEditPill")->isHidden());
    CHECK(ae.findChild<QLabel*>("multiEditPill")->text() == "Editing 2 objects");
    tx = fieldOf(ae, "Translate")->findChild<QLineEdit*>();
    CHECK(tx->text() == "--");
    typeInto(tx, "7");
    CHECK(hCmd("getAttr", {{"args", {"A.tx"}}}).get<double>() == doctest::Approx(7.0));
    CHECK(hCmd("getAttr", {{"args", {"B.tx"}}}).get<double>() == doctest::Approx(7.0));
    bool badged = false;
    for (QLabel* b : ae.findChildren<QLabel*>("multiBadge"))
        badged |= !b->isHidden() && b->text() == QString::fromUtf8("×" "2");
    CHECK(badged);
    hCmd("undo");
    CHECK(hCmd("getAttr", {{"args", {"A.tx"}}}).get<double>() == doctest::Approx(0.0));
    CHECK(hCmd("getAttr", {{"args", {"B.tx"}}}).get<double>() == doctest::Approx(2.0));
    av::setMultiEditForced(false);
    hSettle();

    // Turning the hint off keeps it away.
    av::setHintEnabled(false);
    typeInto(fieldOf(ae, "Translate")->findChild<QLineEdit*>(), "3");
    CHECK(ae.findChild<QWidget*>("multiEditHint")->isHidden());
    av::setHintEnabled(true);
}

TEST_CASE("Header: lead chips, pin, related nodes with back and forward, name mode and section views")
{
    hCmd("newScene");
    hCmd("spaceLocator", {{"name", "A"}});
    hCmd("spaceLocator", {{"name", "B"}});
    hCmd("spaceLocator", {{"name", "Driver"}});
    hCmd("connectAttr", {{"args", {"Driver.tx", "A.ty"}}});
    hCmd("select", {{"args", {"A", "B"}}});
    ui::AttributeEditor ae;
    ae.resize(420, 900);
    ae.show();
    hSettle();
    auto name = [&] { return ae.findChild<QLineEdit*>()->text(); };
    CHECK(name() == "B");
    QList<QToolButton*> chips = ae.findChildren<QToolButton*>("selectionChip");
    REQUIRE(chips.size() == 2);
    CHECK(chips[0]->text() == "B");
    chips[1]->click(); // A becomes the lead
    hSettle();
    CHECK(name() == "A");
    CHECK(Graph::current().selection().size() == 2);
    CHECK(av::selectionLeadFirst().front() == Graph::current().find("A")->id());

    QToolButton* related = ae.findChild<QToolButton*>("relatedChip");
    REQUIRE(related);
    CHECK(related->text() == "Driver");
    related->click();
    hSettle();
    CHECK(name() == "Driver");
    ae.findChild<QToolButton*>("focusBack")->click();
    hSettle();
    CHECK(name() == "A");
    ae.findChild<QToolButton*>("focusForward")->click();
    hSettle();
    CHECK(name() == "Driver");

    ae.findChild<QToolButton*>("aePin")->click();
    hCmd("select", {{"args", {"B"}}});
    hSettle();
    CHECK(name() == "Driver");
    ae.findChild<QToolButton*>("aePin")->click();
    hSettle();
    CHECK(name() == "B");

    av::setNameMode(av::NameMode::Long);
    hSettle();
    CHECK(hasLabel(ae, "translate"));
    av::setNameMode(av::NameMode::Short);
    hSettle();
    CHECK(hasLabel(ae, "t"));
    av::setNameMode(av::NameMode::Nice);
    hSettle();
    CHECK(hasLabel(ae, "Translate"));

    // Hiding a section through the view toggles keeps it hidden for that type.
    const std::string type = Graph::current().find("B")->type().registered().name;
    QString title;
    for (QToolButton* b : ae.findChildren<QToolButton*>())
        if (b->text().endsWith(" Attributes") && b->isVisibleTo(&ae))
        {
            title = b->text();
            break;
        }
    REQUIRE(!title.isEmpty());
    av::setSectionShown(type, title, false);
    hSettle();
    bool visible = false;
    for (QToolButton* b : ae.findChildren<QToolButton*>())
        visible |= b->text() == title && b->isVisibleTo(&ae);
    CHECK_FALSE(visible);
    av::setSectionShown(type, title, true);
    av::setCurrentView("Default");
    CHECK(ae.findChild<QToolButton*>("sectionHelp"));
}

TEST_CASE("attrView: labels by mode, precision, saved views")
{
    AttrDef d{.name = "maxHealth", .shortName = "mh"};
    av::setNameMode(av::NameMode::Nice);
    CHECK(av::label(d) == "Max Health");
    d.niceName = "Max HP";
    CHECK(av::label(d) == "Max HP");
    av::setNameMode(av::NameMode::Short);
    CHECK(av::label(d) == "mh");
    av::setNameMode(av::NameMode::Nice);
    av::setPrecision(2);
    CHECK(av::formatNumber(1.23456) == "1.23");
    CHECK(av::formatNumber(2.0) == "2");
    av::setPrecision(3);
    av::setCurrentView("Rigging");
    CHECK(av::sectionShown("transform", "Transform Attributes"));
    CHECK_FALSE(av::sectionShown("transform", "Render Stats"));
    CHECK(av::saveView("MyView", false));
    bool found = false;
    for (const av::View& v : av::views())
        found |= v.name == "MyView" && v.source == "user";
    CHECK(found);
    av::setCurrentView("Default");
}
