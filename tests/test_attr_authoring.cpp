#include <doctest/doctest.h>

#include "core/Paths.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "scene/AttrComponents.h"
#include "scene/Graph.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;

namespace attrAuthoringTests
{
json cmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
json attrValue(const std::string& plug) { return cmd("getAttr", {{"args", {plug}}}); }
json userAttrs(const std::string& node) { return cmd("listAttr", {{"args", {node}}, {"userDefined", true}}); }
} // namespace attrAuthoringTests
using namespace attrAuthoringTests;

TEST_CASE("renameAttr and editAttr keep connections and keys; a type change converts the value and drops them")
{
    cmd("newScene");
    cmd("spaceLocator", {{"name", "Ctrl"}});
    cmd("spaceLocator", {{"name", "Box"}});
    cmd("addAttr", {{"args", {"Ctrl"}}, {"longName", "power"}, {"defaultValue", 2.0}, {"keyable", true}});
    cmd("setAttr", {{"args", {"Ctrl.power", 3.5}}});
    cmd("connectAttr", {{"args", {"Ctrl.power", "Box.tx"}}});
    cmd("setKeyframe", {{"args", {"Ctrl.power"}}, {"time", 0.0}, {"value", 1.0}});
    cmd("setKeyframe", {{"args", {"Ctrl.power"}}, {"time", 1.0}, {"value", 5.0}});

    CHECK(cmd("renameAttr", {{"args", {"Ctrl.power", "strength"}}}) == "Ctrl.strength");
    CHECK(userAttrs("Ctrl") == json::array({"strength"}));
    CHECK(cmd("listConnections", {{"args", {"Ctrl.strength"}}, {"source", false}, {"plugs", true}}) == json::array({"Box.translateX"}));
    CHECK(cmd("keyframe", {{"args", {"Ctrl.strength"}}, {"query", true}, {"keyCount", true}}).get<int>() == 2);
    cmd("undo");
    CHECK(userAttrs("Ctrl") == json::array({"power"}));
    cmd("redo");

    cmd("editAttr", {{"args", {"Ctrl.strength"}}, {"niceName", "Strength %"}, {"minValue", 0.0}, {"maxValue", 10.0}, {"doc", "How hard"},
                     {"category", "Combat"}});
    json q = cmd("editAttr", {{"args", {"Ctrl.strength"}}, {"query", true}});
    CHECK(q["niceName"] == "Strength %");
    CHECK(q["min"] == 0.0);
    CHECK(q["max"] == 10.0);
    CHECK(q["category"] == "Combat");
    CHECK(q["type"] == "float");
    CHECK(q["keyable"] == true);
    cmd("editAttr", {{"args", {"Ctrl.strength"}}, {"minValue", false}});
    CHECK_FALSE(cmd("editAttr", {{"args", {"Ctrl.strength"}}, {"query", true}}).contains("min"));
    CHECK(cmd("listConnections", {{"args", {"Ctrl.strength"}}}).size() == 2); // the curve and Box

    // A type change converts the value and drops connections and keys.
    cmd("cutKey", {{"args", {"Ctrl.strength"}}});
    cmd("setAttr", {{"args", {"Ctrl.strength", 3.6}}});
    cmd("editAttr", {{"args", {"Ctrl.strength"}}, {"type", "int"}});
    CHECK(attrValue("Ctrl.strength").get<int64_t>() == 4);
    CHECK(cmd("listConnections", {{"args", {"Ctrl.strength"}}}).empty());
    cmd("editAttr", {{"args", {"Ctrl.strength"}}, {"type", "vector"}});
    CHECK(attrValue("Ctrl.strength") == json::array({4.0, 4.0, 4.0}));
    CHECK(attrValue("Ctrl.strengthY").get<double>() == doctest::Approx(4.0));
    cmd("editAttr", {{"args", {"Ctrl.strength"}}, {"type", "string"}});
    CHECK(attrValue("Ctrl.strength") == "4 4 4");
    cmd("editAttr", {{"args", {"Ctrl.strength"}}, {"type", "enum"}, {"enumName", "low:mid:high"}});
    CHECK(cmd("editAttr", {{"args", {"Ctrl.strength"}}, {"query", true}})["enum"] == json::array({"low", "mid", "high"}));
    cmd("undo");
    cmd("undo");
    cmd("undo");
    cmd("undo");
    CHECK(attrValue("Ctrl.strength").get<double>() == doctest::Approx(3.6));

    CHECK_THROWS(cmd("editAttr", {{"args", {"Ctrl.translate"}}, {"niceName", "Move"}}));
    cmd("addAttr", {{"args", {"Ctrl"}}, {"longName", "other"}});
    CHECK_THROWS(cmd("renameAttr", {{"args", {"Ctrl.other", "strength"}}}));
    CHECK_THROWS(cmd("renameAttr", {{"args", {"Ctrl.other", "translateX"}}}));

    // Settings survive a save.
    json doc = io::serialize();
    cmd("newScene");
    io::deserialize(doc);
    CHECK(cmd("editAttr", {{"args", {"Ctrl.strength"}}, {"query", true}})["niceName"] == "Strength %");
}

