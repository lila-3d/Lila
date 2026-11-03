#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "curve/CurveNodes.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/CurveUi.h"
#include "ui/LevelToolkit.h"
#include "ui/ViewportTool.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

using namespace lila;

TEST_CASE("pipe route, supports and channel actions: registered, documented, on the Level toolkit, with option boxes")
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    curve::registerTypes();
    curve::registerCommands();
    ui::registerCurveUi();

    for (const char* id : {"curve.pipeRouteTool", "curve.pipeSupports", "curve.carveChannel"})
    {
        INFO(id);
        const ui::Action* a = ui::actions::find(id);
        REQUIRE(a);
        CHECK(!a->doc.empty());
        CHECK(a->category == "Curves");
        if (!a->command.empty())
            CHECK(ops::find(a->command));
    }
    CHECK(ui::viewportTools::registry().find("curve.pipeRouteTool"));

    const ui::levelToolkit::Section* s = ui::levelToolkit::registry().find("curves.pipes");
    REQUIRE(s);
    for (const ui::levelToolkit::Item& item : s->items)
        CHECK(ui::actions::find(item.action));

    // The option boxes hide what the tools and selection supply.
    for (auto [cmd, flag] : std::initializer_list<std::pair<const char*, const char*>>{
             {"pipeRoute", "start"}, {"pipeRoute", "obstacles"}, {"pipeSupports", "surfaces"}, {"carveChannel", "points"}})
    {
        INFO(cmd << " " << flag);
        const ops::options::Override* o = ops::options::findOverride(cmd);
        REQUIRE(o);
        CHECK(std::ranges::find(o->hidden, std::string(flag)) != o->hidden.end());
    }
}
