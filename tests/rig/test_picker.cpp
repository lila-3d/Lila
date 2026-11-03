#include "TestUtil.h"

#include "ops/Undo.h"
#include "io/SceneFile.h"
#include "scene/Picker.h"
#include "scene/Poses.h"

#include <filesystem>

using namespace rigtest;
using namespace lila::scene;
namespace fs = std::filesystem;

namespace
{
/** Two characters with the same joint names under namespaces a and b. */
void twoCharacters()
{
    run("newScene");
    picker::clearScene();
    poses::clearScene();
    for (const char* ns : {"a", "b"})
    {
        std::string p = std::string(ns) + ":";
        run("select", {{"clear", true}});
        run("joint", {{"position", v3(0, 100, 0)}, {"name", p + "pelvis"}});
        run("joint", {{"position", v3(10, 120, 0)}, {"name", p + "hand_l"}});
        run("select", {{"args", {p + "pelvis"}}});
        run("joint", {{"position", v3(-10, 120, 0)}, {"name", p + "hand_r"}});
    }
    run("select", {{"clear", true}});
}

json layout()
{
    return {{"name", "body"},
            {"character", "pelvis"},
            {"pages", {{{"name", "Body"},
                        {"width", 200},
                        {"items", {{{"target", "pelvis"}, {"x", 100}, {"y", 100}, {"size", 20}},
                                   {{"target", "hand_l"}, {"x", 150}, {"y", 60}, {"size", 10}, {"shape", "circle"}, {"mirror", true}}}}}}}};
}
} // namespace

TEST_CASE("picker documents round trip through json, files and the scene")
{
    picker::Document d = picker::generateFromLayout(layout());
    REQUIRE(d.pages.size() == 1);
    REQUIRE(d.pages[0].items.size() == 3);
    const picker::Item& hand = d.pages[0].items[1];
    CHECK(hand.shape == picker::Shape::Circle);
    CHECK(hand.x == doctest::Approx(145));
    CHECK(hand.color == picker::sideColor("hand_l"));
    // The mirror copy lands on the other side of the page centre with the other side's target and colour.
    const picker::Item& mirror = d.pages[0].items[2];
    CHECK(mirror.targets == std::vector<std::string>{"hand_r"});
    CHECK(mirror.x == doctest::Approx(200 - 155));
    CHECK(mirror.color == picker::sideColor("hand_r"));
    CHECK(mirror.color != hand.color);

    d.pages[0].items[0].action = picker::Action::fromJson({{"kind", "toggleAttr"}, {"attr", "visibility"}, {"values", {1, 0}}});
    d.pages[0].items[1].points = {{0, 0}, {10, 0}, {5, 10}};
    d.pages[0].background = "bg.png";
    json j = d.toJson();
    picker::Document back = picker::Document::fromJson(j);
    CHECK(back.toJson() == j);
    CHECK(back.pages[0].items[0].action.kind == picker::ActionKind::ToggleAttr);
    CHECK(back.pages[0].items[0].action.attr == "visibility");

    fs::path file = fs::temp_directory_path() / "lila_picker_test.lilapicker";
    picker::writeFile(d, file);
    CHECK(picker::readFile(file).toJson() == j);

    CHECK_THROWS(picker::Document::fromJson(json::object()));
    CHECK_THROWS(picker::Item::fromJson({{"shape", "hexagon"}}));

    // Duplicate ids are renamed on load so every id addresses one item.
    json dup = j;
    dup["pages"][0]["items"][1]["id"] = dup["pages"][0]["items"][0]["id"];
    picker::Document fixed = picker::Document::fromJson(dup);
    CHECK(fixed.pages[0].items[0].id != fixed.pages[0].items[1].id);

    twoCharacters();
    run("createPicker", {{"args", {"body"}}, {"layout", layout()}});
    fs::path scene = fs::temp_directory_path() / "lila_picker_test.lila";
    run("saveScene", {{"args", {scene.string()}}});
    picker::clearScene();
    run("openScene", {{"args", {scene.string()}}, {"force", true}});
    REQUIRE(picker::findScene("body"));
    CHECK(picker::findScene("body")->pages[0].items.size() == 3);

    fs::path out = fs::temp_directory_path() / "lila_picker_export";
    std::string written = run("exportPicker", {{"args", {"body"}}, {"file", out.string()}}).get<std::string>();
    CHECK(written.ends_with(".lilapicker"));
    CHECK_THROWS_AS(run("importPicker", {{"args", {written}}}), CommandError);
    run("importPicker", {{"args", {written}}, {"name", "copy"}});
    CHECK(picker::findScene("copy")->pages[0].items.size() == 3);
}

