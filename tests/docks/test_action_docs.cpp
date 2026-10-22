#include <doctest/doctest.h>

#include "cmds/Command.h"
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
#include "ui/QuadDrawTool.h"
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
    cmds::registerBuiltins();
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
