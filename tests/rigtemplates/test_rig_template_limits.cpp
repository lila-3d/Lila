#include "ops/Command.h"
#include "extensions/ExtensionManager.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/Transform.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>

using namespace lila;
using namespace lila::scene;

namespace limit_tests
{
json call(const char* cmd, json args = json::object()) { return ops::execute(cmd, args); }

void loadRigTemplates()
{
    auto& em = extensions::ExtensionManager::get();
    if (!em.isLoaded("RigTemplates"))
        REQUIRE(em.load(LILA_TEST_RIG_TEMPLATES_PACKAGE) == "RigTemplates");
}

Node& joint(const char* name)
{
    Node* n = Graph::current().find(name);
    REQUIRE_MESSAGE(n, name);
    return *n;
}

std::filesystem::path scratch(const char* file)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_rig_templates_tests" / "limits";
    std::filesystem::create_directories(dir);
    return dir / file;
}

/** Rig element node type -> count, for a buildRigSkeleton result. */
std::map<std::string, int> kinds(const json& built)
{
    std::map<std::string, int> out;
    for (const json& e : built["rig"])
        ++out[Graph::current().find(e.get<std::string>())->type().name];
    return out;
}
} // namespace limit_tests

using namespace limit_tests;

TEST_CASE("rig templates: IK limits are template data, saved, loaded and checked")
{
    loadRigTemplates();
    const json info = call("rigTemplateInfo", {{"args", {"UE5_Manny"}}});
    const json& limits = info["rig"]["limits"];
    CHECK(limits["calf_l"]["type"] == "hinge");
    CHECK(limits["calf_l"]["axis"] == "y");
    CHECK(limits["lowerarm_r"]["type"] == "hinge");
    CHECK(limits["thigh_l"]["type"] == "cone");
    CHECK(limits["upperarm_r"]["type"] == "cone");
    CHECK(limits["spine_03"]["cone"].get<double>() == doctest::Approx(15.0));

    json copy = info;
    copy["id"] = "LimitsCopy";
    copy["rig"]["limits"]["head"] = {{"type", "cone"}, {"cone", 30.0}, {"stiffness", 0.25}, {"preferredAngle", {0.0, 5.0, 0.0}}};
    const auto file = scratch("limits_copy.lilarig");
    std::ofstream(file) << copy.dump();
    CHECK(call("loadRigTemplate", {{"args", {file.generic_string()}}}) == "LimitsCopy");
    const auto saved = scratch("limits_saved.lilarig");
    call("saveRigTemplate", {{"args", {"LimitsCopy", saved.generic_string()}}});
    std::ifstream in(saved);
    CHECK(json::parse(in)["rig"]["limits"] == copy["rig"]["limits"]);

    auto refused = [&](const char* marker, json limit, const char* message) {
        json bad = info;
        bad["id"] = "BadLimits";
        bad["rig"]["limits"][marker] = std::move(limit);
        const auto f = scratch("bad_limits.lilarig");
        std::ofstream(f) << bad.dump();
        CHECK_THROWS_WITH(call("loadRigTemplate", {{"args", {f.generic_string()}}}), doctest::Contains(message));
    };
    refused("no_such_marker", {{"type", "cone"}}, "unknown marker no_such_marker");
    refused("calf_l", {{"type", "ball"}}, "type should be none, hinge or cone");
    refused("calf_l", {{"axis", "w"}}, "axis should be x, y or z");
    refused("calf_l", {{"min", "low"}}, "min should be a number");
    refused("calf_l", {{"twist", 1.0}}, "unknown key twist");
}

