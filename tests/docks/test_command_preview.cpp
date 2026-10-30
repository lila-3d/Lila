#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/SceneCommands.h"
#include "ui/CommandPreview.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
scene::NodeId cubeWithEdge()
{
    static bool done = false;
    if (!done)
    {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        done = true;
    }
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), "cube", scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube())));
    g.select({n->id()});
    scene::components::setMode(scene::ComponentMode::Edge);
    scene::components::select(n->id(), scene::ComponentMode::Edge, {0});
    g.evaluate();
    return n->id();
}
} // namespace

TEST_CASE("A command preview shows what the command would make and leaves the scene and undo stack as they were")
{
    scene::NodeId cube = cubeWithEdge();
    scene::Graph& g = scene::Graph::current();
    const scene::MeshPtr before = g.node(cube)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    const size_t steps = UndoStack::get().undoCount();
    int executes = 0;
    const int listener = ops::addExecuteListener([&](const ExecuteEvent&) { ++executes; });

    CHECK(ui::commandPreview::show(json{{"command", "polyBevel"}, {"args", json::object()}}));
    CHECK(ui::commandPreview::showing());
    CHECK(executes == 0);
    CHECK(UndoStack::get().undoCount() == steps);
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
    CHECK(scene::components::selected(cube, scene::ComponentMode::Edge) == scene::components::Ids{0});
    g.evaluate();
    const scene::MeshPtr after = g.node(cube)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    CHECK(after->faceCapacity() == before->faceCapacity());

    ui::commandPreview::clear();
    CHECK_FALSE(ui::commandPreview::showing());
    ops::removeExecuteListener(listener);
}

TEST_CASE("Commands that aren't adjustable are not previewed")
{
    cubeWithEdge();
    CHECK_FALSE(ui::commandPreview::show(json{{"command", "delete"}}));
    CHECK_FALSE(ui::commandPreview::showing());
}

TEST_CASE("No preview while the selected meshes have more faces than the limit")
{
    cubeWithEdge();
    PrefStore::get().set("general", ui::commandPreview::kMaxFacesPref, 5.0);
    CHECK_FALSE(ui::commandPreview::show(json{{"command", "polyBevel"}, {"args", json::object()}}));
    PrefStore::get().set("general", ui::commandPreview::kMaxFacesPref, 6.0);
    CHECK(ui::commandPreview::show(json{{"command", "polyBevel"}, {"args", json::object()}}));
    ui::commandPreview::clear();
    PrefStore::get().set("general", ui::commandPreview::kMaxFacesPref, ui::commandPreview::kMaxFacesDefault);
}
