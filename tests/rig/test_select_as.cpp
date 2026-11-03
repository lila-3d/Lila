#include "TestUtil.h"

#include "geo/Mesh.h"
#include "scene/DominantInfluence.h"
#include "scene/Rig.h"
#include "scene/SelectAs.h"
#include "scene/Skin.h"

#include <filesystem>
#include <set>

using namespace rigtest;
using namespace lila::scene;

namespace
{
/**
 * A tube skinned to clavicle > upperarm_l > lowerarm_l > hand_l, with upperarm_twist_01_l under the upper arm (its
 * own correctiveRoot holding a bicep) taking the weights round y = -25.
 */
std::string twistArm()
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Arm"}, {"radius", 8}, {"height", 120}, {"subdivisionsHeight", 24}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -60, 0)}, {"name", "clavicle_l"}});
    run("joint", {{"position", v3(0, -40, 0)}, {"name", "upperarm_l"}});
    run("joint", {{"position", v3(0, 10, 0)}, {"name", "lowerarm_l"}});
    run("joint", {{"position", v3(0, 45, 0)}, {"name", "hand_l"}});
    run("select", {{"args", {"upperarm_l"}}});
    run("joint", {{"position", v3(0, -15, 0)}, {"name", "upperarm_twist_01_l"}});
    run("joint", {{"position", v3(4, -15, 0)}, {"name", "upperarm_bicep_l"}});
    run("select", {{"args", {"upperarm_l"}}});
    run("joint", {{"position", v3(0, -38, 0)}, {"name", "upperarm_correctiveRoot_l"}});
    run("joint", {{"position", v3(0, -38, 6)}, {"name", "upperarm_fwd_l"}});
    run("skinCluster", {{"args", {"clavicle_l", mesh}}});
    return mesh;
}

/** A face of the mesh that joint dominates. */
uint32_t faceOf(const std::string& mesh, const std::string& joint)
{
    graph().evaluate();
    std::vector<uint32_t> faces = rig::dominatedFaces(graph(), id(mesh), id(joint));
    REQUIRE_FALSE(faces.empty());
    return faces[faces.size() / 2];
}
} // namespace

TEST_CASE("helper joint names")
{
    for (const char* helper : {"upperarm_twist_01_l", "upperarm_twistCor_02_r", "lowerarm_correctiveRoot_l", "lowerarm_in_l",
                               "spine_04_latissimus_l", "calf_kneeBack_r", "wrist_inner_l", "thigh_fwd_lwr_l", "rig:upperarm_twist_01_l"})
        CHECK_MESSAGE(rig::isHelperName(helper), helper);
    for (const char* bone : {"upperarm_l", "index_01_l", "interaction", "spine_05", "head", "ball_r", "thumb_03_r", "pinky_metacarpal_l"})
        CHECK_FALSE_MESSAGE(rig::isHelperName(bone), bone);
}

TEST_CASE("selectAs redirects through renames, files and undo")
{
    twistArm();
    CHECK(run("selectAs", {{"args", {"upperarm_twist_01_l"}}})["upperarm_twist_01_l"].is_null());
    run("selectAs", {{"args", {"upperarm_twist_01_l"}}, {"target", "upperarm_l"}});
    CHECK(rig::selectAsTarget(graph(), id("upperarm_twist_01_l")) == id("upperarm_l"));
    CHECK(rig::selectAsTarget(graph(), id("upperarm_l")) == id("upperarm_l"));
    CHECK_THROWS(run("selectAs", {{"args", {"upperarm_l"}}, {"target", "upperarm_l"}}));

    // Chains follow to the end, and a loop stops instead of spinning.
    run("selectAs", {{"args", {"upperarm_bicep_l"}}, {"target", "upperarm_twist_01_l"}});
    CHECK(rig::selectAsTarget(graph(), id("upperarm_bicep_l")) == id("upperarm_l"));
    run("selectAs", {{"args", {"upperarm_l"}}, {"target", "upperarm_bicep_l"}});
    NodeId looped = rig::selectAsTarget(graph(), id("upperarm_bicep_l"));
    CHECK((looped == id("upperarm_l") || looped == id("upperarm_twist_01_l") || looped == id("upperarm_bicep_l")));
    run("undo");
    CHECK(rig::selectAsOf(graph(), id("upperarm_l")) == kNoNode);

    run("rename", {{"args", {"upperarm_l", "arm_upper_l"}}});
    CHECK(run("selectAs", {{"args", {"upperarm_twist_01_l"}}})["upperarm_twist_01_l"] == "arm_upper_l");
    auto path = (std::filesystem::temp_directory_path() / "lila_rig_tests" / "selectas.lilaa").string();
    run("saveScene", {{"args", {path}}});
    run("openScene", {{"args", {path}}});
    CHECK(rig::selectAsTarget(graph(), id("upperarm_twist_01_l")) == id("arm_upper_l"));

    run("selectAs", {{"args", {"upperarm_twist_01_l"}}, {"clear", true}});
    CHECK(rig::selectAsOf(graph(), id("upperarm_twist_01_l")) == kNoNode);
    run("undo");
    CHECK(rig::selectAsOf(graph(), id("upperarm_twist_01_l")) == id("arm_upper_l"));
}

