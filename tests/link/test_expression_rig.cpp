#include <doctest/doctest.h>

#include "FakeServer.h"

#include "core/MainQueue.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "rigmath/MathNodes.h"
#include "scene/Graph.h"
#include "scene/SceneCommands.h"

using namespace lila;
using namespace lila::link;
using namespace linktest;
using json = nlohmann::json;

namespace exprRigTests
{
void setupExpressionRigTest()
{
    static bool once = [] {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        rigmath::registerTypes();
        rigmath::registerCommands();
        link::registerCommands();
        return true;
    }();
    (void)once;
}

json rigCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }

void buildCharacter()
{
    rigCmd("newScene");
    rigCmd("group", {{"empty", true}, {"name", "Char"}});
    rigCmd("joint", {{"position", {0, 0, 0}}, {"name", "root"}, {"parent", "Char"}});
    rigCmd("joint", {{"position", {0, 50, 0}}, {"name", "jaw"}, {"parent", "root"}});
    rigCmd("spaceLocator", {{"name", "ctrl"}});
    rigCmd("parent", {{"args", {"ctrl", "Char"}}});
    rigCmd("addAttr", {{"args", {"ctrl"}}, {"longName", "open"}, {"keyable", true}, {"minValue", 0.0}, {"maxValue", 1.0}});
    rigCmd("spaceLocator", {{"name", "fx"}});
    rigCmd("parent", {{"args", {"fx", "Char"}}});
    rigCmd("addAttr", {{"args", {"fx"}}, {"longName", "glow"}, {"keyable", true}});
    rigCmd("spaceLocator", {{"name", "away"}});
    rigCmd("expression", {{"args", {"fx.glow"}}, {"string", "jaw.rx * 2"}});
    rigCmd("expression", {{"args", {"jaw.rx"}}, {"string", "ctrl.open * 30 + time"}});
    rigCmd("expression", {{"args", {"root.ry"}}, {"string", "ctrl.open * 10"}});
    rigCmd("expression", {{"args", {"jaw.rz"}}, {"string", "fabs(ctrl.open)"}});
    rigCmd("expression", {{"args", {"away.tx"}}, {"string", "ctrl.open"}});
}

FakeServer::Handler rigEditor(json result)
{
    return [result](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "Chars"}, {"projectDir", "C:/Projects/Chars/"}, {"pluginVersion", "0.6.0"}, {"features", {"progress", "expressionRig"}}});
        if (m == "runPython")
            return FakeServer::reply(req, {{"ok", true}, {"result", ""}, {"log", {{{"type", "Info"}, {"text", "LILA_RESULT:" + result.dump()}}}}});
        if (m == "expressionPostProcess")
            return FakeServer::reply(req, {{"animBlueprint", "/Game/Chars/ABP_Char_Expressions"}, {"warnings", json::array()}});
        return json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"error", {{"code", -32601}, {"message", "Unknown method"}}}};
    };
}
} // namespace exprRigTests
using namespace exprRigTests;

TEST_CASE("unrealExpressionRig binds bones, curves and controls and orders expressions by what they read")
{
    setupExpressionRigTest();
    buildCharacter();
    json r = rigCmd("unrealExpressionRig", {{"args", {"Char"}}, {"skeletalMesh", "/Game/Chars/SK_Char"}, {"dryRun", true}});
    CHECK(r["rig"] == "/Game/Chars/CR_Char_Expressions");
    // glow reads jaw.rx, so jaw's expression runs first.
    const json& plugs = r["expressions"];
    REQUIRE(plugs.size() == 3);
    auto at = [&](const char* p) { return std::find(plugs.begin(), plugs.end(), json(p)) - plugs.begin(); };
    CHECK(at("jaw.rotateX") < at("fx.glow"));
    CHECK(at("root.rotateY") < 3);
    REQUIRE(r["skipped"].size() == 1);
    CHECK(r["skipped"][0]["plug"] == "jaw.rotateZ");
    CHECK(r["skipped"][0]["why"] == "runs as Python");
    REQUIRE(r["controls"].size() == 1);
    CHECK(r["controls"][0]["name"] == "ctrl_open");
    CHECK(r["controls"][0]["max"] == 1.0);
    CHECK(r["curves"] == json{"fx_glow"});

    const json& exprs = r["payload"]["expressions"];
    for (const json& e : exprs)
    {
        const json& b = e["bindings"];
        CHECK(b["basis"].size() == 9);
        if (e["plug"] == "jaw.rotateX")
        {
            CHECK(b["target"]["kind"] == "bone");
            CHECK(b["target"]["name"] == "jaw");
            CHECK(b["target"]["channel"] == 3);
            CHECK_FALSE(b["target"].contains("bindLocal"));
            CHECK(b["inputs"][0]["kind"] == "control");
            CHECK(e["program"].get<std::string>().find(" t") != std::string::npos);
        }
        if (e["plug"] == "fx.glow")
        {
            CHECK(b["target"] == json{{"kind", "curve"}, {"name", "fx_glow"}});
            CHECK(b["inputs"][0]["kind"] == "bone");
        }
        if (e["plug"] == "root.rotateY")
        {
            // A root bone carries its bind pose so LilaLink can solve the frame the import put it in.
            CHECK(b["target"]["bindLocal"].size() == 9);
            CHECK(b["target"]["bindTranslation"].size() == 3);
        }
    }
}

TEST_CASE("unrealExpressionRig builds the rig over the link and wires the post-process Anim Blueprint")
{
    setupExpressionRigTest();
    buildCharacter();
    FakeServer server(rigEditor({{"rig", "/Game/Chars/CR_Char_Expressions"}, {"nodes", {"LilaExpr_jaw_rotatex"}}, {"errors", json::array()}}));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    json r = rigCmd("unrealExpressionRig", {{"args", {"Char"}}, {"skeletalMesh", "/Game/Chars/SK_Char"}});
    CHECK(r["animBlueprint"] == "/Game/Chars/ABP_Char_Expressions");
    CHECK(r["unreal"]["nodes"][0] == "LilaExpr_jaw_rotatex");
    json python, post;
    for (const json& req : server.requests())
    {
        if (req["method"] == "runPython")
            python = req["params"];
        if (req["method"] == "expressionPostProcess")
            post = req["params"];
    }
    const std::string code = python["code"];
    CHECK(code.find("lila_link_expressions.build_and_print(") != std::string::npos);
    CHECK(post["rig"] == "/Game/Chars/CR_Char_Expressions");
    CHECK(post["skeletalMesh"] == "/Game/Chars/SK_Char");
    Link::get().disconnect();
}

TEST_CASE("unrealExpressionRig fails with Unreal's error")
{
    setupExpressionRigTest();
    buildCharacter();
    FakeServer server(rigEditor({{"errors", {"no skeletal mesh at /Game/Chars/SK_Char"}}}));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    CHECK_THROWS_WITH(rigCmd("unrealExpressionRig", {{"args", {"Char"}}, {"skeletalMesh", "/Game/Chars/SK_Char"}}),
                      doctest::Contains("no skeletal mesh"));
    Link::get().disconnect();
}
