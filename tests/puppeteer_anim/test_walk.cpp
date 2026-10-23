#include <doctest/doctest.h>

#include "../../extensions/Puppeteer/anim/WalkCycle.h"

#include <cstring>

using namespace puppeteer::anim;
using Approx = doctest::Approx;

namespace
{
/** A Y-up biped facing +Z, its left side toward +X, every joint unrotated. */
std::vector<RigJoint> biped()
{
    std::vector<RigJoint> j;
    auto add = [&](const char* label, int parent, glm::dvec3 p) {
        j.push_back({label, parent, p, glm::dquat(1, 0, 0, 0)});
        return int(j.size()) - 1;
    };
    const int root = add("root", -1, {0, 0, 0});
    const int pelvis = add("pelvis", root, {0, 100, 0});
    const int s1 = add("spine_01", pelvis, {0, 110, 0});
    const int s2 = add("spine_02", s1, {0, 125, 0});
    const int neck = add("neck_01", s2, {0, 150, 0});
    add("head", neck, {0, 160, 0});
    for (int side = 0; side < 2; ++side)
    {
        const double x = side == 0 ? 10.0 : -10.0;
        const std::string s = side == 0 ? "_l" : "_r";
        const int thigh = add(("thigh" + s).c_str(), pelvis, {x, 95, 0});
        const int calf = add(("calf" + s).c_str(), thigh, {x, 52, 1});
        const int foot = add(("foot" + s).c_str(), calf, {x, 8, 0});
        add(("ball" + s).c_str(), foot, {x, 0, 12});
        const int ua = add(("upperarm" + s).c_str(), s2, {x * 1.8, 145, 0});
        const int la = add(("lowerarm" + s).c_str(), ua, {x * 1.8, 118, 0});
        add(("hand" + s).c_str(), la, {x * 1.8, 95, 0});
    }
    return j;
}

int find(const std::vector<RigJoint>& j, const char* label)
{
    for (size_t i = 0; i < j.size(); ++i)
        if (j[i].label == label)
            return int(i);
    FAIL("no joint labelled " << label);
    return -1;
}

bool same(const WalkPose& a, const WalkPose& b)
{
    return a.position.size() == b.position.size() &&
           std::memcmp(a.position.data(), b.position.data(), a.position.size() * sizeof(glm::dvec3)) == 0 &&
           std::memcmp(a.rotation.data(), b.rotation.data(), a.rotation.size() * sizeof(glm::dquat)) == 0;
}
} // namespace

TEST_CASE("walk solver: the character frame comes from the skeleton")
{
    const WalkSolver s(biped(), {});
    CHECK(s.up() == glm::dvec3(0, 1, 0));
    CHECK(s.left() == glm::dvec3(1, 0, 0));
    CHECK(s.forward() == glm::dvec3(0, 0, 1));
}

TEST_CASE("walk solver: deterministic, and the cycle closes on itself")
{
    WalkParams p;
    const WalkSolver a(biped(), p), b(biped(), p);
    for (double phase : {0.0, 0.13, 0.5, 0.77})
        CHECK(same(a.pose(phase), b.pose(phase)));
    // Phase 1 is phase 0 exactly; just before it the pose is a small step away, not a jump.
    CHECK(same(a.pose(1.0), a.pose(0.0)));
    const WalkPose end = a.pose(1.0 - 1e-4), start = a.pose(0.0);
    for (size_t i = 0; i < start.position.size(); ++i)
        CHECK(glm::length(end.position[i] - start.position[i]) < 0.05);
}

TEST_CASE("walk solver: the planted foot stays down and slides back; the swinging foot lifts")
{
    WalkParams p;
    p.stride = 50;
    p.stepHeight = 10;
    const std::vector<RigJoint> rest = biped();
    const WalkSolver s(rest, p);
    const int foot = find(rest, "foot_r"), thigh = find(rest, "thigh_r"), calf = find(rest, "calf_r");
    const double st = p.stance();
    double prevZ = 1e9, maxLift = 0;
    for (int k = 0; k <= 20; ++k)
    {
        const double q = st * (0.25 + 0.4 * k / 20.0); // mid stance, clear of the heel strike and the heel lift
        const WalkPose pose = s.pose(q);
        CHECK(pose.position[size_t(foot)].y == Approx(8.0).epsilon(0.02));
        CHECK(pose.position[size_t(foot)].z < prevZ);
        prevZ = pose.position[size_t(foot)].z;
        // The IK keeps the bones their length.
        CHECK(glm::length(pose.position[size_t(calf)] - pose.position[size_t(thigh)]) == Approx(glm::length(rest[size_t(calf)].position - rest[size_t(thigh)].position)));
    }
    for (int k = 0; k <= 40; ++k)
        maxLift = std::max(maxLift, s.pose(st + (1.0 - st) * k / 40.0).position[size_t(foot)].y - 8.0);
    CHECK(maxLift > 7.0);
    CHECK(maxLift < 30.0);

    // Arms swing against the legs: at phase 0 the right foot is forward and the left hand ahead of the right.
    const WalkPose zero = s.pose(0.0);
    CHECK(zero.position[size_t(foot)].z > 0.0);
    CHECK(zero.position[size_t(find(rest, "hand_l"))].z > zero.position[size_t(find(rest, "hand_r"))].z);
}

TEST_CASE("walk solver: needs the leg labels; key frames are whole and end on the loop")
{
    std::vector<RigJoint> j = biped();
    j[size_t(find(j, "calf_l"))].label.clear();
    CHECK_THROWS_WITH_AS(WalkSolver(j, {}), doctest::Contains("calf_l"), std::invalid_argument);

    WalkParams p;
    p.cycleFrames = 32;
    p.keysPerStep = 4;
    CHECK(walkKeyFrames(p) == std::vector<int>{0, 4, 8, 12, 16, 20, 24, 28, 32});
    p.speed = 100; // 50 cm under the body in 0.6 of the cycle at 100 cm/s: 0.833 s, 25 frames
    CHECK(p.frames() == 25);
    CHECK(walkKeyFrames(p).back() == 25);
}
