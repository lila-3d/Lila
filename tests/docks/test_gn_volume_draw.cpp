#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "gn/Gn.h"
#include "ui/Overlay.h"
#include "scene/Graph.h"
#include "scene/SceneCommands.h"
#include "ui/GnViewerDraw.h"
#include "ui/ViewportHooks.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json gnRun(const char* name, json call = json::object()) { return ops::execute(name, call); }
json gnArgs(std::initializer_list<json> a) { return {{"args", json(a)}}; }
} // namespace

TEST_CASE("a geometry-nodes volume draws in the viewport inside its box")
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    gn::registerTypes();
    gn::registerCommands();
    ui::registerGnViewerDrawer();
    gnRun("newScene");
    const std::string sphere = gnRun("polySphere", {{"radius", 50.0}, {"name", "Vol"}})[0];
    gnRun("gnCreateGroup", gnArgs({"V"}));
    std::string sdf = gnRun("gnAddNode", gnArgs({"V", "MeshToSDF"})).get<std::string>();
    gnRun("gnDisconnect", gnArgs({"V", "GroupOutput.geometry"}));
    gnRun("gnConnect", gnArgs({"V", "GroupInput.geometry", sdf + ".mesh"}));
    gnRun("gnConnect", gnArgs({"V", sdf + ".volume", "GroupOutput.geometry"}));
    gnRun("gnApply", gnArgs({sphere, "V"}));
    scene::Graph::current().evaluate();

    render::FrameState s;
    ui::OverlayPainter painter(s.overlayTris, 1.0);
    for (const auto& d : ui::viewportHooks::drawers())
        d(s, painter, ui::ViewInfo());
    CHECK(s.tris3d.size() > 300);
    CHECK(s.linesOnTop.size() >= 24);
}
