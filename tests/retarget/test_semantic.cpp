#include "TestUtil.h"

#include "retarget/Semantic.h"

using namespace rtest;
using namespace lila::retarget::semantic;

namespace
{
struct Source
{
    Skeleton s;
    Pose p;
    Rig rig;
    Frames clip;
};

/** A biped walking forward at 120 cm/s with 60 cm strides for three seconds. */
Source bipedWalk()
{
    Source src;
    src.s = build(biped(1.0, "bvh", 2), src.p);
    src.rig = characterize(src.s, src.p);
    src.clip = walk(src.s, src.rig, src.p, 90, 30.0, 120.0, 60.0, {{"LeftLeg", 0.0}, {"RightLeg", 0.5}});
    return src;
}

const std::vector<bool>& plantedOf(const Result& r, const char* chain)
{
    for (const auto& [name, planted] : r.planted)
        if (name == chain)
            return planted;
    FAIL("no planted track for " << chain);
    static std::vector<bool> none;
    return none;
}

double agreement(const std::vector<bool>& a, const std::vector<bool>& b)
{
    int same = 0;
    for (size_t f = 0; f < a.size(); ++f)
        same += a[f] == b[f] ? 1 : 0;
    return double(same) / double(a.size());
}
} // namespace

TEST_CASE("gait phases: walk, trot and pace on four legs, tripods on six")
{
    const std::vector<std::pair<Side, int>> four = {{Side::Left, 0}, {Side::Left, 1}, {Side::Right, 0}, {Side::Right, 1}};
    CHECK(gaitPhases(Gait::Walk, four) == std::vector<double>{0.25, 0.0, 0.75, 0.5});
    CHECK(gaitPhases(Gait::Trot, four) == std::vector<double>{0.0, 0.5, 0.5, 0.0});
    CHECK(gaitPhases(Gait::Pace, four) == std::vector<double>{0.0, 0.0, 0.5, 0.5});
    std::vector<std::pair<Side, int>> six;
    for (Side s : {Side::Left, Side::Right})
        for (int i = 0; i < 3; ++i)
            six.push_back({s, i});
    CHECK(gaitPhases(Gait::Auto, six) == std::vector<double>{0.0, 0.5, 0.0, 0.5, 0.0, 0.5});
    CHECK(gaitFromName("gallop") == Gait::Gallop);
    CHECK_THROWS_AS(gaitFromName("moonwalk"), std::invalid_argument);
}

TEST_CASE("a walk encodes into contacts half a cycle apart and a steady speed in leg lengths")
{
    const Source src = bipedWalk();
    const Clip c = encode(src.s, src.rig, src.p, src.clip);
    REQUIRE(c.limbs.size() == 4); // two legs, two arms
    const Cycle cyc = measureCycle(c);
    CHECK(cyc.period == doctest::Approx(60.0 / (120.0 * 0.6)).epsilon(0.05));
    double left = -1, right = -1;
    for (size_t l = 0; l < c.limbs.size(); ++l)
        if (c.limbs[l].role == Role::Leg)
            (c.limbs[l].side == Side::Left ? left : right) = cyc.phase[l];
    double gap = std::abs(left - right);
    gap = std::min(gap, 1.0 - gap);
    CHECK(gap == doctest::Approx(0.5).epsilon(0.08));
    // 120 cm/s over legs of 87 cm.
    CHECK(c.velocity[45].x == doctest::Approx(120.0 / 87.0).epsilon(0.02));
    CHECK(std::abs(c.velocity[45].y) < 1e-6);

    const Clip back = Clip::fromJson(c.toJson());
    CHECK(back.toJson() == c.toJson());
}

TEST_CASE("a biped walk on a quadruped trots with diagonal pairs and holds planted feet")
{
    const Source src = bipedWalk();
    const Clip c = encode(src.s, src.rig, src.p, src.clip);
    Pose dp;
    const Skeleton dog = build(quadruped(), dp);
    const Rig dogRig = characterize(dog, dp);
    const Result r = apply(c, dog, dogRig, dp, {.gait = Gait::Trot});
    REQUIRE(r.poses.size() == 90);
    CHECK(r.ikError < 0.5);
    const auto& lf = plantedOf(r, "LeftLeg");
    const auto& rf = plantedOf(r, "RightLeg");
    const auto& rh = plantedOf(r, "RightLeg2");
    CHECK(agreement(lf, rh) > 0.9);
    CHECK(agreement(lf, rf) < 0.5);

    // Feet stay where they touched down.
    const int paw = dog.find("paw_l");
    glm::dvec3 anchor(0.0);
    for (size_t f = 0; f < lf.size(); ++f)
    {
        const glm::dvec3 at = positionOf(worldOf(dog, r.poses[f])[size_t(paw)]);
        if (lf[f] && (f == 0 || !lf[f - 1]))
            anchor = at;
        if (lf[f])
            CHECK(glm::length(at - anchor) < 0.5);
    }
    // The dog covers ground in its own leg lengths: the same number of leg lengths the biped did.
    const double dogLeg = chainLength(dog, chainJoints(dog, *dogRig.chain("LeftLeg2")), worldOf(dog, dp));
    const double moved = pos(worldOf(dog, r.poses.back()), dog, "pelvis").z - pos(worldOf(dog, r.poses.front()), dog, "pelvis").z;
    double legs = 0.0;
    for (const Chain& ch : dogRig.chains)
        legs += ch.role == Role::Leg ? chainLength(dog, chainJoints(dog, ch), worldOf(dog, dp)) / 4.0 : 0.0;
    CHECK(moved == doctest::Approx(120.0 / 87.0 * legs * 89.0 / 30.0).epsilon(0.03));
    CHECK(dogLeg > 0.0);
}

TEST_CASE("a biped walk on an insect moves its legs in two tripods")
{
    const Source src = bipedWalk();
    const Clip c = encode(src.s, src.rig, src.p, src.clip);
    Pose ip;
    const Skeleton bug = build(hexapod(), ip);
    const Rig bugRig = characterize(bug, ip);
    const Result r = apply(c, bug, bugRig, ip);
    CHECK(r.ikError < 0.5);
    const auto& l1 = plantedOf(r, "LeftLeg");
    CHECK(agreement(l1, plantedOf(r, "LeftLeg3")) > 0.9);
    CHECK(agreement(l1, plantedOf(r, "RightLeg2")) > 0.9);
    CHECK(agreement(l1, plantedOf(r, "LeftLeg2")) < 0.5);
    for (const Pose& p : r.poses)
        for (const glm::dquat& q : p.rotation)
            CHECK(std::isfinite(q.w));
}
