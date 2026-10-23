#include <doctest/doctest.h>

#include "core/ExtensionContext.h"
#include "core/PrefStore.h"
#include "core/Registry.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/ExtensionUi.h"
#include "ui/HotkeyChord.h"
#include "ui/Hotkeys.h"
#include "ui/Layouts.h"
#include "ui/MarkingMenu.h"
#include "ui/Menus.h"
#include "ui/OptionBox.h"
#include "ui/Panels.h"
#include "ui/PreferencesDialog.h"
#include "ui/Shelf.h"

#include <filesystem>
#include <fstream>

using namespace lila;
using namespace lila::ui;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
std::vector<std::string> itemIds(const std::string& path)
{
    std::vector<std::string> out;
    for (const menus::Item* i : menus::itemsIn(path))
        out.push_back(i->kind == menus::Kind::Action ? i->action : i->id);
    return out;
}

/** Ids every UI registry holds, to compare before an extension loads and after it unloads. */
std::vector<size_t> sizes()
{
    return {actions::registry().size(),       menus::menuRegistry().size(),  menus::itemRegistry().size(), panels::registry().size(),
            shelves::registry().size(),       markingMenus::registry().size(), markingMenus::itemRegistry().size(),
            hotkeyLayers::registry().size(),  layouts::contributed().size(), attributeEditor::registry().size(),
            optionBox::customRegistry().size(), prefPages::registry().size()};
}
} // namespace

TEST_CASE("a registry lists its entries in registration order")
{
    Registry<int> r;
    r.add("b", 1);
    r.add("a", 2);
    r.add("c", 3);
    CHECK(r.idsInOrder() == std::vector<std::string>{"b", "a", "c"});
    r.put("b", 4);
    CHECK(r.idsInOrder() == std::vector<std::string>{"a", "c", "b"});
    CHECK(*r.allInOrder().back() == 4);
}

TEST_CASE("contributed prefs defaults sit between the built-ins and the user's")
{
    PrefStore& p = PrefStore::get();
    const std::string file = "test/contributed";
    p.setDefaults(file, "ext:a", {{"x", 1}, {"y", 1}});
    CHECK(p.value<int>(file, "/x", 0) == 1);
    p.setDefaults(file, "ext:b", {{"x", 2}});
    CHECK(p.value<int>(file, "/x", 0) == 2);
    CHECK(p.merged(file)["y"] == 1);
    p.set(file, "/x", 3);
    CHECK(p.value<int>(file, "/x", 0) == 3);
    CHECK(p.merged(file)["x"] == 3);
    p.remove(file, "/x");
    CHECK(p.removeDefaults(file, "ext:b"));
    CHECK(p.value<int>(file, "/x", 0) == 1);
    CHECK(p.contributedFiles("test/") == std::vector<std::string>{file});
    CHECK(p.removeDefaultsWithPrefix("ext:") == 1);
    CHECK_FALSE(p.hasContributed(file));
    CHECK(p.value<int>(file, "/x", 0) == 0);
}

TEST_CASE("menu items keep registration order, Lila's first, with after and before anchors")
{
    menus::addMenu({.id = "TestMenu"});
    menus::addActions("TestMenu", {"a", "b", "c"});
    menus::addItem({.menu = "TestMenu", .action = "ext1"}, "someExtension");
    menus::addItem({.menu = "TestMenu", .action = "ext2", .after = "TestMenu/a"}, "someExtension");
    menus::addItem({.menu = "TestMenu", .action = "ext3", .before = "TestMenu/a"}, "someExtension");
    menus::addActions("TestMenu", {"d"});
    CHECK(itemIds("TestMenu") == std::vector<std::string>{"ext3", "a", "ext2", "b", "c", "d", "ext1"});

    menus::addSubmenu("TestMenu", "Sub");
    menus::addActions("TestMenu/Sub", {"s1"});
    CHECK(itemIds("TestMenu/Sub") == std::vector<std::string>{"s1"});
    CHECK(menus::removeMenu("TestMenu"));
    CHECK(itemIds("TestMenu").empty());
    CHECK(itemIds("TestMenu/Sub").empty());
}

TEST_CASE("an extension's menu goes before Help unless it says otherwise")
{
    menus::addMenu({.id = "TestHelp", .title = "Help"});
    menus::addMenu({.id = "TestExtMenu"}, "someExtension");
    CHECK(menus::menuRegistry().find("TestExtMenu")->before == "Help");
    menus::removeMenu("TestExtMenu");
    menus::removeMenu("TestHelp");
}

