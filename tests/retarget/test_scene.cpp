#include <doctest/doctest.h>

#include "ops/Command.h"
#include "retarget/Bvh.h"
#include "retarget/SceneRetarget.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/Take.h"
#include "scene/Transform.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
Graph& graph() { return Graph::current(); }

Node& node(const std::string& name)
{
    Node* n = graph().find(name);
    REQUIRE(n);
    return *n;
}

glm::dvec3 posAt(const std::string& name, double t)
{
    graph().setTime(t);
    graph().evaluate();
    return glm::dvec3(node(name).as<glm::dmat4>(Transform::kWorldMatrix)[3]);
}

void joint(const char* name, glm::dvec3 p, const char* parent)
{
    run("joint", {{"position", json::array({p.x, p.y, p.z})}, {"name", name}, {"parent", parent}, {"skipSelect", true}});
}

/** A BVH-named source and a UE-named target four fifths its size. */
void twoBipeds()
{
    run("newScene");
    for (int k = 0; k < 2; ++k)
    {
        const bool ue = k == 1;
        const double s = ue ? 0.8 : 1.0;
        const double x = ue ? 200.0 : 0.0;
        auto P = [&](double px, double py, double pz) { return glm::dvec3(px * s + x, py * s, pz * s); };
        const char* hips = ue ? "pelvis" : "Hips";
        joint(hips, P(0, 100, 0), "");
        joint(ue ? "spine_01" : "Spine", P(0, 115, 0), hips);
        joint(ue ? "spine_02" : "Spine1", P(0, 140, 0), ue ? "spine_01" : "Spine");
        const char* chest = ue ? "spine_02" : "Spine1";
        joint(ue ? "head" : "Head", P(0, 160, 0), chest);
        for (int side = 0; side < 2; ++side)
        {
            const double sx = side == 0 ? 1.0 : -1.0;
            auto n = [&](const char* u, const char* b) {
                return std::string(ue ? std::string(u) + (side == 0 ? "_l" : "_r") : std::string(side == 0 ? "Left" : "Right") + b);
            };
            joint(n("upperarm", "Arm").c_str(), P(18 * sx, 145, 0), chest);
            joint(n("lowerarm", "ForeArm").c_str(), P(46 * sx, 145, -2), n("upperarm", "Arm").c_str());
            joint(n("hand", "Hand").c_str(), P(72 * sx, 145, 0), n("lowerarm", "ForeArm").c_str());
            joint(n("thigh", "UpLeg").c_str(), P(10 * sx, 95, 0), hips);
            joint(n("calf", "Leg").c_str(), P(10 * sx, 52, 2), n("thigh", "UpLeg").c_str());
            joint(n("foot", "Foot").c_str(), P(10 * sx, 8, 0), n("calf", "Leg").c_str());
            joint(n("ball", "ToeBase").c_str(), P(10 * sx, 2, 12), n("foot", "Foot").c_str());
        }
    }
}

void keyHips()
{
    Graph& g = graph();
    const NodeId hips = node("Hips").id();
    const glm::dvec3 t0 = node("Hips").as<glm::dvec3>(Transform::kTranslate);
    for (int k = 0; k < 3; ++k)
        anim::setKey(g, {hips, Transform::kTranslate, k}, 0.0, t0[k]);
    anim::setKey(g, {hips, Transform::kTranslate, 2}, 1.0, t0.z + 100.0);
    anim::setKey(g, {hips, Transform::kTranslate, 0}, 1.0, t0.x);
    anim::setKey(g, {hips, Transform::kTranslate, 1}, 1.0, t0.y);
}

fs::path tempFile(const char* name, const std::string& text)
{
    const fs::path p = fs::temp_directory_path() / "lila_retarget_tests" / name;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

const char* kBvh = R"(HIERARCHY
ROOT Hips
{
  OFFSET 0 100 0
  CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
  JOINT Spine
  {
    OFFSET 0 15 0
    CHANNELS 3 Zrotation Xrotation Yrotation
    End Site
    {
      OFFSET 0 40 0
    }
  }
  JOINT LeftUpLeg
  {
    OFFSET 10 -5 0
    CHANNELS 3 Zrotation Xrotation Yrotation
    JOINT LeftLeg
    {
      OFFSET 0 -43 2
      CHANNELS 3 Zrotation Xrotation Yrotation
      JOINT LeftFoot
      {
        OFFSET 0 -44 -2
        CHANNELS 3 Zrotation Xrotation Yrotation
        End Site
        {
          OFFSET 0 -6 12
        }
      }
    }
  }
  JOINT RightUpLeg
  {
    OFFSET -10 -5 0
    CHANNELS 3 Zrotation Xrotation Yrotation
    JOINT RightLeg
    {
      OFFSET 0 -43 2
      CHANNELS 3 Zrotation Xrotation Yrotation
      JOINT RightFoot
      {
        OFFSET 0 -44 -2
        CHANNELS 3 Zrotation Xrotation Yrotation
        End Site
        {
          OFFSET 0 -6 12
        }
      }
    }
  }
}
MOTION
Frames: 3
Frame Time: 0.0333333
0 100 0 0 0 0  0 0 0  0 0 0  0 0 0  0 0 0  0 0 0  0 0 0  0 0 0
0 100 10 0 0 0  0 10 0  0 0 0  0 0 0  0 0 0  0 0 0  0 0 0  0 0 0
0 100 20 0 0 0  0 20 0  0 0 0  0 0 0  0 0 0  0 0 0  0 0 0  0 0 0
)";
} // namespace

