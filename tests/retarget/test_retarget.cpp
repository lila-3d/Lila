#include "TestUtil.h"

#include <glm/gtx/quaternion.hpp>

using namespace rtest;

namespace
{
struct Pair
{
    Skeleton s, t;
    Pose sp, tp;
    Rig sr, tr;
    Settings set;
};

Pair pair(const std::vector<Spec>& source, const std::vector<Spec>& target)
{
    Pair p;
    p.s = build(source, p.sp);
    p.t = build(target, p.tp);
    p.sr = characterize(p.s, p.sp);
    p.tr = characterize(p.t, p.tp);
    p.set = autoMap(p.sr, p.tr);
    return p;
}

/** Turns a joint by a world rotation, its children following. */
void turn(const Skeleton& s, Pose& p, const char* name, const glm::dquat& q)
{
    const int i = s.find(name);
    REQUIRE(i >= 0);
    const std::vector<glm::dmat4> w = worldOf(s, p);
    p.rotation[size_t(i)] = localRotationFor(s, i, parentWorldOf(s, w, i), q * rotationOf(w[size_t(i)]));
}

void move(const Skeleton& s, Pose& p, const char* name, const glm::dvec3& by)
{
    const int i = s.find(name);
    const std::vector<glm::dmat4> w = worldOf(s, p);
    p.translate[size_t(i)] = translateFor(parentWorldOf(s, w, i), positionOf(w[size_t(i)]) + by);
}

glm::dvec3 dir(const std::vector<glm::dmat4>& w, const Skeleton& s, const char* a, const char* b) { return glm::normalize(pos(w, s, b) - pos(w, s, a)); }

void near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6)
{
    INFO(a.x << " " << a.y << " " << a.z << " vs " << b.x << " " << b.y << " " << b.z);
    CHECK(glm::length(a - b) < eps);
}

double angleOf(const glm::dquat& q) { return glm::degrees(glm::angle(glm::normalize(q))); }
} // namespace

TEST_CASE("a source in its retarget pose leaves the target in its own")
{
    Pair p = pair(biped(1.0, "bvh", 2), biped(0.8, "ue", 3, true));
    const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
    CHECK(r.scale() == doctest::Approx(0.8));
    const Pose out = r.frame(worldOf(p.s, p.sp));
    const std::vector<glm::dmat4> a = worldOf(p.t, out), b = worldOf(p.t, p.tp);
    for (size_t i = 0; i < p.t.size(); ++i)
        near(positionOf(a[i]), positionOf(b[i]), 1e-5);
}

TEST_CASE("FK turns the target's arm the way the source's turns, whatever the joint orients")
{
    Pair p = pair(biped(1.0, "bvh", 2), biped(0.8, "ue", 3, true));
    p.set.chain("LeftArm")->ik.enabled = false;
    const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
    Pose src = p.sp;
    turn(p.s, src, "LeftArm", glm::angleAxis(glm::radians(-60.0), glm::dvec3(0, 0, 1)));
    turn(p.s, src, "LeftForeArm", glm::angleAxis(glm::radians(70.0), glm::dvec3(0, 1, 0)));
    const std::vector<glm::dmat4> sw = worldOf(p.s, src);
    const std::vector<glm::dmat4> tw = worldOf(p.t, r.frame(sw));
    near(dir(tw, p.t, "upperarm_l", "lowerarm_l"), dir(sw, p.s, "LeftArm", "LeftForeArm"), 1e-2);
    near(dir(tw, p.t, "lowerarm_l", "hand_l"), dir(sw, p.s, "LeftForeArm", "LeftHand"), 1e-2);
}

TEST_CASE("the pelvis moves by the source's motion scaled to the target's legs")
{
    Pair p = pair(biped(1.0, "bvh", 2), biped(0.8, "ue", 3));
    const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
    Pose src = p.sp;
    move(p.s, src, "Hips", {0, -5, 10});
    const std::vector<glm::dmat4> tw = worldOf(p.t, r.frame(worldOf(p.s, src)));
    near(pos(tw, p.t, "pelvis"), glm::dvec3(0, 80 - 4, 8), 1e-6);
}

TEST_CASE("IK puts the target's foot where the source's goes, scaled, keeping the target's stance")
{
    Pair p = pair(biped(1.0, "bvh", 2), biped(0.8, "ue", 3, true));
    const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
    Pose src = p.sp;
    std::vector<glm::dmat4> sw = worldOf(p.s, src);
    const std::vector<int> leg = p.s.path(p.s.find("LeftUpLeg"), p.s.find("LeftFoot"));
    const glm::dvec3 lift(0, 20, 15);
    solveChainIk(p.s, src, sw, leg, pos(sw, p.s, "LeftFoot") + lift, rotationOf(sw[size_t(leg.back())]));
    const std::vector<glm::dmat4> tw = worldOf(p.t, r.frame(sw));
    const std::vector<glm::dmat4> t0 = worldOf(p.t, p.tp);
    near(pos(tw, p.t, "foot_l"), pos(t0, p.t, "foot_l") + lift * 0.8, 1e-3);
    near(pos(tw, p.t, "foot_r"), pos(t0, p.t, "foot_r"), 1e-3);
}