TEST_CASE("picker targets resolve per namespace")
{
    twoCharacters();
    picker::Document d = picker::generateFromLayout(layout());
    const picker::Item& pelvis = d.pages[0].items[0];
    CHECK(picker::namespaceOf("a:b:hand_l") == "a:b");
    CHECK(picker::namespaceOf("hand_l").empty());
    CHECK(picker::namespacesFor(graph(), d) == std::vector<std::string>{"a", "b"});
    CHECK(graph().node(picker::resolve(graph(), "pelvis", "a"))->name() == "a:pelvis");
    CHECK(graph().node(picker::resolve(graph(), "pelvis", "b"))->name() == "b:pelvis");
    // Ambiguous without a namespace, absent in an unknown one.
    CHECK(picker::resolve(graph(), "pelvis", "") == kNoNode);
    CHECK(picker::resolve(graph(), "pelvis", "c") == kNoNode);
    CHECK(picker::resolveTargets(graph(), pelvis, "b").size() == 1);

    // With only one character the bare name finds it whatever its namespace.
    run("delete", {{"args", {"b:pelvis"}}});
    CHECK(graph().node(picker::resolve(graph(), "pelvis", ""))->name() == "a:pelvis");
}

TEST_CASE("picker mirror, align and distribute")
{
    picker::Item it;
    it.x = 10;
    it.w = 20;
    it.targets = {"arm_l", "spine"};
    it.label = "arm_l";
    it.shape = picker::Shape::Polygon;
    it.points = {{0, 0}, {20, 0}, {5, 10}};
    it.action.kind = picker::ActionKind::ApplyPose;
    picker::Item m = picker::mirrored(it, 50);
    CHECK(m.x == doctest::Approx(70));
    CHECK(m.targets == std::vector<std::string>{"arm_r", "spine"});
    CHECK(m.label == "arm_r");
    CHECK(m.points[0].x == doctest::Approx(20));
    CHECK(m.points[2].x == doctest::Approx(15));
    CHECK(m.action.mirror);
    CHECK(picker::mirrored(m, 50).x == doctest::Approx(it.x));

    picker::Item a, b, c;
    a.x = 0, a.w = 10, a.y = 5, a.h = 10;
    b.x = 50, b.w = 20, b.y = 0, b.h = 30;
    c.x = 20, c.w = 10, c.y = 40, c.h = 10;
    std::vector<picker::Item*> items = {&a, &b, &c};
    picker::distribute(items, "horizontal");
    // Total span 0..70 holds 40 of items: two gaps of 15, in position order a, c, b.
    CHECK(a.x == doctest::Approx(0));
    CHECK(c.x == doctest::Approx(25));
    CHECK(b.x == doctest::Approx(50));
    picker::align(items, "top");
    CHECK(a.y == 0);
    CHECK(c.y == 0);
    picker::align(items, "hcenter");
    CHECK(a.x + a.w / 2 == doctest::Approx(b.x + b.w / 2));
    CHECK_THROWS(picker::align(items, "diagonal"));
    CHECK(picker::snap(13, 8) == 16);

    CHECK(picker::sideColor("hand_l") != picker::sideColor("spine"));
    CHECK(picker::sideColor("Left_foot") == picker::sideColor("hand_l"));
}

