#include "TestUtil.h"

#include "core/Workspace.h"
#include "scene/Poses.h"
#include "scene/Rig.h"

#include <filesystem>

using namespace rigtest;
using namespace lila::scene;
namespace fs = std::filesystem;

namespace
{
void cleanFileTiers()
{
    std::error_code ec;
    fs::remove_all(poses::tierDir(poses::Tier::User), ec);
}

/** pelvis with a two-joint leg on each side, and an empty pose library. */
void legs()
{
    run("newScene");
    poses::clearScene();
    cleanFileTiers();
    run("joint", {{"position", v3(0, 100, 0)}, {"name", "pelvis"}});
    run("joint", {{"position", v3(10, 90, 0)}, {"name", "thigh_l"}});
    run("joint", {{"position", v3(10, 50, 0)}, {"name", "calf_l"}});
    run("select", {{"args", {"pelvis"}}});
    run("joint", {{"position", v3(-10, 90, 0)}, {"name", "thigh_r"}});
    run("joint", {{"position", v3(-10, 50, 0)}, {"name", "calf_r"}});
    run("select", {{"args", {"pelvis"}}});
}

glm::dvec3 rotate(const std::string& n) { return node(n).as<glm::dvec3>(Transform::kRotate); }
glm::dvec3 translate(const std::string& n) { return node(n).as<glm::dvec3>(Transform::kTranslate); }

/** Same rotation, whatever Euler angles express it. */
void checkSameRotation(const glm::dvec3& a, const glm::dvec3& b)
{
    glm::dmat4 ma = eulerToMatrix(a, RotateOrder::XYZ), mb = eulerToMatrix(b, RotateOrder::XYZ);
    for (int c = 0; c < 3; ++c)
        checkNear(glm::dvec3(ma[c]), glm::dvec3(mb[c]), 1e-6);
}

} // namespace

TEST_CASE("poses save by name and apply back, with blend and channel filters")
{
    legs();
    run("setAttr", {{"args", {"thigh_l.rotate", v3(10, 20, 30)}}});
    run("setAttr", {{"args", {"pelvis.translate", v3(1, 102, 3)}}});
    json saved = run("savePose", {{"args", {"crouch"}}, {"thumbnail", false}});
    CHECK(saved["character"] == "pelvis");
    CHECK(saved["tier"] == "scene");
    CHECK(saved["nodes"].get<int>() == 5);

    run("setAttr", {{"args", {"thigh_l.rotate", v3(0, 0, 0)}}});
    run("setAttr", {{"args", {"pelvis.translate", v3(0, 100, 0)}}});
    json r = run("applyPose", {{"args", {"crouch"}}});
    CHECK(r["missing"].empty());
    CHECK(r["nodes"].get<int>() == 2);
    checkSameRotation(rotate("thigh_l"), {10, 20, 30});
    checkNear(translate("pelvis"), {1, 102, 3});

    // Halfway: translate lerps, rotation slerps.
    run("setAttr", {{"args", {"thigh_l.rotate", v3(0, 0, 0)}}});
    run("setAttr", {{"args", {"pelvis.translate", v3(0, 100, 0)}}});
    run("setAttr", {{"args", {"calf_l.rotate", v3(0, 0, 60)}}});
    run("savePose", {{"args", {"bend"}}, {"thumbnail", false}});
    run("setAttr", {{"args", {"calf_l.rotate", v3(0, 0, 0)}}});
    run("setAttr", {{"args", {"pelvis.translate", v3(0, 0, 0)}}});
    run("applyPose", {{"args", {"bend"}}, {"weight", 0.5}});
    checkSameRotation(rotate("calf_l"), {0, 0, 30});
    checkNear(translate("pelvis"), {0, 50, 0});

    // A channel filter leaves the others alone.
    run("setAttr", {{"args", {"calf_l.rotate", v3(0, 0, 0)}}});
    run("setAttr", {{"args", {"pelvis.translate", v3(0, 0, 0)}}});
    run("applyPose", {{"args", {"bend"}}, {"channels", {"translate"}}});
    checkNear(rotate("calf_l"), {0, 0, 0});
    checkNear(translate("pelvis"), {0, 100, 0});

    // One undo step per apply.
    run("applyPose", {{"args", {"bend"}}});
    checkSameRotation(rotate("calf_l"), {0, 0, 60});
    run("undo");
    checkNear(rotate("calf_l"), {0, 0, 0});
}