TEST_CASE("chains with different joint counts turn by the same amount at the same fraction")
{
    Pair p = pair(biped(1.0, "bvh", 2), biped(1.0, "ue", 4));
    const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
    Pose src = p.sp;
    for (const char* j : {"Spine", "Spine1"})
        turn(p.s, src, j, glm::angleAxis(glm::radians(10.0), glm::dvec3(1, 0, 0)));
    const Pose out = r.frame(worldOf(p.s, src));
    const std::vector<glm::dmat4> tw = worldOf(p.t, out), t0 = worldOf(p.t, p.tp);
    auto turned = [&](const char* name) {
        const int i = p.t.find(name);
        return angleOf(rotationOf(tw[size_t(i)]) * glm::inverse(rotationOf(t0[size_t(i)])));
    };
    CHECK(turned("spine_01") == doctest::Approx(10.0).epsilon(1e-6));
    CHECK(turned("spine_04") == doctest::Approx(20.0).epsilon(1e-6));
    CHECK(turned("spine_02") > 10.0);
    CHECK(turned("spine_02") < 20.0);
}

TEST_CASE("pole matching swings the target's elbow to the side the source's bends")
{
    std::vector<Spec> target = biped(1.0, "ue", 3);
    for (Spec& sp : target)
        if (sp.name == "lowerarm_l")
            sp.pos.z = 6.0; // the target's elbow points forward, the source's back
    Pair p = pair(biped(1.0, "bvh", 2), target);
    ChainMap& arm = *p.set.chain("LeftArm");
    arm.ik.enabled = false;
    arm.fk.rotation = FkRotation::None;
    {
        const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
        CHECK(pos(worldOf(p.t, r.frame(worldOf(p.s, p.sp))), p.t, "lowerarm_l").z > 0.0);
    }
    arm.fk.poleMatching = 1.0;
    const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
    const std::vector<glm::dmat4> tw = worldOf(p.t, r.frame(worldOf(p.s, p.sp)));
    CHECK(pos(tw, p.t, "lowerarm_l").z < 0.0);
    // The hand stays on the shoulder-to-hand line it had.
    near(pos(tw, p.t, "hand_l"), glm::dvec3(72, 145, 0), 1e-6);
}

TEST_CASE("auto-align turns an A-posed target's arms onto a T-posed source's")
{
    std::vector<Spec> target = biped(1.0, "ue", 3);
    for (Spec& sp : target)
    {
        const double s = sp.name.ends_with("_l") ? 1.0 : -1.0;
        if (sp.name.starts_with("lowerarm"))
            sp.pos = {s * (18 + 20), 145 - 20, 0};
        if (sp.name.starts_with("hand"))
            sp.pos = {s * (18 + 38), 145 - 38, 0};
    }
    Pair p = pair(biped(1.0, "bvh", 2), target);
    const Pose aligned = alignRetargetPose(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
    const std::vector<glm::dmat4> tw = worldOf(p.t, aligned), sw = worldOf(p.s, p.sp);
    // Bones align by length fraction: the elbows sit at slightly different fractions, so allow for the bend between.
    near(dir(tw, p.t, "upperarm_r", "lowerarm_r"), dir(sw, p.s, "RightArm", "RightForeArm"), 1e-2);
    near(dir(tw, p.t, "lowerarm_r", "hand_r"), dir(sw, p.s, "RightForeArm", "RightHand"), 1e-2);
    near(dir(tw, p.t, "upperarm_r", "hand_r"), dir(sw, p.s, "RightArm", "RightHand"), 1e-2);
}

TEST_CASE("planted feet hold still on the target through each stance")
{
    Pair p = pair(biped(1.0, "bvh", 2), biped(0.8, "ue", 3, true));
    const Frames clip = walk(p.s, p.sr, p.sp, 90, 30.0, 120.0, 60.0, {{"LeftLeg", 0.0}, {"RightLeg", 0.5}});
    const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
    const Result res = r.clip(clip);
    REQUIRE(res.poses.size() == 90);
    CHECK(res.ikError < 0.01);
    int plantedFrames = 0;
    for (const auto& [chain, planted] : res.planted)
    {
        if (chain != "LeftLeg")
            continue;
        const int foot = p.t.find("foot_l");
        glm::dvec3 anchor(0.0);
        for (size_t f = 0; f < planted.size(); ++f)
        {
            const glm::dvec3 at = positionOf(worldOf(p.t, res.poses[f])[size_t(foot)]);
            if (planted[f] && (f == 0 || !planted[f - 1]))
                anchor = at;
            if (planted[f])
            {
                ++plantedFrames;
                near(at, anchor, 1e-3);
            }
        }
    }
    CHECK(plantedFrames > 30);
    // The target covers 0.8 of the source's ground.
    const glm::dvec3 start = pos(worldOf(p.t, res.poses.front()), p.t, "pelvis"), end = pos(worldOf(p.t, res.poses.back()), p.t, "pelvis");
    CHECK((end - start).z == doctest::Approx(120.0 * 89.0 / 30.0 * 0.8).epsilon(1e-3));
}

TEST_CASE("stride warping lengthens steps around the pelvis")
{
    Pair p = pair(biped(1.0, "bvh", 2), biped(1.0, "ue", 3));
    const Frames clip = walk(p.s, p.sr, p.sp, 60, 30.0, 120.0, 60.0, {{"LeftLeg", 0.0}, {"RightLeg", 0.5}});
    auto reach = [&](const Settings& set) {
        const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, set);
        const Result res = r.clip(clip);
        double most = 0.0;
        for (const Pose& pose : res.poses)
        {
            const std::vector<glm::dmat4> w = worldOf(p.t, pose);
            most = std::max(most, std::abs(pos(w, p.t, "foot_l").z - pos(w, p.t, "pelvis").z));
        }
        return most;
    };
    Settings warped = p.set;
    warped.warp.enabled = true;
    warped.warp.warpForwards = 1.5;
    for (ChainMap& m : warped.chains)
        m.plant.enabled = false;
    Settings plain = warped;
    plain.warp.enabled = false;
    CHECK(reach(warped) > reach(plain) * 1.3);
}