TEST_CASE("reorderAttr moves added attributes and their connections; copyAttr copies definitions and values")
{
    cmd("newScene");
    cmd("spaceLocator", {{"name", "A"}});
    cmd("spaceLocator", {{"name", "B"}});
    for (const char* n : {"one", "two", "three"})
        cmd("addAttr", {{"args", {"A"}}, {"longName", n}, {"keyable", true}});
    cmd("setAttr", {{"args", {"A.one", 1.0}}});
    cmd("setAttr", {{"args", {"A.three", 3.0}}});
    cmd("connectAttr", {{"args", {"A.three", "B.ty"}}});
    cmd("spaceLocator", {{"name", "D"}});
    cmd("connectAttr", {{"args", {"D.tx", "A.one"}}});

    cmd("reorderAttr", {{"args", {"A.three"}}, {"before", "one"}});
    CHECK(userAttrs("A") == json::array({"three", "one", "two"}));
    CHECK(cmd("listConnections", {{"args", {"A.three"}}, {"plugs", true}}) == json::array({"B.translateY"}));
    CHECK(cmd("listConnections", {{"args", {"A.one"}}, {"plugs", true}}) == json::array({"D.translateX"}));
    CHECK(attrValue("A.three").get<double>() == doctest::Approx(3.0));
    cmd("reorderAttr", {{"args", {"A.three"}}, {"index", -1}});
    CHECK(userAttrs("A") == json::array({"one", "two", "three"}));
    cmd("reorderAttr", {{"args", {"A.one"}}, {"after", "two"}});
    CHECK(userAttrs("A") == json::array({"two", "one", "three"}));
    cmd("undo");
    cmd("undo");
    CHECK(userAttrs("A") == json::array({"three", "one", "two"}));
    CHECK(cmd("listConnections", {{"args", {"A.three"}}, {"plugs", true}}) == json::array({"B.translateY"}));
    CHECK_THROWS(cmd("reorderAttr", {{"args", {"A.one"}}, {"before", "translate"}}));

    cmd("disconnectAttr", {{"args", {"A.one"}}});
    cmd("setAttr", {{"args", {"A.one", 7.0}}});
    cmd("spaceLocator", {{"name", "C"}});
    CHECK(cmd("copyAttr", {{"args", {"A", "C"}}, {"attribute", {"one", "two"}}}) == json::array({"C.one", "C.two"}));
    CHECK(attrValue("C.one").get<double>() == doctest::Approx(7.0));
    CHECK(userAttrs("C") == json::array({"one", "two"}));
    CHECK(cmd("copyAttr", {{"args", {"A", "C"}}, {"connections", true}}) == json::array({"C.three"}));
    CHECK(cmd("listConnections", {{"args", {"C.three"}}, {"destination", false}}).empty());
    cmd("connectAttr", {{"args", {"D.tz", "A.two"}}});
    cmd("copyAttr", {{"args", {"A", "C"}}, {"attribute", {"two"}}, {"connections", true}});
    CHECK(cmd("listConnections", {{"args", {"C.two"}}, {"plugs", true}, {"connections", true}}) == json::array({"C.two", "D.translateZ"}));
}

TEST_CASE("listConnections filters by direction and type")
{
    cmd("newScene");
    cmd("spaceLocator", {{"name", "A"}});
    cmd("spaceLocator", {{"name", "B"}});
    cmd("connectAttr", {{"args", {"A.tx", "B.ty"}}});
    cmd("setKeyframe", {{"args", {"A.ry"}}});
    CHECK(cmd("listConnections", {{"args", {"A"}}, {"destination", false}, {"type", "animCurve"}}).size() == 1);
    CHECK(cmd("listConnections", {{"args", {"A"}}, {"source", false}}) == json::array({"B"}));
    CHECK(cmd("listConnections", {{"args", {"B"}}}) == json::array({"A"}));
}