TEST_CASE("poses apply across namespaces and aliases, and report missing names")
{
    legs();
    poses::Pose p;
    p.name = "probe";
    p.character = "pelvis";
    p.nodes = {{"thigh_l", {{"rotate", json::array({0.0, 0.0, std::sin(0.25), std::cos(0.25)})}}},
               {"Hips", {{"translate", v3(0, 120, 0)}}},
               {"tail", {{"rotate", json::array({0.0, 0.0, 0.0, 1.0})}}}};
    graph().rename(node("thigh_l").id(), "rig:thigh_l");
    graph().rename(node("pelvis").id(), "rig:pelvis");

    poses::ApplyOptions o;
    o.aliases = {{"Hips", {"pelvis"}}};
    poses::ApplyReport r = poses::apply(graph(), p, o);
    CHECK(r.missing == std::vector<std::string>{"tail"});
    checkSameRotation(rotate("rig:thigh_l"), {0, 0, 28.64788975654116});
    checkNear(translate("rig:pelvis"), {0, 120, 0});

    // Character ids ignore the namespace too.
    CHECK(poses::characterOf(graph(), node("calf_l").id()).id == "pelvis");
    CHECK(poses::characterOf(graph(), node("calf_l").id()).nameSpace == "rig");
}

TEST_CASE("mirroring swaps sides and reflects the centre")
{
    CHECK(poses::mirrorNodeName("thigh_l") == "thigh_r");
    CHECK(poses::mirrorNodeName("thigh_r") == "thigh_l");
    CHECK(poses::mirrorNodeName("LeftHand") == "RightHand");
    CHECK(poses::mirrorNodeName("l_arm") == "r_arm");
    CHECK(poses::mirrorNodeName("pelvis") == "pelvis");

    legs();
    run("setAttr", {{"args", {"thigh_l.rotate", v3(0, 0, 40)}}});
    run("setAttr", {{"args", {"calf_l.translate", v3(0, -40, 5)}}});
    run("setAttr", {{"args", {"pelvis.rotate", v3(10, 20, 30)}}});
    run("setAttr", {{"args", {"pelvis.translate", v3(4, 100, 0)}}});
    run("mirrorPose", json::object());
    // Sided joints trade values: rotation as is, translate negated.
    checkSameRotation(rotate("thigh_r"), {0, 0, 40});
    checkSameRotation(rotate("thigh_l"), {0, 0, 0});
    checkNear(translate("calf_r"), {0, 40, -5});
    // The centre reflects across the YZ plane.
    checkSameRotation(rotate("pelvis"), {10, -20, -30});
    checkNear(translate("pelvis"), {-4, 100, 0});

    // A stored pose mirrors into a new one.
    run("savePose", {{"args", {"step"}}, {"thumbnail", false}});
    json m = run("mirrorPose", {{"args", {"step"}}});
    CHECK(m["name"] == "step_mirror");
    auto e = poses::find("pelvis", "step_mirror");
    REQUIRE(e);
    CHECK(e->pose.nodes.contains("thigh_l"));
}

