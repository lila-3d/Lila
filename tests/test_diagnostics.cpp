#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"

using namespace lila;
namespace dx = scene::diagnostics;

namespace
{
json run(const std::string& name, const json& call = json::object()) { return ops::execute(name, call); }

/** A copy: reporting more moves the stored ones. */
dx::Issue issue(const json& id)
{
    const dx::Issue* i = dx::find(id.get<uint64_t>());
    REQUIRE(i);
    return *i;
}

std::string makeCrate()
{
    run("newScene");
    run("clearIssues");
    return run("createNode", {{"args", {"transform"}}, {"name", "crate"}}).get<std::string>();
}
} // namespace

TEST_CASE("diagnostics: targets split into object, component and attribute")
{
    dx::Target t = dx::parseTarget("/World/crate.healthcomponent:health");
    CHECK(t.object == "/World/crate");
    CHECK(t.component == "healthcomponent");
    CHECK(t.attr == "health");
    t = dx::parseTarget("|grp|crate");
    CHECK(t.object == "|grp|crate");
    CHECK(t.component.empty());
    CHECK_THROWS_AS(dx::parseTarget(".health"), CommandError);
}

TEST_CASE("diagnostics: issues attach to nodes, attributes and components")
{
    const std::string crate = makeCrate();
    scene::Graph& g = scene::Graph::current();
    const scene::NodeId id = g.find(crate)->id();

    const dx::Issue whole = issue(run("reportIssue", {{"args", {crate}}, {"m", "No collision"}, {"severity", "error"}}));
    CHECK(whole.node == id);
    CHECK(whole.severity == dx::Severity::Error);
    CHECK(whole.source == "user");

    // An attribute the node has, by its name alone.
    const dx::Issue attr = issue(run("reportIssue", {{"args", {crate + ".translate"}}, {"m", "Off the grid"}}));
    CHECK(attr.attr == "translate");
    CHECK(attr.component.empty());

    // A component (the node type) with an attribute in it.
    const dx::Issue comp = issue(run("reportIssue", {{"args", {crate + ".transform:rotate"}}, {"m", "Tilted"}}));
    CHECK(comp.component == "transform");
    CHECK(comp.attr == "rotate");
    CHECK(dx::componentMatches(comp.component, "transform", "", ""));

    // A namespaced attribute: "<component>:<attr>" is the attribute's whole name.
    run("addAttr", {{"args", {crate}}, {"longName", "health:max"}, {"attributeType", "double"}});
    const dx::Issue ns = issue(run("reportIssue", {{"args", {crate + ".health:max"}}, {"m", "Must be above 0"}, {"severity", "error"}}));
    CHECK(ns.attr == "health:max");
    CHECK(ns.component.empty());
    CHECK(dx::componentMatches("health", "", "", "health:max"));
    CHECK(dx::attrMatches(ns, "health:max"));

    CHECK(dx::issuesOf(id).size() == 4);
    CHECK(run("listIssues", {{"severity", "error"}}).size() == 2);
    CHECK(run("clearIssues", {{"severity", "warning"}}).get<int>() == 2);
    CHECK(dx::issuesOf(id).size() == 2);

    run("rename", {{"args", {crate, "Renamed"}}});
    for (const json& i : run("listIssues"))
        CHECK(i["target"].get<std::string>().starts_with("Renamed"));
}

TEST_CASE("diagnostics: unresolved objects attach when their node appears, and detach when it goes")
{
    makeCrate();
    const json id = run("reportIssue", {{"args", {"barrel.mass"}}, {"m", "Too heavy"}});
    CHECK(issue(id).node == scene::kNoNode);
    const std::string barrel = run("createNode", {{"args", {"transform"}}, {"name", "barrel"}}).get<std::string>();
    CHECK(issue(id).node == scene::Graph::current().find(barrel)->id());
    run("delete", {{"args", {barrel}}});
    CHECK(issue(id).node == scene::kNoNode);
    run("newScene");
    CHECK(dx::issues().empty());
}

TEST_CASE("diagnostics: blocking issues refuse their commands until cleared")
{
    const std::string crate = makeCrate();
    run("reportIssue", {{"args", {crate}}, {"m", "No collision"}, {"severity", "error"}, {"blocks", {"centerPivot", "clearIssues"}}});
    CHECK(issue(json(dx::issues().back().id)).blocks == std::vector<std::string>{"centerPivot"});
    CHECK_THROWS_WITH_AS(run("centerPivot", {{"args", {crate}}}), doctest::Contains("No collision"), CommandError);
    CHECK(run("listIssues", {{"blocking", "centerPivot"}}).size() == 1);
    run("clearIssues", {{"args", {crate}}});
    CHECK_NOTHROW(run("centerPivot", {{"args", {crate}}}));
}

TEST_CASE("diagnostics: validators own what they report and replace it on each run")
{
    const std::string crate = makeCrate();
    static int runs = 0;
    runs = 0;
    ops::registerCommand({.name = "testCheckCrate",
                          .doc = "Test validator.",
                          .undoable = false,
                          .run = [crate](const Args&) -> json {
                              ++runs;
                              ops::execute("reportIssue", {{"args", {crate}}, {"m", "Run " + std::to_string(runs)}});
                              return nullptr;
                          }});
    run("addValidator", {{"args", {"crates"}}, {"command", "testCheckCrate"}, {"doc", "Crate rules"}});
    run("reportIssue", {{"m", "Scene-wide note"}, {"severity", "info"}});
    run("validate");
    run("validate", {{"args", {"crates"}}});
    CHECK(runs == 2);
    const json mine = run("listIssues", {{"source", "crates"}});
    REQUIRE(mine.size() == 1);
    CHECK(mine[0]["message"] == "Run 2");
    CHECK(run("listIssues", {{"source", "user"}}).size() == 1);
    CHECK(run("validate", {{"list", true}}).contains("crates"));
    CHECK_THROWS_AS(run("validate", {{"args", {"nope"}}}), CommandError);
    CHECK(run("removeValidator", {{"args", {"crates"}}}).get<int>() == 1);
    CHECK(run("listIssues", {{"source", "crates"}}).empty());
    ops::unregisterCommand("testCheckCrate");
    run("clearIssues");
}

TEST_CASE("diagnostics: selectIssue selects the issue's object and lists its select")
{
    const std::string crate = makeCrate();
    scene::Graph& g = scene::Graph::current();
    g.select({});
    const json id = run("reportIssue", {{"args", {crate}}, {"m", "Holes"}, {"select", "select('crate')"}});
    CHECK(issue(id).select == "select('crate')");
    const json listed = run("listIssues");
    REQUIRE(listed.size() == 1);
    CHECK(listed[0]["select"] == "select('crate')");
    run("selectIssue", {{"args", {id}}});
    REQUIRE(g.selection().size() == 1);
    CHECK(g.selection()[0] == g.find(crate)->id());
    CHECK_THROWS_AS(run("selectIssue", {{"args", {999999}}}), CommandError);
    run("clearIssues");
}
