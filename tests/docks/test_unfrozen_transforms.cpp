#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "ops/Command.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "scene/Graph.h"
#include "ui/UnfrozenTransforms.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QTreeWidget>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

QStringList rows(QTreeWidget* tree)
{
    QStringList out;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        out << tree->topLevelItem(i)->text(0);
    out.sort();
    return out;
}

std::vector<std::string> selection()
{
    std::vector<std::string> out;
    for (const json& n : run("ls", {{"selection", true}}))
        out.push_back(n);
    return out;
}
} // namespace

TEST_CASE("unfrozen transforms panel: filters update the list and picking rows selects without changing it")
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();

    run("newScene");
    run("polyCube", {{"name", "Frozen"}});
    run("polyCube", {{"name", "Quarter"}});
    run("polyCube", {{"name", "Tilted"}});
    run("rotate", {{"args", {"Quarter"}}, {"by", {0, 90, 0}}});
    run("rotate", {{"args", {"Tilted"}}, {"by", {0, 37.5, 0}}});
    run("select", {{"args", {"Frozen", "Quarter", "Tilted"}}});

    ui::UnfrozenTransformsPanel panel;
    panel.show();
    panel.loadSelection();
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE(tree);
    CHECK(rows(tree) == QStringList{"Quarter", "Tilted"});

    // Picking a row selects that object alone, and the list stays.
    tree->clearSelection();
    tree->topLevelItem(0)->setSelected(true);
    QApplication::processEvents();
    CHECK(selection() == std::vector<std::string>{tree->topLevelItem(0)->text(0).toStdString()});
    CHECK(rows(tree) == QStringList{"Quarter", "Tilted"});

    QComboBox* step = nullptr;
    for (QComboBox* c : panel.findChildren<QComboBox*>())
        if (c->itemText(0) == "Off")
            step = c;
    REQUIRE(step);
    step->setCurrentIndex(step->findData(90.0));
    CHECK(rows(tree) == QStringList{"Tilted"});
    QCheckBox* invert = nullptr;
    for (QCheckBox* c : panel.findChildren<QCheckBox*>())
        if (c->text() == "Invert")
            invert = c;
    REQUIRE(invert);
    invert->setChecked(true);
    CHECK(rows(tree) == QStringList{"Quarter"});

    // Freeze clears the picked row off the list.
    tree->topLevelItem(0)->setSelected(true);
    QApplication::processEvents();
    for (QPushButton* b : panel.findChildren<QPushButton*>())
        if (b->text() == "Freeze")
            b->click();
    CHECK(tree->topLevelItemCount() == 1);
    CHECK(!(tree->topLevelItem(0)->flags() & Qt::ItemIsSelectable)); // the All frozen placeholder
}
