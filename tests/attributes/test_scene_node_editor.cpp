#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/NodeEditor.h"

#include <QApplication>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsView>

using namespace lila;

namespace sceneNodeEditorTests
{
json sneCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }

void snePump()
{
    for (int i = 0; i < 4; ++i)
    {
        mainq::drain();
        QApplication::processEvents();
    }
}

/** Nodes on the canvas: the movable, selectable items that aren't wires. */
int canvasNodes(QGraphicsView* view)
{
    int n = 0;
    for (QGraphicsItem* i : view->scene()->items())
        if ((i->flags() & QGraphicsItem::ItemIsMovable) && (i->flags() & QGraphicsItem::ItemIsSelectable) && !dynamic_cast<QGraphicsPathItem*>(i))
            ++n;
    return n;
}

/** Wires: path items behind the nodes. */
int canvasWires(QGraphicsView* view)
{
    int n = 0;
    for (QGraphicsItem* i : view->scene()->items())
        if (auto* p = dynamic_cast<QGraphicsPathItem*>(i); p && p->zValue() < -0.5 && p->isVisible())
            ++n;
    return n;
}
} // namespace sceneNodeEditorTests
using namespace sceneNodeEditorTests;

TEST_CASE("Scene node editor: the selection's nodes and wires, rebuilt as connections and the selection change")
{
    sneCmd("newScene");
    for (const char* n : {"A", "B", "C"})
        sneCmd("spaceLocator", {{"name", n}});
    sneCmd("connectAttr", {{"args", {"A.translateX", "B.translateY"}}});
    sneCmd("connectAttr", {{"args", {"B.translate", "C.rotate"}}});
    sneCmd("select", {{"args", {"B"}}});

    QWidget host;
    auto* editor = new ui::NodeEditor(&host, ui::kSceneGraphBackend);
    host.resize(1000, 700);
    host.show();
    snePump();
    REQUIRE(editor->backend().id() == ui::kSceneGraphBackend);
    CHECK(editor->group() == "Selection");
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    CHECK(canvasNodes(view) == 3);
    CHECK(canvasWires(view) == 2);

    sneCmd("disconnectAttr", {{"args", {"B.translateY"}}});
    snePump();
    CHECK(canvasNodes(view) == 2);
    CHECK(canvasWires(view) == 1);
    sneCmd("undo");
    snePump();
    CHECK(canvasNodes(view) == 3);

    sneCmd("select", {{"args", {"C"}}});
    snePump();
    CHECK(canvasNodes(view) == 2);
    CHECK(canvasWires(view) == 1);

    editor->setGroup("Upstream");
    snePump();
    CHECK(canvasNodes(view) == 3);
    CHECK(canvasWires(view) == 2);
}
