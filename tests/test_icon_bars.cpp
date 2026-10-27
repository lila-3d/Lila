#include <doctest/doctest.h>

#include "core/ExtensionContext.h"
#include "core/PrefStore.h"
#include "core/Registry.h"
#include "ops/Command.h"
#include "scene/ComponentPick.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "ui/Actions.h"
#include "ui/IconBars.h"
#include "ui/Tools.h"
#include "ui/ViewportContext.h"
#include "ui/ViewportToolbar.h"

using namespace lila;
using namespace lila::ui;

namespace
{
std::vector<std::string> idsIn(const std::string& bar)
{
    std::vector<std::string> out;
    for (const iconBars::Item* i : iconBars::itemsIn(bar))
        out.push_back(i->id);
    return out;
}
} // namespace

TEST_CASE("icon bar items keep registration order, Lila's first, with after and before anchors")
{
    const std::string bar = "testBar";
    iconBars::add({.id = "t/a", .bar = bar, .action = "a"});
    iconBars::add({.id = "t/b", .bar = bar, .action = "b"});
    iconBars::add({.id = "t/x", .bar = bar, .action = "x"}, "someExtension");
    iconBars::add({.id = "t/c", .bar = bar, .action = "c", .before = "t/b"});
    iconBars::add({.id = "t/y", .bar = bar, .action = "y", .after = "t/a"}, "someExtension");
    CHECK(idsIn(bar) == std::vector<std::string>{"t/a", "t/y", "t/c", "t/b", "t/x"});
    // Ids made from the action stay unique within the registry.
    std::string first = iconBars::add({.bar = bar, .action = "same"});
    std::string second = iconBars::add({.bar = bar, .action = "same"});
    CHECK(first == "testBar/same");
    CHECK(second != first);
    CHECK(iconBars::registry().removeOwned("someExtension") == 2);
    for (const std::string& id : idsIn(bar))
        iconBars::remove(id);
    CHECK(idsIn(bar).empty());
}

TEST_CASE("an extension's icon bar items go when it unloads")
{
    const size_t before = iconBars::registry().size();
    {
        ext::Context ctx("iconBarTestExtension");
        CHECK(iconBars::add(ctx, {.id = "iconBarTest/go", .bar = iconBars::kToolBox, .action = "tool.select", .icon = "pointer"}));
        CHECK(iconBars::add(ctx, {.id = "iconBarTest/menu", .bar = iconBars::kOutliner, .kind = iconBars::Kind::Menu,
                                  .menuActions = {"outliner.expandAll", "", "#More", "outliner.collapseAll"}}));
        ext::Context other("otherIconBarExtension");
        CHECK(iconBars::add(other, {.id = "iconBarTest/go", .bar = iconBars::kToolBox}) == 0);
        CHECK(iconBars::registry().size() == before + 2);
        ctx.unloadAll();
    }
    CHECK(iconBars::registry().size() == before);
    CHECK(!iconBars::registry().contains("iconBarTest/go"));
}

TEST_CASE("toggle actions and tools report their state to bars and menus")
{
    bool on = false;
    actions::add({.id = "iconBarTest.toggle", .label = "Toggle", .category = "Test", .run = [&on] { on = !on; }, .doc = "Test.",
                  .checked = [&on] { return on; }});
    CHECK(viewportActionState("iconBarTest.toggle", nullptr) == false);
    actions::run(nlohmann::json("iconBarTest.toggle"));
    CHECK(viewportActionState("iconBarTest.toggle", nullptr) == true);
    actions::remove("iconBarTest.toggle");

    const Tool was = tools::current();
    tools::setCurrent(Tool::Select);
    PrefStore::get().set("general", "/select/lasso", false);
    CHECK(viewportActionState("tool.select", nullptr) == true);
    CHECK(viewportActionState("tool.move", nullptr) == false);
    CHECK(viewportActionState("select.marqueeTool", nullptr) == true);
    CHECK(viewportActionState("select.lassoTool", nullptr) == false);
    PrefStore::get().set("general", "/select/lasso", true);
    CHECK(viewportActionState("select.lassoTool", nullptr) == true);
    tools::setCurrent(Tool::Move);
    CHECK(viewportActionState("select.lassoTool", nullptr) == false);
    PrefStore::get().set("general", "/select/lasso", false);
    tools::setCurrent(was);
}

TEST_CASE("viewport context: feature options follow what the scene holds")
{
    namespace vc = viewportContext;
    using namespace lila::scene;
    ops::execute("newScene", json::object());
    Graph& g = Graph::current();
    CHECK(vc::scan(g) == 0);
    g.createNode(*types::find("mesh"), "ctxMesh");
    g.createNode(*types::find("light"), "ctxLight");
    CHECK(vc::scan(g) == 0);
    g.createNode(*types::find("joint"), "ctxJoint");
    CHECK(vc::scan(g) == vc::Joints);
    Node* m = g.createNode(*types::find("material"), "ctxMaterial");
    CHECK(vc::scan(g) == (vc::Joints | vc::Materials));
    g.setValue({m->id(), material::kShadingModel, -1}, int64_t(material::ShadingModel::Painterly));
    CHECK(vc::scan(g) == (vc::Joints | vc::Materials | vc::StylisedMaterials));
    CHECK(vc::present() == vc::scan(g));
    ops::execute("newScene", json::object());
    CHECK(vc::present() == 0);
}

TEST_CASE("lights have their own show filter and selection mask")
{
    namespace sm = lila::scene::selectionMask;
    using namespace lila::scene;
    Graph& g = Graph::current();
    Node* light = g.createNode(*types::find("light"), "maskLight");
    Node* loc = g.createNode(*types::find("locator"), "maskLocator");
    CHECK(sm::kindOf(*light) == sm::Kind::Light);
    CHECK(sm::kindOf(*loc) == sm::Kind::Locator);
    sm::setShown(sm::Kind::Light, false);
    CHECK_FALSE(sm::shown(*light));
    CHECK_FALSE(sm::pickable(*light));
    CHECK(sm::shown(*loc));
    CHECK(viewportActionState("viewport.show.light", nullptr) == false);
    sm::setShown(sm::Kind::Light, true);
    CHECK(viewportActionState("viewport.show.light", nullptr) == true);
    g.deleteNode(light->id());
    g.deleteNode(loc->id());
}
