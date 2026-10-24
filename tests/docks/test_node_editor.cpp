#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "gn/Clipboard.h"
#include "gn/Gn.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/GraphEditor.h"
#include "ui/Hotkeys.h"
#include "ui/NodeEditor.h"

#include <QApplication>
#include <QClipboard>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QMainWindow>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

void pump()
{
    for (int i = 0; i < 4; ++i)
    {
        mainq::drain();
        QApplication::processEvents();
    }
}

/** A key press as a user types it: to the focus widget, through the app-wide hotkey filter. */
void press(QWidget* w, Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent down(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, key, mods);
    QApplication::sendEvent(w, &up);
    pump();
}

size_t nodeCount(const char* group) { return gn::readGroup(*gn::findGroupNode(scene::Graph::current(), group)).nodes.size(); }

void selectNode(QGraphicsView* view, const std::string& name)
{
    view->scene()->clearSelection();
    // Items keep their node name internally, so match by position.
    gn::Group g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    const gn::GNode* n = g.node(name);
    REQUIRE(n);
    for (QGraphicsItem* i : view->scene()->items())
        if ((i->flags() & QGraphicsItem::ItemIsSelectable) && QLineF(i->pos(), QPointF(n->position.x, n->position.y)).length() < 0.5)
            i->setSelected(true);
}
} // namespace

TEST_CASE("node editor keys act on the graph, not the scene selection")
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    gn::registerTypes();
    gn::registerCommands();
    ui::actions::registerBuiltins();
    ui::registerNodeEditorActions();
    ui::Hotkeys::get().install();

    run("newScene");
    run("spaceLocator", {{"name", "keepMe"}});
    run("select", {{"args", {"keepMe"}}});
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnAddNode", {{"args", {"G", "Value"}}, {"position", {0, 300}}});

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win);
    win.setCentralWidget(editor);
    win.resize(900, 600);
    win.show();
    win.activateWindow();
    editor->setGroup("G");
    pump();
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    view->setFocus();
    pump();
    REQUIRE(QApplication::focusWidget() == view);
    REQUIRE(nodeCount("G") == 3);

    selectNode(view, "Value");
    press(view, Qt::Key_Delete);
    CHECK(nodeCount("G") == 2);
    CHECK(scene::Graph::current().find("keepMe")); // edit.delete didn't run
    run("undo");
    pump();
    CHECK(nodeCount("G") == 3);

    // Copy as text, paste it back as a new node.
    selectNode(view, "Value");
    press(view, Qt::Key_C, Qt::ControlModifier);
    QString text = QApplication::clipboard()->text();
    CHECK(text.contains(gn::kClipboardFormat));
    press(view, Qt::Key_V, Qt::ControlModifier);
    CHECK(nodeCount("G") == 4);

    // Text that isn't node data changes nothing.
    QApplication::clipboard()->setText("just some words");
    press(view, Qt::Key_V, Qt::ControlModifier);
    CHECK(nodeCount("G") == 4);

    selectNode(view, "Value");
    press(view, Qt::Key_D, Qt::ControlModifier);
    CHECK(nodeCount("G") == 5);

    // Tab opens the add-node search rather than moving focus.
    press(view, Qt::Key_Tab);
    QWidget* popup = QApplication::activePopupWidget();
    CHECK(popup);
    if (popup)
        popup->close();
    pump();
}

namespace
{
/** Geometry nodes under another id, as an extension's backend would sit beside it. */
class AliasBackend final : public ui::GraphBackend
{
public:
    std::string id() const override { return "tstAlias"; }
    QString title() const override { return "Alias"; }
    std::string commandPrefix() const override { return "gn"; }
    std::vector<std::string> groupNames(scene::Graph& g) const override { return gn::groupNames(g); }
    bool isGroupNode(const scene::Node& n) const override { return n.type().name == gn::kGroupNodeType; }
};
} // namespace

TEST_CASE("a node editor opens on a registered backend and falls back when it goes")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnAddNode", {{"args", {"G", "Value"}}, {"position", {0, 300}}});
    ui::graphBackends::add(std::make_shared<AliasBackend>(), "tstOwner");

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, "tstAlias");
    win.setCentralWidget(editor);
    win.resize(900, 600);
    win.show();
    editor->setGroup("G");
    pump();
    CHECK(editor->backend().id() == "tstAlias");
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    view->setFocus();
    pump();
    selectNode(view, "Value");
    press(view, Qt::Key_Delete);
    CHECK(nodeCount("G") == 2);

    ui::graphBackends::remove("tstAlias");
    pump();
    CHECK(editor->backend().id() == ui::kGeometryNodesBackend);
    CHECK_FALSE(editor->setBackend("tstAlias"));
}

TEST_CASE("graph editor Delete stays in the graph editor")
{
    ui::registerAnimationUi();
    run("newScene");
    run("spaceLocator", {{"name", "keepMe"}});
    run("select", {{"args", {"keepMe"}}});

    QMainWindow win;
    auto* editor = new ui::GraphEditor(&win);
    win.setCentralWidget(editor);
    win.resize(900, 600);
    win.show();
    win.activateWindow();
    pump();
    QWidget* curves = editor->findChild<QWidget*>("curveView");
    REQUIRE(curves);
    curves->setFocus();
    pump();
    REQUIRE(QApplication::focusWidget() == curves);
    press(curves, Qt::Key_Delete);
    press(curves, Qt::Key_Backspace);
    CHECK(scene::Graph::current().find("keepMe"));
}