TEST_CASE("Components: save a heading, add it elsewhere with its schema, choose its Unreal export")
{
    namespace fs = std::filesystem;
    const fs::path dir = attrComponents::folder(false);
    cmd("newScene");
    cmd("spaceLocator", {{"name", "Hero"}});
    cmd("spaceLocator", {{"name", "Npc"}});
    cmd("addAttr", {{"args", {"Hero"}}, {"longName", "maxHealth"}, {"defaultValue", 100.0}, {"minValue", 0.0}, {"category", "TestHealthAPI"},
                    {"doc", "Hit points"}});
    cmd("addAttr", {{"args", {"Hero"}}, {"longName", "regen"}, {"attributeType", "bool"}, {"category", "TestHealthAPI"}});
    CHECK_THROWS(cmd("saveComponent", {{"args", {"Hero"}}, {"category", "Nothing"}}));
    const std::string file = cmd("saveComponent", {{"args", {"Hero"}}, {"category", "TestHealthAPI"}, {"doc", "Life"}});
    CHECK(fs::exists(paths::fromUtf8(file)));

    json listed;
    for (const json& c : cmd("listComponents", {{"source", "user"}}))
        if (c["name"] == "TestHealthAPI")
            listed = c;
    REQUIRE(listed.is_object());
    CHECK(listed["doc"] == "Life");
    CHECK(listed["attrs"].size() == 2);
    CHECK(listed["attrs"][0]["default"] == 100.0);

    CHECK(cmd("addComponent", {{"args", {"Npc"}}, {"name", "TestHealthAPI"}}) == json::array({"Npc.maxHealth", "Npc.regen"}));
    CHECK(attrValue("Npc.maxHealth").get<double>() == doctest::Approx(100.0));
    CHECK(cmd("editAttr", {{"args", {"Npc.maxHealth"}}, {"query", true}})["category"] == "TestHealthAPI");
    CHECK(cmd("addComponent", {{"args", {"Npc"}}, {"name", "TestHealthAPI"}}).empty());
    CHECK_THROWS(cmd("addComponent", {{"args", {"Npc"}}, {"name", "NoSuchComponent"}}));

    // A component naming a schema adds it to usdApiSchemas; removing the heading takes it out again.
    attrComponents::Component schema;
    schema.name = "TestSchemaAPI";
    schema.usdSchema = "TestSchemaAPI";
    schema.attrs.push_back(AttrDef{.name = "speed", .type = PlugType::Float, .defaultValue = 1.0});
    attrComponents::save(schema, false);
    cmd("addComponent", {{"args", {"Npc"}}, {"name", "TestSchemaAPI"}});
    CHECK(attrValue("Npc.usdApiSchemas") == json::array({"TestSchemaAPI"}));
    cmd("deleteAttr", {{"args", {"Npc"}}, {"category", "TestSchemaAPI"}});
    CHECK(attrValue("Npc.usdApiSchemas") == json::array());

    CHECK(cmd("componentExport", {{"args", {"Npc"}}, {"component", "TestHealthAPI"}, {"query", true}})["mode"] == "assetUserData");
    cmd("componentExport", {{"args", {"Npc"}}, {"component", "TestHealthAPI"}, {"mode", "component"}, {"unrealClass", "/Script/Game.HealthComponent"}});
    json e = cmd("componentExport", {{"args", {"Npc"}}, {"component", "TestHealthAPI"}, {"query", true}});
    CHECK(e["mode"] == "component");
    CHECK(e["class"] == "/Script/Game.HealthComponent");
    CHECK_THROWS(cmd("componentExport", {{"args", {"Npc"}}, {"component", "TestHealthAPI"}, {"mode", "banana"}}));
    cmd("saveComponent", {{"args", {"Npc"}}, {"category", "TestHealthAPI"}, {"name", "TestHealth2"}});
    json saved = attrComponents::find("TestHealth2")->toJson();
    CHECK(saved["unreal"]["mode"] == "component");

    for (const char* n : {"TestHealthAPI", "TestSchemaAPI", "TestHealth2"})
        CHECK(cmd("deleteComponent", {{"name", n}}).get<bool>());
    CHECK_FALSE(attrComponents::find("TestHealthAPI"));
}

TEST_CASE("mute holds a channel at the time it was muted; breakdown marks keys")
{
    cmd("newScene");
    cmd("spaceLocator", {{"name", "M"}});
    cmd("setKeyframe", {{"args", {"M.tx"}}, {"time", 0.0}, {"value", 0.0}});
    cmd("setKeyframe", {{"args", {"M.tx"}}, {"time", 1.0}, {"value", 10.0}});
    cmd("setKeyframe", {{"args", {"M.tx"}}, {"time", 0.5}, {"value", 2.0}, {"breakdown", true}});
    CHECK(cmd("breakdown", {{"args", {"M.tx"}}, {"query", true}}) == json::array({0.5}));
    CHECK(cmd("breakdown", {{"args", {"M.tx"}}, {"time", 1.0}}).get<int>() == 1);
    CHECK(cmd("breakdown", {{"args", {"M.tx"}}, {"query", true}}) == json::array({0.5, 1.0}));
    CHECK(cmd("breakdown", {{"args", {"M.tx"}}, {"value", false}}).get<int>() == 2);

    cmd("currentTime", {{"args", {1.0}}});
    CHECK(cmd("mute", {{"args", {"M.tx"}}}).get<int>() == 1);
    CHECK(cmd("mute", {{"args", {"M.tx"}}, {"query", true}}).get<bool>());
    cmd("currentTime", {{"args", {0.0}}});
    CHECK(attrValue("M.tx").get<double>() == doctest::Approx(10.0));
    cmd("mute", {{"args", {"M.tx"}}, {"disable", true}});
    CHECK(attrValue("M.tx").get<double>() == doctest::Approx(0.0));
    cmd("undo");
    CHECK(attrValue("M.tx").get<double>() == doctest::Approx(10.0));
    cmd("setKeyframe", {{"args", {"M.ry"}}, {"time", 0.0}, {"value", 30.0}});
    cmd("setKeyframe", {{"args", {"M.ry"}}, {"time", 1.0}, {"value", 60.0}});
    cmd("mute", {{"args", {"M.ry"}}});
    cmd("currentTime", {{"args", {1.0}}});
    CHECK(attrValue("M.ry").get<double>() == doctest::Approx(30.0));
}
