#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "gn/Gn.h"
#include "retopo/RetopoCommands.h"
#include "scene/Modifiers.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "sculpt/SculptSession.h"
#include "ui/Actions.h"
#include "ui/AdjustLastOp.h"
#include "ui/AgentSetup.h"
#include "ui/Animation.h"
#include "ui/AutoBevelUi.h"
#include "ui/Editors.h"
#include "ui/MaterialEditor.h"
#include "ui/MeasureTool.h"
#include "ui/ModelingToolkit.h"
#include "ui/ModifierStack.h"
#include "ui/NodeEditor.h"
#include "ui/Panels.h"
#include "ui/RetopoDrawTool.h"
#include "ui/RigViewport.h"
#include "ui/SculptPanel.h"
#include "ui/Shelf.h"
#include "ui/Tools.h"
#include "ui/UvEditor.h"
#include "uv/UvCommands.h"

#include <string>

using namespace lila;

// The main window, hotbox and layout menu register theirs when they are built; Actions::add warns about those at startup.
TEST_CASE("every registered action has a doc")
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    gn::registerTypes();
    scene::registerCommands();
    gn::registerCommands();
    uv::registerCommands();
    retopo::registerCommands();
    sculpt::registerCommands();

    ui::actions::registerBuiltins();
    ui::tools::registerActions();
    ui::registerShelfActions();
    ui::registerEditorActions();
    ui::registerPanelActions();
    ui::registerMaterialUi();
    ui::registerNodeEditorActions();
    ui::registerUvActions();
    ui::registerModelingToolkitActions();
    ui::registerModifierStackActions();
    ui::adjustLastOp::install();
    ui::registerRetopoActions();
    ui::registerSculptUi();
    ui::registerAnimationUi();
    ui::agentSetup::registerActions();
    ui::registerRigViewport();
    ui::registerAutoBevelUi();
    ui::registerMeasureUi();

    std::vector<const ui::Action*> all = ui::actions::all();
    REQUIRE(all.size() > 100);
    std::string missing;
    for (const ui::Action* a : all)
        if (a->doc.empty())
            missing += a->id + "\n";
    CHECK_MESSAGE(missing.empty(), "actions without a doc:\n" << missing);

    std::string unknown;
    for (const std::string& shelf : ui::shelves::registry().idsInOrder())
        for (const nlohmann::json& item : ui::shelves::registry().find(shelf)->items)
            if (item["target"].is_string() && !ui::actions::find(item["target"].get<std::string>()))
                unknown += shelf + ": " + item["target"].get<std::string>() + "\n";
    CHECK(ui::shelves::registry().find("Rigging"));
    CHECK_MESSAGE(unknown.empty(), "built-in shelf buttons without an action:\n" << unknown);

    // Every modifier kind gets an add action whose doc is the kind's own.
    for (const std::string& kind : scene::modifiers::kinds())
    {
        CAPTURE(kind);
        const ui::Action* a = ui::actions::find("modifier." + kind);
        REQUIRE(a);
        CHECK(a->doc == scene::modifiers::kindDoc(kind));
    }

    std::string tip = ui::actions::tooltip("modifier.bevel");
    CHECK(tip.starts_with("Bevel"));
    CHECK(tip.ends_with("\n" + scene::modifiers::kindDoc("bevel")));
}

TEST_CASE("repeat last runs the last edit again, skipping selection, views, tools, windows and undo")
{
    ui::actions::registerBuiltins();
    using json = nlohmann::json;
    CHECK(ui::actions::repeatable(json("mesh.extrude")));
    CHECK(ui::actions::repeatable(json{{"command", "polyExtrude"}}));
    CHECK(ui::actions::repeatable(json{{"python", "print(1)"}}));
    for (const char* id : {"select.grow", "edit.undo", "edit.redo", "edit.repeatLast", "edit.deselect", "viewport.frameAll", "tool.move",
                           "window.graphEditor", "hotbox.show", "snap.gridOn", "softSelect.keyUp", "shelf.floatingRelease", "file.save",
                           "uv.vertexMode", "uv.islandMode", "uv.selectStacks", "rig.dominantInfluenceSelect", "mode.next"})
    {
        CAPTURE(id);
        CHECK_FALSE(ui::actions::repeatable(json(id)));
    }
    CHECK_FALSE(ui::actions::repeatable(json{{"command", "select"}}));
    CHECK_FALSE(ui::actions::repeatable(json{{"command", "undo"}}));

    int edits = 0, selects = 0;
    ui::actions::add({.id = "test.repeatEdit", .label = "Test Edit", .category = "Test", .run = [&] { ++edits; }, .doc = "test"});
    ui::actions::add({.id = "select.testRepeat", .label = "Test Select", .category = "Test", .run = [&] { ++selects; }, .doc = "test"});
    ui::actions::run(json("test.repeatEdit"));
    ui::actions::run(json("select.testRepeat"));
    REQUIRE(ui::actions::repeatTarget());
    CHECK(*ui::actions::repeatTarget() == json("test.repeatEdit"));
    CHECK(ui::actions::displayLabel("edit.repeatLast") == "Repeat Test Edit");
    CHECK(ui::actions::run(json("edit.repeatLast")));
    CHECK(edits == 2);
    CHECK(selects == 1);
    // Repeating doesn't make Repeat Last the thing to repeat, or a recent command.
    CHECK(*ui::actions::repeatTarget() == json("test.repeatEdit"));
    for (const json& r : ui::actions::recent())
        CHECK(r != json("edit.repeatLast"));
}
