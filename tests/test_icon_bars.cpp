#include <doctest/doctest.h>

#include "core/ExtensionContext.h"
#include "core/PrefStore.h"
#include "core/Registry.h"
#include "ui/Actions.h"
#include "ui/IconBars.h"
#include "ui/Tools.h"
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