TEST_CASE("a mode layer overrides a key's tap or hold alone")
{
    hotkeys::Slots space = hotkeys::splitSlots(json{{"press", "viewport.toggleQuad"}, {"hold", "hotbox.show"}});
    hotkeys::Slots tap = hotkeys::overlaySlots(space, json{{"press", "graph.fullScreen"}});
    CHECK(tap.press == "graph.fullScreen");
    CHECK(tap.hold == "hotbox.show");
    hotkeys::Slots hold = hotkeys::overlaySlots(space, json{{"hold", "puppet.scrub"}});
    CHECK(hold.press == "viewport.toggleQuad");
    CHECK(hold.hold == "puppet.scrub");
    CHECK(hotkeys::overlaySlots(space, json("other")).press == "other");
    CHECK(hotkeys::overlaySlots(space, json("other")).hold == "hotbox.show");
    CHECK(hotkeys::overlaySlots(space, json{{"press", nullptr}, {"hold", nullptr}}).empty());
}

TEST_CASE("hotkey layers become hotkeys defaults, mode layers under /modes")
{
    hotkeyLayers::add({"test.layer", {{"global", {{"Ctrl+Alt+F12", "test.action"}}}}, ""}, "someExtension");
    hotkeyLayers::add({"test.modeLayer", {{"viewport", {{"Space", {{"press", "test.tap"}}}}}}, "testMode"}, "someExtension");
    json def = PrefStore::get().merged("hotkeys");
    CHECK(def["contexts"]["global"]["Ctrl+Alt+F12"] == "test.action");
    CHECK(def["modes"]["testMode"]["contexts"]["viewport"]["Space"]["press"] == "test.tap");
    CHECK(hotkeyLayers::registry().removeOwned("someExtension") == 2);
    def = PrefStore::get().merged("hotkeys");
    CHECK_FALSE(def["contexts"]["global"].contains("Ctrl+Alt+F12"));
    CHECK_FALSE(def.contains("modes"));
}

TEST_CASE("marking menus resolve through the active set and take added items")
{
    markingMenus::add("testMenu", {{"N", {{"label", "Plain"}, {"action", "a"}}}});
    markingMenus::addSet("testSet", {{"testMenu", {{"N", {{"label", "From Set"}, {"action", "b"}}}}}});
    CHECK(markingMenus::resolve("testMenu")["N"]["label"] == "Plain");
    markingMenus::setActiveSet("testSet");
    CHECK(markingMenus::resolve("testMenu")["N"]["label"] == "From Set");
    markingMenus::setActiveSet("");
    std::string id = markingMenus::addItem({"testMenu", "overflow", {{"label", "Extra"}, {"action", "c"}}});
    std::string slot = markingMenus::addItem({"testMenu", "S", {{"label", "South"}, {"action", "d"}}});
    json def = markingMenus::resolve("testMenu");
    CHECK(def["overflow"].back()["label"] == "Extra");
    CHECK(def["S"]["label"] == "South");
    CHECK(markingMenus::removeItem(id));
    CHECK(markingMenus::removeItem(slot));
    CHECK(markingMenus::removeSet("testSet") == 1);
    CHECK(markingMenus::remove("testMenu"));
    CHECK(markingMenus::resolve("testMenu").empty());
}

TEST_CASE("a component mode opens the component viewport menu wherever it is pressed")
{
    for (const char* base : {"viewport", "viewportShift", "viewportCtrl"})
    {
        CHECK(markingMenus::viewportMenuName(base, false, false) == std::string(base) + "Empty");
        CHECK(markingMenus::viewportMenuName(base, true, false) == std::string(base) + "Object");
        CHECK(markingMenus::viewportMenuName(base, false, true) == std::string(base) + "Object");
        CHECK(markingMenus::viewportMenuName(base, true, true) == std::string(base) + "Object");
    }
}

TEST_CASE("floating shelf icon size steps by wheel notches within its limits")
{
    CHECK(shelves::stepFloatingIconSize(shelves::kFloatingIconDefault, 0) == shelves::kFloatingIconDefault);
    CHECK(shelves::stepFloatingIconSize(24, 2) == 32);
    CHECK(shelves::stepFloatingIconSize(24, -1) == 20);
    CHECK(shelves::stepFloatingIconSize(24, -50) == 16);
    CHECK(shelves::stepFloatingIconSize(24, 50) == 96);
}

