#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/DisplayLayersPanel.h"
#include "ui/MaterialEditor.h"
#include "ui/SceneDialogs.h"

#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTreeWidget>

using namespace lila;
using json = nlohmann::json;

namespace
{
void pump()
{
    for (int i = 0; i < 4; ++i)
    {
        mainq::drain();
        QApplication::processEvents();
    }
}

void registerAll()
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    ui::registerMaterialUi();
}

void pressOutside(QWidget* dialog)
{
    QWidget other;
    other.setGeometry(dialog->geometry().right() + 40, dialog->geometry().bottom() + 40, 50, 50);
    other.show();
    const QPointF global = QPointF(other.geometry().center());
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10), global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&other, &press);
    pump();
}
} // namespace

TEST_CASE("welcome dialog shows two rows of three examples without scrolling and ignores clicks outside")
{
    QPointer<ui::StartupDialog> welcome = new ui::StartupDialog({}, true, nullptr);
    auto* examples = welcome->findChild<QListWidget*>();
    REQUIRE(examples);
    examples->clear();
    for (int i = 0; i < 6; ++i)
        new QListWidgetItem(QString("Example %1").arg(i), examples);
    welcome->present();
    pump();
    CHECK_FALSE(examples->verticalScrollBar()->isVisible());
    CHECK_FALSE(examples->horizontalScrollBar()->isVisible());
    const QRect first = examples->visualItemRect(examples->item(0));
    CHECK(examples->visualItemRect(examples->item(2)).top() == first.top());
    CHECK(examples->visualItemRect(examples->item(3)).top() > first.top());
    CHECK(examples->viewport()->rect().contains(examples->visualItemRect(examples->item(5))));

    new QListWidgetItem("Example 6", examples);
    examples->doItemsLayout();
    pump();
    CHECK(examples->verticalScrollBar()->isVisible());

    pressOutside(welcome);
    REQUIRE(welcome);
    CHECK(welcome->isVisible());
    welcome->reject();
    pump();

    QPointer<ui::StartupDialog> recent = new ui::StartupDialog({}, false, nullptr);
    recent->present();
    pump();
    pressOutside(recent);
    CHECK((!recent || !recent->isVisible()));
    delete welcome.data();
    delete recent.data();
}

TEST_CASE("material editor filters by name and shows icon-only buttons in order")
{
    registerAll();
    ops::execute("newScene", json::object());
    for (const char* name : {"M_Brick", "M_Wood", "M_RedBRICK"})
        ops::execute("createMaterial", {{"args", {name}}});
    ui::MaterialEditor editor;
    editor.show();
    pump();
    auto* list = editor.findChild<QListWidget*>();
    QLineEdit* filter = nullptr;
    for (QLineEdit* e : editor.findChildren<QLineEdit*>())
        if (e->placeholderText() == "Filter")
            filter = e;
    REQUIRE(list);
    REQUIRE(filter);
    auto shown = [&] {
        QStringList out;
        for (int i = 0; i < list->count(); ++i)
            if (!list->item(i)->isHidden())
                out << list->item(i)->text();
        return out;
    };
    const qsizetype all = shown().size();
    filter->setText("brick");
    CHECK(shown() == QStringList{"M_Brick", "M_RedBRICK"});
    ops::execute("createMaterial", {{"args", {"M_Stone"}}});
    pump();
    CHECK(shown() == QStringList{"M_Brick", "M_RedBRICK"});
    filter->clear();
    CHECK(shown().size() == all + 1);

    QStringList tips;
    for (QPushButton* b : editor.findChildren<QPushButton*>(Qt::FindDirectChildrenOnly))
        if (b->text().isEmpty() && !b->icon().isNull())
            tips << b->toolTip().section(' ', 0, 0).remove(':');
    CHECK(tips.mid(0, 4) == QStringList{"Assign", "New", "Delete", "Select"});
}

TEST_CASE("display layers panel marks empty layers")
{
    registerAll();
    ops::execute("newScene", json::object());
    ops::execute("createDisplayLayer", {{"empty", true}});
    ui::DisplayLayersPanel panel;
    panel.show();
    pump();
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE(tree);
    REQUIRE(tree->topLevelItemCount() == 1);
    const int last = tree->columnCount() - 1;
    CHECK_FALSE(tree->topLevelItem(0)->icon(last).isNull());
    CHECK(tree->topLevelItem(0)->text(1).isEmpty());
}