TEST_CASE("pose tiers: scene shadows workspace and user, template is last")
{
    legs();
    fs::path ws = fs::temp_directory_path() / "lila_rig_tests_ws";
    fs::remove_all(ws);
    fs::create_directories(ws / ".lila");
    Workspace::get().open(ws);

    run("setAttr", {{"args", {"thigh_l.rotate", v3(0, 0, 10)}}});
    run("savePose", {{"args", {"idle"}}, {"tier", "user"}, {"thumbnail", false}});
    CHECK(fs::exists(poses::tierDir(poses::Tier::User) / "pelvis" / "idle.json"));
    run("setAttr", {{"args", {"thigh_l.rotate", v3(0, 0, 20)}}});
    run("savePose", {{"args", {"idle"}}, {"tier", "workspace"}, {"thumbnail", false}});
    CHECK(fs::exists(ws / ".lila" / "poses" / "pelvis" / "idle.json"));
    run("setAttr", {{"args", {"thigh_l.rotate", v3(0, 0, 30)}}});
    run("savePose", {{"args", {"idle"}}, {"thumbnail", false}});

    poses::Pose t;
    t.name = "idle";
    t.character = "pelvis";
    t.nodes = {{"thigh_l", {{"rotate", v3(0, 0, 40)}}}};
    poses::registerTemplatePose(t, "rigTemplates");
    t.name = "aPose";
    poses::registerTemplatePose(t, "rigTemplates");

    CHECK(poses::find("pelvis", "idle")->tier == poses::Tier::Scene);
    json listed = run("listPoses");
    REQUIRE(listed.size() == 2);
    CHECK(listed[0]["name"] == "aPose");
    CHECK(listed[0]["tier"] == "template");
    CHECK(listed[1]["tier"] == "scene");
    CHECK(run("listPoses", {{"all", true}}).size() == 5);

    auto applied = [](const char* tier) {
        run("applyPose", {{"args", {"idle"}}, {"tier", tier}});
        return rotate("thigh_l").z;
    };
    CHECK(applied("user") == doctest::Approx(10));
    CHECK(applied("workspace") == doctest::Approx(20));
    CHECK(applied("template") == doctest::Approx(40));

    run("deletePose", {{"args", {"idle"}}});
    CHECK(poses::find("pelvis", "idle")->tier == poses::Tier::Workspace);
    run("undo");
    CHECK(poses::find("pelvis", "idle")->tier == poses::Tier::Scene);
    run("deletePose", {{"args", {"idle"}}, {"tier", "workspace"}});
    CHECK(poses::find("pelvis", "idle")->tier == poses::Tier::Scene);
    run("renamePose", {{"args", {"idle", "rest"}}, {"tier", "user"}});
    CHECK(fs::exists(poses::tierDir(poses::Tier::User) / "pelvis" / "rest.json"));
    CHECK(poses::find("pelvis", "rest")->tier == poses::Tier::User);

    poses::unregisterOwned("rigTemplates");
    CHECK_FALSE(poses::find("pelvis", "aPose"));
    Workspace::get().close();
    CHECK(poses::tierDir(poses::Tier::Workspace).empty());
}

TEST_CASE("reserved poses can't be renamed and need force to delete")
{
    legs();
    run("savePose", {{"args", {"bind"}}, {"thumbnail", false}});
    CHECK_THROWS(run("deletePose", {{"args", {"bind"}}}));
    CHECK_THROWS(run("renamePose", {{"args", {"bind", "other"}}}));
    run("savePose", {{"args", {"other"}}, {"thumbnail", false}});
    CHECK_THROWS(run("renamePose", {{"args", {"other", "Ref"}}}));
    json listed = run("listPoses");
    REQUIRE(listed.size() == 2);
    CHECK(listed[0]["name"] == "bind");
    CHECK(listed[0]["reserved"] == true);
    run("deletePose", {{"args", {"bind"}}, {"force", true}});
    CHECK_FALSE(poses::find("pelvis", "bind"));
}

TEST_CASE("scene poses round-trip through json and clear on a new scene")
{
    legs();
    run("savePose", {{"args", {"keep"}}, {"tags", {"walk", "contact"}}, {"thumbnail", false}});
    poses::Pose with = poses::find("pelvis", "keep")->pose;
    with.thumbnail = std::string("\x89PNG\r\n\x1a\n\0\xff", 10);
    json j = with.toJson();
    poses::Pose back = poses::Pose::fromJson(j);
    CHECK(back.thumbnail == with.thumbnail);
    CHECK(back.tags == std::vector<std::string>{"walk", "contact"});
    CHECK(back.nodes == with.nodes);

    json scene = poses::sceneToJson();
    REQUIRE(scene.size() == 1);
    run("newScene");
    CHECK(poses::sceneToJson().empty());
    poses::sceneFromJson(scene);
    CHECK(poses::find("pelvis", "keep"));
    poses::clearScene();
}