TEST_CASE("picker hit tests and selection sync")
{
    twoCharacters();
    picker::Document d = picker::generateFromLayout(layout());
    const picker::Page& page = d.pages[0];
    const std::string pelvisId = page.items[0].id, handId = page.items[1].id, handRId = page.items[2].id;
    CHECK(page.itemAt(100, 100)->id == pelvisId);
    // The circle's bounding corner misses.
    CHECK(page.itemAt(145.5, 55.5) == nullptr);
    CHECK(page.itemAt(150, 60)->id == handId);
    CHECK(page.itemsIn(0, 0, 200, 80).size() == 2);

    // Scene to picker: which items light up.
    run("select", {{"args", {"a:hand_l"}}});
    CHECK(picker::highlightOf(graph(), page.items[1], "a", graph().selection()) == picker::Highlight::Full);
    CHECK(picker::highlightOf(graph(), page.items[1], "b", graph().selection()) == picker::Highlight::None);
    picker::Item both = page.items[1];
    both.targets.push_back("hand_r");
    CHECK(picker::highlightOf(graph(), both, "a", graph().selection()) == picker::Highlight::Partial);

    // Picker to scene.
    std::vector<NodeId> sel = picker::selectionFor(graph(), page, {pelvisId, handId}, "b");
    REQUIRE(sel.size() == 2);
    CHECK(graph().node(sel[0])->name() == "b:pelvis");

    run("createPicker", {{"args", {"body"}}, {"layout", layout()}});
    run("pickerSelect", {{"args", {"body"}}, {"items", {pelvisId}}, {"namespace", "b"}});
    CHECK(selectedNames() == std::vector<std::string>{"b:pelvis"});
    run("pickerSelect", {{"args", {"body"}}, {"items", {handId, handRId}}, {"namespace", "b"}, {"mode", "add"}});
    CHECK(selectedNames() == std::vector<std::string>{"b:pelvis", "b:hand_l", "b:hand_r"});
    run("pickerSelect", {{"args", {"body"}}, {"items", {pelvisId}}, {"namespace", "b"}, {"mode", "toggle"}});
    CHECK(selectedNames() == std::vector<std::string>{"b:hand_l", "b:hand_r"});
    run("pickerSelect", {{"args", {"body"}}, {"namespace", "b"}});
    CHECK(graph().selection().empty());
    CHECK_THROWS_AS(run("pickerSelect", {{"args", {"body"}}, {"items", {"nope"}}}), CommandError);
}