TEST_CASE("the auto rule sends helpers to their limb bone")
{
    twistArm();
    run("select", {{"args", json::array()}});
    json would = run("autoSelectAs", {{"query", true}});
    CHECK(would["upperarm_twist_01_l"] == "upperarm_l");
    CHECK(would["upperarm_bicep_l"] == "upperarm_l");
    CHECK(would["upperarm_correctiveRoot_l"] == "upperarm_l");
    CHECK(would["upperarm_fwd_l"] == "upperarm_l");
    CHECK_FALSE(would.contains("lowerarm_l"));
    CHECK_FALSE(would.contains("hand_l"));
    CHECK(rig::selectAsOf(graph(), id("upperarm_twist_01_l")) == kNoNode);

    // What a template or the user set is kept unless overwritten.
    run("selectAs", {{"args", {"upperarm_fwd_l"}}, {"target", "clavicle_l"}});
    CHECK(rig::applyAutoSelectAs(graph(), {id("clavicle_l")}) == 3);
    CHECK(rig::selectAsOf(graph(), id("upperarm_fwd_l")) == id("clavicle_l"));
    CHECK(rig::selectAsOf(graph(), id("upperarm_twist_01_l")) == id("upperarm_l"));
    run("autoSelectAs", {{"overwrite", true}});
    CHECK(rig::selectAsOf(graph(), id("upperarm_fwd_l")) == id("upperarm_l"));

    // A twist joint inside the main chain keeps its children: only leaves under a helper count as helpers.
    run("newScene");
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "thigh_l"}});
    run("joint", {{"position", v3(0, -20, 0)}, {"name", "thigh_twist_01_l"}});
    run("joint", {{"position", v3(0, -40, 0)}, {"name", "calf_l"}});
    run("joint", {{"position", v3(0, -80, 0)}, {"name", "foot_l"}});
    run("select", {{"args", json::array()}});
    json chain = run("autoSelectAs", {{"query", true}});
    CHECK(chain["thigh_twist_01_l"] == "thigh_l");
    CHECK_FALSE(chain.contains("calf_l"));
}

TEST_CASE("candidates under the cursor: redirect, weighted, parent, child, influences")
{
    std::string mesh = twistArm();
    // With joints selected only those and what is under them change.
    run("select", {{"args", {"upperarm_twist_01_l"}}});
    CHECK(run("autoSelectAs").size() == 2);
    CHECK(rig::selectAsOf(graph(), id("upperarm_correctiveRoot_l")) == kNoNode);
    run("select", {{"args", json::array()}});
    run("autoSelectAs");
    const uint32_t face = faceOf(mesh, "upperarm_twist_01_l");
    json c = run("selectionCandidates", {{"args", {mesh}}, {"face", face}, {"position", v3(0, -20, 8)}});
    REQUIRE(c.size() >= 4);
    CHECK(c[0]["joint"] == "upperarm_l");
    CHECK(c[0]["role"] == "redirect");
    CHECK(c[1]["joint"] == "upperarm_twist_01_l");
    CHECK(c[1]["role"] == "weighted");
    CHECK(c[1]["weight"].get<double>() > 0.3);
    CHECK(c[2]["joint"] == "clavicle_l");
    CHECK(c[2]["role"] == "parent");
    // The helpers under the upper arm click back to it, so the child is where the limb goes on.
    CHECK(c[3]["joint"] == "lowerarm_l");
    CHECK(c[3]["role"] == "child");
    std::set<std::string> seen;
    double last = 2.0;
    for (size_t i = 0; i < c.size(); ++i)
    {
        CHECK(seen.insert(c[i]["joint"].get<std::string>()).second);
        if (c[i]["role"] == "influence")
        {
            CHECK(c[i]["weight"].get<double>() >= rig::kSignificantInfluence);
            CHECK(c[i]["weight"].get<double>() <= last);
            last = c[i]["weight"].get<double>();
        }
    }

    // Without the redirect the dominant joint is the click: no redirect entry, the twist's own parent and child.
    json raw = run("selectionCandidates", {{"args", {mesh}}, {"face", face}, {"redirect", false}});
    CHECK(raw[0]["joint"] == "upperarm_twist_01_l");
    CHECK(raw[0]["role"] == "weighted");
    CHECK(raw[1]["joint"] == "upperarm_l");
    CHECK(raw[1]["role"] == "parent");

    // A face of a plain bone has no redirect entry.
    json fore = run("selectionCandidates", {{"args", {mesh}}, {"face", faceOf(mesh, "lowerarm_l")}});
    CHECK(fore[0]["joint"] == "lowerarm_l");
    CHECK(fore[0]["role"] == "weighted");
    CHECK(fore[1]["joint"] == "upperarm_l");
    CHECK(fore[1]["role"] == "parent");
    CHECK(fore[2]["joint"] == "hand_l");
    CHECK(fore[2]["role"] == "child");
}
