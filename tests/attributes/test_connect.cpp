#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/ConnectPopover.h"

#include <QListWidget>

using namespace lila;
using namespace lila::scene;

namespace connectTests
{
json cCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
QListWidgetItem* itemFor(QListWidget* list, const QString& plug)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->toolTip() == plug)
            return list->item(i);
    return nullptr;
}
} // namespace connectTests
using namespace connectTests;

TEST_CASE("Connect popover: pick pairs with only fitting inputs lit, match by name, swap")
{
    cCmd("newScene");
    cCmd("spaceLocator", {{"name", "A"}});
    cCmd("spaceLocator", {{"name", "B"}});
    cCmd("addAttr", {{"args", {"A"}}, {"longName", "gain"}, {"keyable", true}});
    cCmd("addAttr", {{"args", {"B"}}, {"longName", "gain"}, {"keyable", true}});
    Graph& g = Graph::current();
    ui::ConnectPopover pop(g.find("A")->id(), g.find("B")->id());
    auto* left = pop.findChild<QListWidget*>("connectLeft");
    auto* right = pop.findChild<QListWidget*>("connectRight");
    QListWidgetItem* tx = itemFor(left, "A.translateX");
    REQUIRE(tx);
    left->setCurrentItem(tx);
    QListWidgetItem* ry = itemFor(right, "B.rotateY");
    QListWidgetItem* translate = itemFor(right, "B.translate");
    REQUIRE(ry);
    REQUIRE(translate);
    CHECK((ry->flags() & Qt::ItemIsEnabled));
    CHECK_FALSE((translate->flags() & Qt::ItemIsEnabled)); // a scalar doesn't feed a whole vector
    CHECK(pop.wire("A.translateX", "B.rotateY"));
    CHECK(g.plugName(*g.source(g.plug("B.ry"))) == "A.translateX");

    CHECK(pop.matchByName() > 0);
    CHECK(g.plugName(*g.source(g.plug("B.gain"))) == "A.gain");
    CHECK(g.plugName(*g.source(g.plug("B.translate"))) == "A.translate");
    CHECK_FALSE(g.source(g.plug("B.rotate")));
    // An input already wired by hand stays as it was.
    CHECK(g.plugName(*g.source(g.plug("B.ry"))) == "A.translateX");

    pop.swap();
    CHECK(itemFor(pop.findChild<QListWidget*>("connectLeft"), "B.translateX"));
}