TEST_CASE("contributed layouts list as extension layouts and can't be deleted")
{
    layouts::add("Test Contributed", {{"preset", {{"open", {"outliner"}}}}}, "someExtension");
    std::optional<layouts::Entry> e = layouts::find("Test Contributed");
    REQUIRE(e);
    CHECK(e->origin == layouts::Origin::Extension);
    CHECK(layouts::read("Test Contributed")->contains("preset"));
    CHECK_FALSE(layouts::remove("Test Contributed"));
    layouts::contributed().removeOwned("someExtension");
    CHECK_FALSE(layouts::find("Test Contributed"));
}

TEST_CASE("unloading an extension's context empties every UI registry it used")
{
    std::vector<size_t> before = sizes();
    {
        ext::Context ctx("uiTestExtension");
        CHECK(actions::add(ctx, {.id = "uiTest.go", .label = "Go", .category = "Test", .run = [] {}, .doc = "Test."}));
        CHECK(menus::addMenu(ctx, {.id = "UiTest"}));
        CHECK(menus::addItem(ctx, {.menu = "UiTest", .action = "uiTest.go"}));
        CHECK(panels::add(ctx, {.id = "uiTestPanel", .title = "UI Test"}));
        CHECK(actions::find("window.uiTestPanel"));
        CHECK(shelves::add(ctx, {"UiTest", json::array({{{"label", "Go"}, {"target", "uiTest.go"}}})}));
        CHECK(markingMenus::add(ctx, "uiTestMenu", {{"N", {{"label", "Go"}, {"action", "uiTest.go"}}}}));
        CHECK(markingMenus::addItem(ctx, {"uiTestMenu", "overflow", {{"label", "More"}}}));
        CHECK(hotkeyLayers::add(ctx, {"uiTest.keys", {{"global", {{"Ctrl+Alt+F11", "uiTest.go"}}}}, ""}));
        CHECK(layouts::add(ctx, "UI Test", {{"preset", json::object()}}));
        CHECK(attributeEditor::addSection(ctx, "transform", "UI Test", [](scene::NodeId) { return nullptr; }));
        CHECK(optionBox::addCustom(ctx, "uiTest.go", [] {}));
        CHECK(prefPages::add(ctx, {.id = "uiTest", .title = "UI Test"}));
        // Another extension can't take what this one owns.
        ext::Context other("otherExtension");
        CHECK(actions::add(other, {.id = "uiTest.go", .label = "Mine", .doc = "Test."}) == 0);
        CHECK(shelves::items("UiTest").size() == 1);
        CHECK(sizes() != before);
        ctx.unloadAll();
    }
    CHECK(sizes() == before);
    CHECK(shelves::items("UiTest").empty());
}

TEST_CASE("an owner sweep removes what Python registered without a context")
{
    std::vector<size_t> before = sizes();
    actions::add({.id = "pyTest.go", .label = "Go", .run = [] {}, .doc = "Test."}, "pyTestExtension");
    menus::addItem({.menu = "File", .action = "pyTest.go"}, "pyTestExtension");
    markingMenus::add("pyTestMenu", json::object(), "pyTestExtension");
    CHECK(extensionUi::removeOwned("pyTestExtension") == 3);
    CHECK(sizes() == before);
    CHECK(extensionUi::removeOwned(kLilaOwner) == 0);
}

TEST_CASE("an extension's resources folder registers marking menus, shelves, layouts and hotkeys")
{
    fs::path root = fs::temp_directory_path() / "lila_ui_resources_test.lilaext";
    fs::remove_all(root);
    auto write = [&](const fs::path& rel, const json& j) {
        fs::create_directories((root / "resources" / rel).parent_path());
        std::ofstream(root / "resources" / rel) << j.dump();
    };
    write("markingMenus/resSet/viewportObject.json", {{"N", {{"label", "Res"}, {"action", "x"}}}});
    write("shelves/ResShelf.json", {{"items", json::array({{{"label", "A"}, {"target", "x"}}})}});
    write("layouts/Res Layout.json", {{"preset", {{"open", {"outliner"}}}}});
    write("hotkeys/keys.json", {{"mode", "resMode"}, {"contexts", {{"global", {{"Ctrl+Alt+F10", "x"}}}}}});
    std::vector<size_t> before = sizes();
    CHECK(extensionUi::loadResources("resExtension", root) == 4);
    CHECK(markingMenus::registry().contains("resSet/viewportObject"));
    CHECK(shelves::items("ResShelf").size() == 1);
    CHECK(layouts::find("Res Layout"));
    CHECK(PrefStore::get().merged("hotkeys")["modes"]["resMode"]["contexts"]["global"]["Ctrl+Alt+F10"] == "x");
    CHECK(extensionUi::removeOwned("resExtension") == 4);
    CHECK(sizes() == before);
    fs::remove_all(root);
}