TEST_CASE("a pinned bone follows the bone it is pinned to")
{
    std::vector<Spec> target = biped(1.0, "ue", 3);
    target.push_back({"ik_foot_l", "root", {10, 8, 0}, {}});
    Pair p = pair(biped(1.0, "bvh", 2), target);
    p.set.pins.push_back({.bone = "ik_foot_l", .to = "foot_l"});
    const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
    Pose src = p.sp;
    std::vector<glm::dmat4> sw = worldOf(p.s, src);
    const std::vector<int> leg = p.s.path(p.s.find("LeftUpLeg"), p.s.find("LeftFoot"));
    solveChainIk(p.s, src, sw, leg, pos(sw, p.s, "LeftFoot") + glm::dvec3(5, 25, 20), rotationOf(sw[size_t(leg.back())]));
    const std::vector<glm::dmat4> tw = worldOf(p.t, r.frame(sw));
    near(pos(tw, p.t, "ik_foot_l"), pos(tw, p.t, "foot_l"), 1e-6);
}

TEST_CASE("a hand on the hip stays on the target's hip")
{
    Pair p = pair(biped(1.0, "bvh", 2), biped(0.8, "ue", 3));
    p.set.contacts.push_back({.effector = "LeftArm", .body = "Spine", .bodyParam = 0.0, .distance = 6.0});
    Pose src = p.sp;
    std::vector<glm::dmat4> sw = worldOf(p.s, src);
    const glm::dvec3 hip = pos(sw, p.s, "Spine") + glm::dvec3(14, -2, 0);
    const std::vector<int> arm = p.s.path(p.s.find("LeftArm"), p.s.find("LeftHand"));
    solveChainIk(p.s, src, sw, arm, hip, rotationOf(sw[size_t(arm.back())]));
    Frames clip;
    clip.world = {sw, sw, sw};
    const Retargeter r(p.s, p.sr, p.sp, p.t, p.tr, p.tp, p.set);
    const Result res = r.clip(clip);
    const std::vector<glm::dmat4> tw = worldOf(p.t, res.poses[1]);
    near(pos(tw, p.t, "hand_l") - pos(tw, p.t, "spine_01"), glm::dvec3(14, -2, 0) * 0.8, 0.05);
}

TEST_CASE("retarget settings survive JSON and reject unknown modes")
{
    Pair p = pair(biped(), biped());
    p.set.chain("LeftArm")->fk.rotation = FkRotation::OneToOne;
    p.set.chain("LeftLeg")->ik.extension = 0.97;
    p.set.warp.enabled = true;
    p.set.pins.push_back({.bone = "a", .to = "b"});
    p.set.curves.push_back({"x", "y"});
    const Settings back = Settings::fromJson(p.set.toJson());
    CHECK(back.toJson() == p.set.toJson());
    json bad = p.set.toJson();
    bad["chains"][0]["fk"]["rotation"] = "sideways";
    CHECK_THROWS_AS(Settings::fromJson(bad), std::invalid_argument);
}

TEST_CASE("auto-mapping pairs chains by name, then by role and side")
{
    Pair p = pair(biped(1.0, "bvh", 2), biped(1.0, "ue", 3, false, true));
    CHECK(p.set.chain("LeftArm")->source == "LeftArm");
    CHECK(p.set.chain("RightLeg")->source == "RightLeg");
    CHECK(p.set.chain("RightLeg")->ik.enabled);
    CHECK(p.set.chain("Root")->fk.translation == FkTranslation::GloballyScaled);
    // The source has no fingers: the target's stay in their retarget pose.
    CHECK(p.set.chain("LeftIndex")->source.empty());
}
