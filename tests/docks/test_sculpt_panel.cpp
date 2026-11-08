#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/MainQueue.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/SceneCommands.h"
#include "sculpt/SculptSession.h"
#include "ui/Actions.h"
#include "ui/SculptPanel.h"
#include "ui/SculptTool.h"

#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
template <class T> T* panelChild(QWidget* w, const char* name)
{
    T* c = w->findChild<T*>(name);
    REQUIRE(c);
    return c;
}
} // namespace

TEST_CASE("sculpt panel: level row follows the session and its buttons step levels")
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    sculpt::registerCommands();
    ui::actions::registerBuiltins();
    ui::registerSculptUi();
    ops::execute("newScene", json::object());

    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), "Rock");
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube({}))));
    ops::execute("select", {{"args", {"Rock"}}});

    ui::SculptPanel panel;
    panel.resize(360, 900);
    panel.show();
    pump();
    auto* status = panelChild<QLabel>(&panel, "sculptStatus");
    auto* row = panelChild<QWidget>(&panel, "sculptLevels");
    CHECK(status->text() == "Rock");
    CHECK_FALSE(row->isVisible());

    ops::execute("sculptEnter", {{"args", {"Rock"}}, {"levels", 2}});
    pump();
    CHECK(row->isVisible());
    CHECK(panelChild<QLabel>(&panel, "sculptLevel")->text() == "Level 2 / 2");
    CHECK(status->text() == QString::fromUtf8("Rock  \u00b7  96 faces"));

    ui::actions::run(json("sculpt.levelDown"));
    pump();
    CHECK(panelChild<QLabel>(&panel, "sculptLevel")->text() == "Level 1 / 2");
    ui::actions::run(json("sculpt.addLevel"));
    pump();
    CHECK(panelChild<QLabel>(&panel, "sculptLevel")->text() == "Level 3 / 3");
    CHECK(sculpt::active()->sculpt.mesh().numFaces() == 384);

    ops::execute("sculptExit", json::object());
    pump();
    CHECK_FALSE(row->isVisible());
    CHECK(status->text() == QString::fromUtf8("Rock  \u00b7  level 3 of 3"));
}