TEST_CASE("rig templates: a built rig carries the template's IK limits")
{
    loadRigTemplates();
    call("newScene");
    call("buildRigSkeleton", {{"args", {"UE5_Manny"}}});
    Node& calf = joint("calf_l");
    CHECK(calf.as<int64_t>(Joint::kLimitType) == 1); // hinge
    CHECK(calf.as<int64_t>(Joint::kLimitAxis) == 1); // y
    CHECK(calf.as<double>(Joint::kLimitMin) == doctest::Approx(-150.0));
    CHECK(calf.as<double>(Joint::kLimitMax) == doctest::Approx(5.0));
    CHECK(calf.as<glm::dvec3>(Joint::kPreferredAngle).y == doctest::Approx(-10.0));
    Node& thigh = joint("thigh_r");
    CHECK(thigh.as<int64_t>(Joint::kLimitType) == 2); // cone
    CHECK(thigh.as<double>(Joint::kConeAngle) == doctest::Approx(90.0));
    CHECK(joint("spine_01").as<double>(Joint::kStiffness) == doctest::Approx(0.4));
    CHECK(joint("lowerarm_r").as<int64_t>(Joint::kLimitType) == 1);
    CHECK(joint("pelvis").as<int64_t>(Joint::kLimitType) == 0);

    // The leg's IK still reaches with the knee hinged: lifting the foot bends the knee within its range.
    Graph& g = Graph::current();
    g.evaluate();
    const glm::dvec3 ankle0(joint("foot_l").as<glm::dmat4>(Transform::kWorldMatrix)[3]);
    call("ikPose", {{"args", {"foot_l"}}, {"moveBy", json::array({0, 30, 0})}});
    g.evaluate();
    const glm::dvec3 ankle(joint("foot_l").as<glm::dmat4>(Transform::kWorldMatrix)[3]);    CHECK(glm::length(ankle - (ankle0 + glm::dvec3(0, 30, 0))) < 1e-3);

    // Without the rig, joints are left free.
    call("newScene");
    call("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"rig", false}});
    CHECK(joint("calf_l").as<int64_t>(Joint::kLimitType) == 0);
}

TEST_CASE("rig templates: limbs build jointed or as rubber hoses")
{
    loadRigTemplates();
    call("newScene");
    const json jointed = call("buildRigSkeleton", {{"args", {"UE5_Manny"}}});
    CHECK(kinds(jointed)["hoseLimb"] == 0);
    CHECK(joint("lowerarm_l").as<bool>(Joint::kDeform));

    // One limb by name: its joints hand the skin to the hose joints; the other arm stays jointed.
    call("newScene");
    const json one = call("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"hose", "arm_l"}});
    CHECK(kinds(one)["hoseLimb"] == 1);
    CHECK(kinds(one)["ikChain"] == 4);
    for (const char* n : {"upperarm_l", "lowerarm_l", "hand_l", "upperarm_twist_01_l", "lowerarm_twist_01_l"})
        CHECK_FALSE_MESSAGE(joint(n).as<bool>(Joint::kDeform), n);
    CHECK(joint("lowerarm_r").as<bool>(Joint::kDeform));
    CHECK(joint("index_01_l").as<bool>(Joint::kDeform));
    CHECK(joint("upperarm_l_hose_01").as<bool>(Joint::kDeform));
    CHECK(joint("upperarm_l_hose_12").as<bool>(Joint::kDeform));

    call("newScene");
    CHECK(kinds(call("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"hose", "all"}}))["hoseLimb"] == 4);
    call("newScene");
    CHECK_THROWS_WITH(call("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"hose", "arm_l, tail"}}), doctest::Contains("no IK chain 'tail'"));

    // A template can make a limb a hose by default, with its own joint count; hose=none overrides it.
    json info = call("rigTemplateInfo", {{"args", {"UE5_Manny"}}});
    info["id"] = "HoseLegs";
    for (json& c : info["rig"]["ikChains"])
        if (c["name"] == "leg_r")
            c["hose"] = {{"enabled", true}, {"joints", 8}, {"amount", 0.5}};
    const auto file = scratch("hose_legs.lilarig");
    std::ofstream(file) << info.dump();
    CHECK(call("loadRigTemplate", {{"args", {file.generic_string()}}}) == "HoseLegs");
    call("newScene");
    const json legs = call("buildRigSkeleton", {{"args", {"HoseLegs"}}});
    CHECK(kinds(legs)["hoseLimb"] == 1);
    Node& hose = joint("thigh_r_hose");
    CHECK(hose.as<double>(hose.type().attrIndex("hose")) == doctest::Approx(0.5));
    CHECK(Graph::current().find("thigh_r_hose_08"));
    CHECK_FALSE(Graph::current().find("thigh_r_hose_09"));
    call("newScene");
    CHECK(kinds(call("buildRigSkeleton", {{"args", {"HoseLegs"}}, {"hose", "none"}}))["hoseLimb"] == 0);
}