TEST_CASE("picker commands edit with undo, and run actions")
{
    twoCharacters();
    UndoStack::get().clear();
    run("createPicker", {{"args", {"body"}}, {"layout", layout()}, {"namespace", "a"}});
    CHECK(run("listPickers").size() == 1);
    CHECK_THROWS_AS(run("createPicker", {{"args", {"body"}}}), CommandError);

    json r = run("editPicker", {{"args", {"body"}}, {"addItem", {{"shape", "rect"}, {"x", 3}, {"y", 4}, {"targets", {"hand_l"}}}}});
    REQUIRE(r["added"].size() == 1);
    std::string added = r["added"][0].get<std::string>();
    CHECK(picker::findScene("body")->pages[0].items.size() == 4);
    run("undo");
    CHECK(picker::findScene("body")->pages[0].items.size() == 3);
    run("redo");
    CHECK(picker::findScene("body")->pages[0].items.size() == 4);

    run("editPicker", {{"args", {"body"}}, {"moveItems", {added}}, {"offset", {10, 20}}});
    picker::Document d = *picker::findScene("body");
    CHECK(d.item(added)->x == doctest::Approx(13));
    CHECK(d.item(added)->y == doctest::Approx(24));
    run("editPicker", {{"args", {"body"}}, {"setItems", {{{"id", added}, {"label", "Hand"}, {"color", "#112233"},
                                                         {"action", {{"kind", "toggleAttr"}, {"attr", "visibility"}, {"values", {true, false}}}}}}}});
    d = *picker::findScene("body");
    CHECK(d.item(added)->label == "Hand");
    CHECK(d.item(added)->action.kind == picker::ActionKind::ToggleAttr);

    r = run("editPicker", {{"args", {"body"}}, {"mirrorItems", {added}}});
    std::string mirror = r["added"][0].get<std::string>();
    d = *picker::findScene("body");
    CHECK(d.item(mirror)->targets == std::vector<std::string>{"hand_r"});
    CHECK(d.item(mirror)->x == doctest::Approx(200 - 13 - 24));

    run("editPicker", {{"args", {"body"}}, {"addPage", "Face"}, {"renamePage", {"Body", "Main"}}});
    CHECK(picker::findScene("body")->pages.size() == 2);
    CHECK(picker::findScene("body")->page("Main"));

    run("select", {{"args", {"b:pelvis", "b:hand_l"}}});
    r = run("editPicker", {{"args", {"body"}}, {"page", "Face"}, {"addFromSelection", true}});
    CHECK(r["added"].size() == 2);
    d = *picker::findScene("body");
    CHECK(d.page("Face")->items[0].targets == std::vector<std::string>{"pelvis"});

    run("editPicker", {{"args", {"body"}}, {"removeItems", {added}}});
    CHECK_FALSE(picker::findScene("body")->item(added));
    run("undo");
    CHECK(picker::findScene("body")->item(added));

    // Toggle: hand_l's visibility in namespace a flips, b's stays.
    run("pickerRun", {{"args", {"body", added}}});
    CHECK_FALSE(node("a:hand_l").as<bool>(Transform::kVisibility));
    CHECK(node("b:hand_l").as<bool>(Transform::kVisibility));
    run("pickerRun", {{"args", {"body", added}}});
    CHECK(node("a:hand_l").as<bool>(Transform::kVisibility));
    run("pickerRun", {{"args", {"body", added}}, {"namespace", "b"}});
    CHECK_FALSE(node("b:hand_l").as<bool>(Transform::kVisibility));

    // Run command: args default to the targets.
    run("editPicker", {{"args", {"body"}}, {"setItems", {{{"id", added}, {"action", {{"kind", "runCommand"}, {"command", "select"}}}}}}});
    run("select", {{"clear", true}});
    run("pickerRun", {{"args", {"body", added}}, {"namespace", "b"}});
    CHECK(selectedNames() == std::vector<std::string>{"b:hand_l"});

    run("deletePicker", {{"args", {"body"}}});
    CHECK(run("listPickers").empty());
    run("undo");
    CHECK(run("listPickers").size() == 1);
}

TEST_CASE("template pickers are read only until copied")
{
    twoCharacters();
    picker::Document t = picker::generateFromLayout(layout());
    t.name = "mannyBody";
    picker::registerTemplatePicker(t, "rigTemplates");
    json list = run("listPickers", {{"namespaces", true}});
    REQUIRE(list.size() == 1);
    CHECK(list[0]["source"] == "template");
    CHECK(list[0]["namespaces"] == json::array({"a", "b"}));
    CHECK_THROWS_AS(run("editPicker", {{"args", {"mannyBody"}}, {"rename", "x"}}), CommandError);
    CHECK_THROWS_AS(run("deletePicker", {{"args", {"mannyBody"}}}), CommandError);
    // Selecting through a template picker works without copying it.
    run("pickerSelect", {{"args", {"mannyBody"}}, {"items", {t.pages[0].items[0].id}}, {"namespace", "a"}});
    CHECK(selectedNames() == std::vector<std::string>{"a:pelvis"});

    run("createPicker", {{"args", {"myBody"}}, {"fromTemplate", "mannyBody"}});
    run("editPicker", {{"args", {"myBody"}}, {"rename", "myBody2"}});
    CHECK(picker::findScene("myBody2"));
    picker::unregisterOwned("rigTemplates");
    CHECK(picker::templatePickers().empty());
}
