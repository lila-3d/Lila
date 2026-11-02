#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/LightingChannels.h"
#include "ui/RelationshipsPanel.h"

#include <QApplication>
#include <QLineEdit>
#include <QTableWidget>
#include <QToolButton>

using namespace lila;
using namespace lila::scene;

namespace relationshipTests
{
json rCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
void rSettle()
{
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
}
int columnOf(QTableWidget* t, const QString& name)
{
    for (int c = 0; c < t->columnCount(); ++c)
        if (t->horizontalHeaderItem(c)->text() == name)
            return c;
    return -1;
}
} // namespace relationshipTests
using namespace relationshipTests;

TEST_CASE("Relationships: channels, sets and layers as checkable columns for the selection")
{
    rCmd("newScene");
    rCmd("spaceLocator", {{"name", "Lamp"}});
    rCmd("spaceLocator", {{"name", "Rock"}});
    rCmd("createSet", {{"name", "Props"}, {"empty", true}});
    rCmd("lightingChannel", {{"args", {"night"}}, {"create", true}});
    rCmd("select", {{"args", {"Lamp", "Rock"}}});
    ui::RelationshipsPanel panel;
    panel.show();
    rSettle();
    auto* table = panel.findChild<QTableWidget*>("relTable");
    REQUIRE(table->rowCount() == 2);
    const int night = columnOf(table, "night");
    const int def = columnOf(table, "default");
    REQUIRE(night >= 0);
    REQUIRE(def >= 0);
    CHECK(table->item(0, def)->checkState() == Qt::Checked);
    CHECK(table->item(0, night)->checkState() == Qt::Unchecked);

    // Ticking writes through the command; the table follows.
    table->item(1, night)->setCheckState(Qt::Checked); // row 1 is Lamp: the lead (Rock) comes first
    rSettle();
    CHECK(lightingChannels::of(*Graph::current().find("Lamp")) == std::vector<std::string>{"default", "night"});
    const int props = columnOf(table, "Props");
    if (props >= 0)
    {
        table->item(0, props)->setCheckState(Qt::Checked);
        rSettle();
        CHECK(rCmd("setMembers", {{"args", {"Props"}}, {"nodes", true}}).size() == 1);
    }

    panel.findChild<QLineEdit*>("relSearch")->setText("nig");
    rSettle();
    CHECK(table->columnCount() == 1);
    panel.findChild<QLineEdit*>("relSearch")->setText("dusk");
    panel.findChild<QToolButton*>("relNewChannel")->click();
    rSettle();
    CHECK(lightingChannels::of(*Graph::current().find("Rock")) == std::vector<std::string>{"default", "dusk"});
}