TEST_CASE("characterize and the rig commands")
{
    twoBipeds();
    const json r = run("characterize", {{"args", {"pelvis"}}});
    CHECK(r["pelvis"] == "pelvis");
    CHECK(r["rig"] == "pelvis_retargetRig");
    bool arm = false;
    for (const json& c : r["chains"])
        arm = arm || (c["name"] == "LeftArm" && c["goal"] == "hand_l");
    CHECK(arm);

    run("editRetargetRig", {{"args", {"hand_l"}}, {"chain", {{"name", "Spare"}, {"role", "other"}, {"start", "head"}}}});
    json q = run("editRetargetRig", {{"args", {"pelvis_retargetRig"}}, {"query", true}});
    bool spare = false;
    for (const json& c : q["chains"])
        spare = spare || c["name"] == "Spare";
    CHECK(spare);
    CHECK(q["poseRotation"].contains("hand_l"));
    run("editRetargetRig", {{"args", {"pelvis"}}, {"removeChain", "Spare"}});
    q = run("editRetargetRig", {{"args", {"pelvis"}}, {"query", true}});
    for (const json& c : q["chains"])
        CHECK(c["name"] != "Spare");
    CHECK_THROWS_AS(run("editRetargetRig", {{"args", {"pelvis"}}, {"chain", {{"name", "X"}, {"role", "elbow"}, {"start", "head"}}}}), CommandError);
}

TEST_CASE("retargeter settings edit by merging, and queries come back as set")
{
    twoBipeds();
    const json made = run("retargeter", {{"source", "Hips"}, {"target", "pelvis"}});
    const std::string r = made["retargeter"];
    CHECK(r == "Hips_to_pelvis");
    run("editRetargeter", {{"args", {r}}, {"chain", {{"target", "LeftLeg"}, {"ik", {{"extension", 0.95}}}}}, {"root", {{"scaleVertical", 0.9}}}});
    const json s = run("editRetargeter", {{"args", {r}}, {"query", true}});
    CHECK(s["root"]["scaleVertical"] == 0.9);
    CHECK(s["root"]["scaleHorizontal"] == 1.0);
    for (const json& c : s["chains"])
        if (c["target"] == "LeftLeg")
        {
            CHECK(c["ik"]["extension"] == 0.95);
            CHECK(c["source"] == "LeftLeg");
        }
    CHECK_THROWS_AS(run("editRetargeter", {{"args", {r}}, {"chain", {{"target", "Nope"}}}}), CommandError);
    run("alignRetargetPose", {{"args", {r}}});
}

TEST_CASE("retarget keys the target into a take, scaled, and undoes cleanly")
{
    twoBipeds();
    keyHips();
    const glm::dvec3 before = posAt("pelvis", 1.0);
    const json rep = run("retarget", {{"source", "Hips"}, {"target", "pelvis"}});
    CHECK(rep["take"] == "Hips_retarget");
    CHECK(rep["mode"] == "fk");
    CHECK(rep["frames"] == 31);
    CHECK(rep["scale"].get<double>() == doctest::Approx(0.8));
    const glm::dvec3 start = posAt("pelvis", 0.0), end = posAt("pelvis", 1.0);
    CHECK((end - start).z == doctest::Approx(80.0).epsilon(1e-6));
    CHECK(end.x == doctest::Approx(200.0));
    // Feet stay down while the hips glide: nothing planted moves them.
    CHECK(posAt("foot_l", 0.5).y == doctest::Approx(8.0 * 0.8).epsilon(1e-3));
    bool take = false;
    for (NodeId t : anim::activeTakes(graph()))
        take = take || graph().node(t)->name() == "Hips_retarget";
    CHECK(take);

    run("undo");
    CHECK(posAt("pelvis", 1.0).z == doctest::Approx(before.z));
    CHECK(graph().find("Hips_to_pelvis") == nullptr);
}

