#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/OutlinerFilter.h"

#include <algorithm>

using namespace lila;
using json = nlohmann::json;
namespace outliner = lila::ui::outliner;
using outliner::Filter;
using outliner::globMatch;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

scene::NodeId idOf(const std::string& name) { return scene::Graph::current().find(name)->id(); }

const scene::NodeType* typeOf(const char* name) { return scene::types::find(name); }

bool passes(const Filter& f, std::string_view name, std::string_view path = {}, const char* type = "transform")
{
    return f.matches(name, path.empty() ? std::string("|") + std::string(name) : path, typeOf(type));
}
} // namespace

TEST_CASE("outliner globs: stars, single characters and classes, ignoring case")
{
    CHECK(globMatch("pCube*", "pcube1"));
    CHECK(globMatch("*cube*", "polyCubeShape"));
    CHECK_FALSE(globMatch("pCube*", "xpCube1"));
    CHECK(globMatch("arm_?", "arm_L"));
    CHECK_FALSE(globMatch("arm_?", "arm_LL"));
    CHECK(globMatch("arm_[lr]", "arm_R"));
    CHECK(globMatch("joint[0-9]", "joint7"));
    CHECK_FALSE(globMatch("joint[0-9]", "jointA"));
    CHECK(globMatch("joint[!0-9]", "jointA"));
    CHECK(globMatch("joint[^0-9]", "jointA"));
    CHECK(globMatch("a[b", "a[b"));
    CHECK(globMatch("*", ""));
    CHECK(globMatch("a*b*c", "aXXbYYc"));
    CHECK_FALSE(globMatch("a*b*c", "aXXbYY"));
}

TEST_CASE("outliner filter terms: substrings, globs, alternatives and negation")
{
    CHECK(Filter("").empty());
    CHECK(Filter(" , ").empty());
    CHECK(passes(Filter(""), "anything"));

    Filter sub("CUBE");
    CHECK(passes(sub, "pCube1"));
    CHECK_FALSE(passes(sub, "pSphere1"));

    Filter glob("p*1");
    CHECK(passes(glob, "pCube1"));
    CHECK_FALSE(passes(glob, "pCube12"));

    Filter any("cube, sphere");
    CHECK(passes(any, "pCube1"));
    CHECK(passes(any, "pSphere1"));
    CHECK_FALSE(passes(any, "pCone1"));
    CHECK(passes(Filter("cube sphere"), "pSphere1"));

    Filter notShapes("!*Shape");
    CHECK(passes(notShapes, "pCube1"));
    CHECK_FALSE(passes(notShapes, "pCubeShape"));

    Filter both("p* !*Shape");
    CHECK(passes(both, "pCube1"));
    CHECK_FALSE(passes(both, "pCubeShape"));
    CHECK_FALSE(passes(both, "locator1"));
}

TEST_CASE("outliner filter terms with a pipe match the full path")
{
    Filter f("|rig|*");
    CHECK(passes(f, "arm", "|rig|arm"));
    CHECK_FALSE(passes(f, "arm", "|geo|arm"));
    CHECK_FALSE(passes(Filter("rig"), "arm", "|rig|arm"));
    CHECK(passes(Filter("rig|arm"), "arm", "|rig|arm"));
}

TEST_CASE("outliner type filter includes derived types and can be negated")
{
    CHECK(passes(Filter("type:mesh"), "body", "|body", "mesh"));
    CHECK_FALSE(passes(Filter("type:mesh"), "grp", "|grp", "transform"));
    CHECK(passes(Filter("TYPE:Mesh"), "body", "|body", "mesh"));
    CHECK(passes(Filter("type:mesh type:locator"), "loc", "|loc", "locator"));
    CHECK(passes(Filter("type:*Constraint"), "c", "|c", "parentConstraint"));
    CHECK_FALSE(passes(Filter("!type:mesh"), "body", "|body", "mesh"));
    // Constraint kinds derive from the constraint base, which the filter names.
    CHECK(passes(Filter("type:constraint"), "c", "|c", "parentConstraint"));
    CHECK(passes(Filter("type:mesh bo"), "body", "|body", "mesh"));
    CHECK_FALSE(passes(Filter("type:mesh xx"), "body", "|body", "mesh"));

    Filter dropdown("bo");
    dropdown.setType("mesh");
    CHECK(passes(dropdown, "body", "|body", "mesh"));
    CHECK_FALSE(passes(dropdown, "bone", "|bone", "joint"));
}

