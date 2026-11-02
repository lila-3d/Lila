#include "TestUtil.h"

using namespace rtest;

namespace
{
Rig characterized(const std::vector<Spec>& specs, Skeleton& s, Pose& p)
{
    s = build(specs, p);
    return characterize(s, p);
}

const Chain& need(const Rig& r, const char* name)
{
    const Chain* c = r.chain(name);
    INFO("missing chain " << std::string(name));
    REQUIRE(c);
    return *c;
}
} // namespace

TEST_CASE("characterize finds a UE-named biped's pelvis, limbs, sides and goals")
{
    Skeleton s;
    Pose p;
    const Rig r = characterized(biped(1.0, "ue", 3, false, true), s, p);
    CHECK(r.pelvis == "pelvis");
    CHECK(need(r, "Root").start == "root");
    CHECK(need(r, "Spine").start == "spine_01");
    CHECK(need(r, "Spine").end == "spine_03");
    CHECK(need(r, "Neck").start == "neck_01");
    CHECK(need(r, "Head").start == "head");
    CHECK(need(r, "LeftClavicle").start == "clavicle_l");
    const Chain& arm = need(r, "LeftArm");
    CHECK(arm.start == "upperarm_l");
    CHECK(arm.end == "hand_l");
    CHECK(arm.goal == "hand_l");
    const Chain& leg = need(r, "RightLeg");
    CHECK(leg.start == "thigh_r");
    CHECK(leg.goal == "foot_r");
    CHECK(need(r, "RightToe").start == "ball_r");
    CHECK(need(r, "LeftThumb").start == "thumb_01_l");
    CHECK(need(r, "LeftIndex").end == "index_02_l");
    CHECK(need(r, "RightPinky").start == "pinky_01_r");
    CHECK(glm::length(r.forward - glm::dvec3(0, 0, 1)) < 1e-6);
    CHECK(r.notes.empty());
}

TEST_CASE("characterize reads BVH names and finds the same chains")
{
    Skeleton s;
    Pose p;
    const Rig r = characterized(biped(0.9, "bvh", 2), s, p);
    CHECK(r.pelvis == "Hips");
    CHECK(need(r, "LeftArm").start == "LeftArm");
    CHECK(need(r, "LeftArm").goal == "LeftHand");
    CHECK(need(r, "LeftClavicle").start == "LeftShoulder");
    CHECK(need(r, "RightLeg").start == "RightUpLeg");
    CHECK(need(r, "RightLeg").goal == "RightFoot");
}

TEST_CASE("characterize works from shape alone on a skeleton with meaningless names")
{
    Skeleton s;
    Pose p;
    const Rig r = characterized(biped(1.0, "none", 4, true), s, p);
    const Chain& leg = need(r, "LeftLeg");
    // The left thigh sits at +X with the body facing +Z.
    const std::vector<glm::dmat4> w = worldOf(s, p);
    CHECK(positionOf(w[size_t(s.find(leg.start))]).x > 0.0);
    CHECK(positionOf(w[size_t(s.find(leg.goal))]).y == doctest::Approx(8.0));
    const Chain& arm = need(r, "RightArm");
    CHECK(positionOf(w[size_t(s.find(arm.goal))]).x == doctest::Approx(-72.0));
    CHECK(chainJoints(s, need(r, "Spine")).size() == 4);
    CHECK(r.chain("LeftClavicle"));
}

TEST_CASE("characterize finds four legs on a quadruped, front ones first, and its tail")
{
    Skeleton s;
    Pose p;
    const Rig r = characterized(quadruped(), s, p);
    CHECK(r.pelvis == "pelvis");
    CHECK(need(r, "LeftLeg").start == "scapula_l");
    CHECK(need(r, "LeftLeg").goal == "paw_l");
    CHECK(need(r, "LeftLeg2").start == "thigh_l");
    CHECK(need(r, "LeftLeg2").goal == "foot_l");
    CHECK(need(r, "RightLeg2").goal == "foot_r");
    CHECK(need(r, "Tail").start == "tail1");
    CHECK(need(r, "Neck").start == "neck");
    CHECK(need(r, "Head").start == "head");
    CHECK(!r.chain("LeftArm"));
}

TEST_CASE("characterize finds six legs on an insect by shape and numbers them front to back")
{
    Skeleton s;
    Pose p;
    const Rig r = characterized(hexapod(), s, p);
    int legs = 0;
    for (const Chain& c : r.chains)
        legs += c.role == Role::Leg ? 1 : 0;
    CHECK(legs == 6);
    CHECK(need(r, "LeftLeg").start == "leg0a"); // the coxa is named as part of the leg
    CHECK(need(r, "LeftLeg3").goal == "leg2d");
    CHECK(need(r, "RightLeg2").start == "leg4a");
    CHECK(need(r, "Head").start == "head");
}

TEST_CASE("a retarget rig survives JSON")
{
    Skeleton s;
    Pose p;
    const Rig r = characterized(biped(), s, p);
    const Rig back = Rig::fromJson(r.toJson());
    CHECK(back.toJson() == r.toJson());
    CHECK_THROWS_AS(Rig::fromJson(json{{"chains", json::array({{{"name", "x"}, {"role", "elbow"}, {"start", "a"}}})}}), std::invalid_argument);
}