TEST_CASE("BVH files parse, import as keyed joints, and retarget from disk")
{
    const lila::retarget::bvh::File f = lila::retarget::bvh::parse(kBvh);
    CHECK(f.joints.size() == 11);
    CHECK(f.frames == 3);
    CHECK(f.channelCount() == 6 + 3 * 7);
    CHECK_THROWS_AS(lila::retarget::bvh::parse("HIERARCHY\nROOT a\n{ OFFSET 0 0 }"), std::invalid_argument);

    run("newScene");
    const fs::path file = tempFile("step.bvh", kBvh);
    const json im = run("importBvh", {{"args", {file.string()}}});
    CHECK(im["frames"] == 3);
    CHECK(im["root"] == "Hips");
    CHECK(node("Hips").as<int64_t>(Transform::kRotateOrder) == int64_t(RotateOrder::YXZ));
    CHECK(posAt("Hips", 2.0 / 30.0 * 1.00001).z == doctest::Approx(20.0).epsilon(1e-3));
    CHECK_FALSE(node("LeftFoot_end").as<bool>(Joint::kDeform));

    twoBipeds();
    const json rep = run("retarget", {{"target", "pelvis"}, {"files", {file.string()}}});
    REQUIRE(rep["files"].size() == 1);
    const json one = rep["files"][0];
    INFO(one.dump());
    CHECK(!one.contains("error"));
    CHECK(one["take"] == "step");
    CHECK(one["mode"] == "fk");
    // The imported skeleton is gone again; the BVH-named source in the scene was there before and stays.
    std::string hips;
    for (NodeId id : graph().nodes())
        if (graph().node(id)->name().starts_with("Hips"))
            hips += graph().node(id)->name() + " ";
    CHECK(hips == "Hips ");
}

TEST_CASE("a semantic retarget drives a body with other legs")
{
    twoBipeds();
    keyHips();
    // A four-legged target.
    auto j = [](const char* n, double x, double y, double z, const char* p) { joint(n, {x + 400, y, z}, p); };
    j("dog_pelvis", 0, 60, -30, "");
    j("dog_spine", 0, 62, 0, "dog_pelvis");
    j("dog_chest", 0, 62, 28, "dog_spine");
    j("dog_head", 0, 78, 44, "dog_chest");
    for (int s = 0; s < 2; ++s)
    {
        const double x = s == 0 ? 8.0 : -8.0;
        const std::string L = s == 0 ? "_l" : "_r";
        j(("front_upper" + L).c_str(), x, 50, 28, "dog_chest");
        j(("front_lower" + L).c_str(), x, 26, 30, ("front_upper" + L).c_str());
        j(("front_paw" + L).c_str(), x, 3, 30, ("front_lower" + L).c_str());
        j(("front_toe" + L).c_str(), x, 1, 35, ("front_paw" + L).c_str());
        j(("hind_upper" + L).c_str(), x, 55, -30, "dog_pelvis");
        j(("hind_lower" + L).c_str(), x, 30, -24, ("hind_upper" + L).c_str());
        j(("hind_paw" + L).c_str(), x, 3, -30, ("hind_lower" + L).c_str());
        j(("hind_toe" + L).c_str(), x, 1, -25, ("hind_paw" + L).c_str());
    }
    const json rep = run("retarget", {{"source", "Hips"}, {"target", "dog_pelvis"}, {"gait", "trot"}});
    INFO(rep.dump());
    CHECK(rep["mode"] == "semantic");
    CHECK(posAt("dog_pelvis", 1.0).z > posAt("dog_pelvis", 0.0).z + 20.0);
    CHECK_THROWS_AS(run("retarget", {{"source", "Hips"}, {"target", "dog_pelvis"}, {"gait", "hop"}}), CommandError);
}

TEST_CASE("a body-independent clip saves and plays on another body")
{
    twoBipeds();
    keyHips();
    const fs::path file = fs::temp_directory_path() / "lila_retarget_tests" / "glide.lilaclip";
    fs::create_directories(file.parent_path());
    const json saved = run("saveRetargetClip", {{"args", {file.string()}}, {"source", "Hips"}});
    CHECK(saved["frames"] == 31);
    const json rep = run("applyRetargetClip", {{"args", {file.string()}}, {"target", "pelvis"}});
    INFO(rep.dump());
    CHECK(rep["take"] == "glide");
    CHECK(rep["mode"] == "semantic");
    // A hundred cm on 87 cm legs is about 80 cm on the target's 70 cm legs.
    CHECK((posAt("pelvis", 1.0) - posAt("pelvis", 0.0)).z == doctest::Approx(80.0).epsilon(0.02));
    CHECK_THROWS_AS(run("applyRetargetClip", {{"args", {"nope.lilaclip"}}, {"target", "pelvis"}}), CommandError);
}

TEST_CASE("an ACCAD clip retargets onto UE5 Manny" * doctest::skip(!std::getenv("LILA_TEST_BVH")))
{
    const fs::path manny = fs::path(LILA_TEST_DATA_DIR) / "rig" / "SKM_Manny_skeleton.fbx";
    if (!fs::exists(manny))
        return;
    run("newScene");
    run("importFbx", {{"args", {manny.string()}}});
    const json rep = run("retarget", {{"target", "pelvis"}, {"files", {std::getenv("LILA_TEST_BVH")}}});
    INFO(rep.dump(2));
    const json one = rep["files"][0];
    CHECK(!one.contains("error"));
    CHECK(one["ikError"].get<double>() < 1.0);
    MESSAGE(rep.dump(2));
}
