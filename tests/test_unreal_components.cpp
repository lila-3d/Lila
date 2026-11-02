#include <doctest/doctest.h>

#include "core/ExtensionContext.h"
#include "ops/Command.h"
#include "scene/AttrComponents.h"
#include "scene/Graph.h"

using namespace lila;
using namespace lila::scene;

namespace unrealComponentsTests
{
json ucCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
json ucPayloadOf(const std::string& node) { return attrComponents::unrealPayload(Graph::current(), Graph::current().find(node)->id()); }
} // namespace unrealComponentsTests
using namespace unrealComponentsTests;

TEST_CASE("unrealPayload: an object's components by export mode, values as JSON, none and hidden attrs left out")
{
    ucCmd("newScene");
    ucCmd("spaceLocator", {{"name", "Hero"}});
    CHECK(ucPayloadOf("Hero").empty());

    ucCmd("addAttr", {{"args", {"Hero"}}, {"longName", "usdApiSchemas"}, {"dataType", "stringArray"}});
    ucCmd("setAttr", {{"args", {"Hero.usdApiSchemas", json::array({"Health"})}}});
    ucCmd("addAttr", {{"args", {"Hero"}}, {"longName", "maxHealth"}, {"defaultValue", 100.0}, {"category", "Health"}});
    ucCmd("addAttr", {{"args", {"Hero"}}, {"longName", "regen"}, {"attributeType", "bool"}, {"category", "Health"}});
    ucCmd("addAttr", {{"args", {"Hero"}}, {"longName", "team"}, {"attributeType", "enum"}, {"enumName", "red:blue"}, {"category", "Health"}});
    ucCmd("addAttr", {{"args", {"Hero"}}, {"longName", "secret"}, {"hidden", true}, {"category", "Health"}});
    ucCmd("addAttr", {{"args", {"Hero"}}, {"longName", "tint"}, {"attributeType", "double3"}, {"category", "Look"}});
    ucCmd("addAttr", {{"args", {"Hero"}}, {"longName", "tags"}, {"dataType", "stringArray"}, {"category", "Look"}});
    ucCmd("addAttr", {{"args", {"Hero"}}, {"longName", "note"}, {"dataType", "string"}});
    ucCmd("addAttr", {{"args", {"Hero"}}, {"longName", "debugOnly"}, {"attributeType", "long"}, {"category", "Debug"}});
    ucCmd("setAttr", {{"args", {"Hero.maxHealth", 150.0}}});
    ucCmd("setAttr", {{"args", {"Hero.regen", true}}});
    ucCmd("setAttr", {{"args", {"Hero.team", 1}}});
    ucCmd("setAttr", {{"args", {"Hero.tintY", 0.5}}});
    ucCmd("setAttr", {{"args", {"Hero.tags", json::array({"metal", "heavy"})}}});
    ucCmd("setAttr", {{"args", {"Hero.note", "hello"}}});
    ucCmd("componentExport", {{"args", {"Hero"}}, {"component", "Look"}, {"mode", "component"}, {"unrealClass", "/Script/Game.LookComponent"}});
    ucCmd("componentExport", {{"args", {"Hero"}}, {"component", "Debug"}, {"mode", "none"}});

    const json p = ucPayloadOf("Hero");
    REQUIRE(p.size() == 3);
    CHECK(p[0]["name"] == "Health");
    CHECK(p[0]["mode"] == "assetUserData");
    CHECK_FALSE(p[0].contains("class"));
    CHECK(p[0]["attrs"] == json({{"maxHealth", 150.0}, {"regen", true}, {"team", "blue"}}));
    CHECK(p[1]["name"] == "Look");
    CHECK(p[1]["mode"] == "component");
    CHECK(p[1]["class"] == "/Script/Game.LookComponent");
    CHECK(p[1]["attrs"]["tint"] == json::array({0.0, 0.5, 0.0}));
    CHECK(p[1]["attrs"]["tags"] == json::array({"metal", "heavy"}));
    CHECK(p[2]["name"] == "");
    CHECK(p[2]["mode"] == "assetUserData");
    CHECK(p[2]["attrs"] == json({{"note", "hello"}}));

    ucCmd("componentExport", {{"args", {"Hero"}}, {"component", ""}, {"mode", "actorTags"}});
    CHECK(ucPayloadOf("Hero")[2]["mode"] == "actorTags");
}

TEST_CASE("addUnrealExporter: an extension's exporter names go when it unloads")
{
    ext::Context ctx("unrealComponentsTestExt");
    CHECK(attrComponents::addUnrealExporter(ctx, "TestLootExporter", "Rolls loot"));
    CHECK_FALSE(attrComponents::addUnrealExporter(ctx, "TestLootExporter"));
    REQUIRE(attrComponents::unrealExporters().find("TestLootExporter"));
    CHECK(*attrComponents::unrealExporters().find("TestLootExporter") == "Rolls loot");
    ctx.unloadAll();
    CHECK_FALSE(attrComponents::unrealExporters().contains("TestLootExporter"));
}
