#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "scene/BatchRename.h"
#include "scene/Graph.h"

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

namespace
{
std::string makeTransform(const char* name, const char* parent = nullptr)
{
    json call = {{"args", {"transform"}}, {"name", name}, {"skipSelect", true}};
    if (parent)
        call["parent"] = parent;
    return run("createNode", call).get<std::string>();
}

std::vector<std::string> names(const json& plans)
{
    std::vector<std::string> out;
    for (const json& p : plans)
        out.push_back(p["to"].get<std::string>());
    return out;
}
} // namespace

TEST_CASE("batchRenamed applies each rule")
{
    using scene::batchRenamed;
    scene::BatchRename r;
    r.name = "tail_##";
    CHECK(batchRenamed(r, "joint7", 0, "", "joint") == "tail_01");
    CHECK(batchRenamed(r, "joint7", 14, "", "joint") == "tail_15");

    r = {.find = "left", .replace = "Right"};
    CHECK(batchRenamed(r, "LeftArm_left", 0, "", "") == "RightArm_Right");
    r.matchCase = true;
    CHECK(batchRenamed(r, "LeftArm_left", 0, "", "") == "LeftArm_Right");

    r = {.find = "^([a-z]+)_([A-Z]+)$", .replace = "$2_$1", .regex = true};
    CHECK(batchRenamed(r, "arm_L", 0, "", "") == "L_arm");

    r = {.stripDigits = true};
    CHECK(batchRenamed(r, "Arm_02", 0, "", "") == "Arm");
    CHECK(batchRenamed(r, "Arm", 0, "", "") == "Arm");
    r = {.stripStart = 2, .stripEnd = 1};
    CHECK(batchRenamed(r, "L_ArmX", 0, "", "") == "Arm");

    r = {.caseStyle = "snake_case", .prefix = "{parent}_", .suffix = "_{type}"};
    CHECK(batchRenamed(r, "UpperArm", 0, "Body", "joint") == "Body_upper_arm_joint");
    r = {.prefix = "#_", .start = 10, .step = 5};
    CHECK(batchRenamed(r, "a", 2, "", "") == "20_a");

    CHECK(scene::sanitizeName("my arm-2") == "my_arm_2");
    CHECK(scene::sanitizeName("2arm") == "_2arm");
}

TEST_CASE("batchRename names a hierarchy chain and undoes in one step")
{
    run("newScene");
    makeTransform("Root");
    makeTransform("Mid", "Root");
    makeTransform("Tip", "Mid");
    json plans = run("batchRename", {{"args", {"Root"}}, {"name", "tail_##"}, {"hierarchy", true}, {"order", "outliner"}});
    CHECK(names(plans) == std::vector<std::string>{"tail_01", "tail_02", "tail_03"});
    CHECK(scene::Graph::current().find("|tail_01|tail_02|tail_03"));
    run("undo");
    CHECK(scene::Graph::current().find("|Root|Mid|Tip"));
}

TEST_CASE("batchRename follows selection order, previews and swaps names")
{
    run("newScene");
    makeTransform("A");
    makeTransform("B");
    makeTransform("C");
    run("select", {{"args", {"C", "A", "B"}}});
    json preview = run("batchRename", {{"name", "n#"}, {"preview", true}});
    CHECK(names(preview) == std::vector<std::string>{"n1", "n2", "n3"});
    CHECK(scene::Graph::current().find("A"));

    run("newScene");
    makeTransform("n1");
    makeTransform("n2");
    // Each asks for the other's name; both land there because every name is freed first.
    json swapped = run("batchRename", {{"args", {"n1", "n2"}}, {"name", "n#"}, {"start", 2}, {"step", -1}});
    CHECK(names(swapped) == std::vector<std::string>{"n2", "n1"});
    CHECK(swapped[0]["status"] == "renamed");

    makeTransform("Taken");
    json clash = run("batchRename", {{"args", {"n1"}}, {"name", "Taken"}, {"preview", true}});
    CHECK(clash[0]["status"] == "clash");
}

TEST_CASE("namingConvention fix covers a hierarchy and keeps side suffixes")
{
    run("newScene");
    makeTransform("root_grp");
    makeTransform("dog_leg_b", "root_grp");
    makeTransform("wheel_lf");
    scene::Graph& g = scene::Graph::current();
    g.setNamingPolicy(scene::NamingPolicy::fromJson({{"enabled", true}}));
    json fixed = run("namingConvention", {{"args", {"root_grp"}}, {"fix", true}});
    CHECK(fixed.size() == 2);
    CHECK(g.find("|RootGrp|DogLeg_B"));
    CHECK(g.find("wheel_lf"));
    run("namingConvention", {{"fix", true}});
    CHECK(g.find("Wheel_LF"));
    g.setNamingPolicy({});
}