TEST_CASE("outliner regex mode")
{
    Filter re("^p(cube|sphere)\\d+$", true);
    REQUIRE(re.valid());
    CHECK(passes(re, "pCube1"));
    CHECK(passes(re, "pSphere22"));
    CHECK_FALSE(passes(re, "pCubeShape1"));
    CHECK(passes(Filter("^\\|rig\\|", true), "arm", "|rig|arm"));

    Filter bad("(unclosed", true);
    CHECK_FALSE(bad.valid());
    CHECK_FALSE(bad.error().empty());
    CHECK(passes(bad, "anything"));
}

TEST_CASE("scene finder fuzzy matching ranks real matches first")
{
    Filter f("lfa");
    CHECK_FALSE(passes(f, "left_foot_ankle"));
    f.setFuzzy(true);
    CHECK(passes(f, "left_foot_ankle"));
    CHECK_FALSE(passes(f, "ankle_foot"));
    const scene::NodeType* t = typeOf("transform");
    CHECK(f.score("lfa", "|lfa", t) > f.score("lfaX", "|lfaX", t));
    CHECK(f.score("lfaX", "|lfaX", t) > f.score("xlfa", "|xlfa", t));
    CHECK(f.score("xlfa", "|xlfa", t) > f.score("left_foot_ankle", "|left_foot_ankle", t));
    CHECK(outliner::fuzzyScore("lfa", "left_foot_ankle") > outliner::fuzzyScore("lfa", "xlxxxfxxxa"));
}

TEST_CASE("outliner filter keeps the ancestors of matches")
{
    run("newScene");
    run("group", {{"empty", true}, {"name", "outer"}});
    run("group", {{"empty", true}, {"name", "inner"}});
    run("spaceLocator", {{"name", "target"}});
    run("spaceLocator", {{"name", "other"}});
    run("parent", {{"args", {"inner", "outer"}}});
    run("parent", {{"args", {"target", "inner"}}});
    run("parent", {{"args", {"other", "outer"}}});
    scene::Graph& g = scene::Graph::current();

    std::vector<scene::NodeId> all = outliner::hierarchyNodes(g);
    CHECK(all.size() == 4);
    CHECK(all.front() == idOf("outer"));

    outliner::FilterRows rows = outliner::filterRows(g, Filter("targ"));
    CHECK(rows.matched == std::set<scene::NodeId>{idOf("target")});
    CHECK(rows.kept == std::set<scene::NodeId>{idOf("outer"), idOf("inner")});

    rows = outliner::filterRows(g, Filter("inner target"));
    CHECK(rows.matched == std::set<scene::NodeId>{idOf("inner"), idOf("target")});
    CHECK(rows.kept == std::set<scene::NodeId>{idOf("outer")});

    rows = outliner::filterRows(g, Filter("|outer|*|*"));
    CHECK(rows.matched == std::set<scene::NodeId>{idOf("target")});
    run("newScene");
}

TEST_CASE("outliner filter completes the type key and type names")
{
    const std::vector<std::string> types{"camera", "locator", "mesh", "polyMeshEdit", "transform"};
    using V = std::vector<std::string>;
    CHECK(outliner::filterCompletions("ty", types) == V{"type:"});
    CHECK(outliner::filterCompletions("!T", types) == V{"!type:"});
    CHECK(outliner::filterCompletions("cube", types).empty());
    CHECK(outliner::filterCompletions("", types).empty());
    CHECK(outliner::filterCompletions("type:", types).size() == types.size());
    CHECK(outliner::filterCompletions("type:mesh", types) == V{"type:mesh", "type:polyMeshEdit"});
    CHECK(outliner::filterCompletions("!TYPE:Cam", types) == V{"!type:camera"});
    CHECK(outliner::filterCompletions("type:me*", types).empty());

    std::vector<std::string> real = outliner::nodeTypeNames();
    CHECK(std::find(real.begin(), real.end(), "transform") != real.end());
    CHECK(std::is_sorted(real.begin(), real.end()));
}
