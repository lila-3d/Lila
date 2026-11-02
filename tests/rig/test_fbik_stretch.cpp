#include "TestUtil.h"

#include "scene/FullBodyIk.h"
#include "scene/Rig.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace rigtest;
using namespace lila::scene;

namespace
{
/** A straight chain of four 10 cm bones along X; the bones after the first may stretch to most. */
fbik::Problem stretchyChain(double most)
{
    fbik::Problem p;
    for (int i = 0; i < 4; ++i)
    {
        fbik::Bone b;
        b.parent = i - 1;
        b.before = glm::translate(glm::dmat4(1.0), glm::dvec3(i == 0 ? 0.0 : 10.0, 0.0, 0.0));
        b.maxStretch = i == 0 ? 1.0 : most;
        p.bones.push_back(b);
    }
    p.lengthScale = 10.0;
    return p;
}

glm::dvec3 endOf(const fbik::Problem& p, const fbik::Solution& s) { return glm::dvec3(fbik::forward(p, s.rotation, s.offset, s.stretch)[3][3]); }

/** upper -> lower -> hand along X, 10 cm bones, with an arm IK handle (ikBlend 0, so FK shows) whose stretch is on. */
void stretchyArm(bool handleStretch)
{
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "upper"}});
    run("joint", {{"position", v3(10, 0, 0)}, {"name", "lower"}});
    run("joint", {{"position", v3(20, 0, 0)}, {"name", "hand"}});
    const std::string h = run("ikHandle", {{"startJoint", "upper"}, {"endEffector", "hand"}, {"name", "armIk"}}).get<std::string>();
    run("setAttr", {{"args", {h + ".stretch", handleStretch}}});
    run("setAttr", {{"args", {h + ".maxStretch", 1.4}}});
    run("setAttr", {{"args", {h + ".ikBlend", 0.0}}});
}

json pinHand(const glm::dvec3& p) { return json::array({{{"joint", "hand"}, {"position", v3(p.x, p.y, p.z)}}}); }
} // namespace

TEST_CASE("fbik stretch: a pin within reach never stretches")
{
    fbik::Problem p = stretchyChain(1.5);
    p.effectors.push_back({.bone = 3, .position = glm::dvec3(15.0, 15.0, 5.0)});
    const fbik::Solution s = fbik::solve(p);
    CHECK(s.error < 1e-3);
    for (double f : s.stretch)
        CHECK(f == 1.0);
}

TEST_CASE("fbik stretch: an out-of-reach pin is met by lengthening the stretchy bones within their max")
{
    fbik::Problem p = stretchyChain(1.5);
    p.effectors.push_back({.bone = 3, .position = glm::dvec3(0.0, 40.0, 0.0)});
    const fbik::Solution s = fbik::solve(p);
    CHECK(s.error < 1e-3);
    checkNear(endOf(p, s), glm::dvec3(0.0, 40.0, 0.0), 1e-4);
    CHECK(s.stretch[0] == 1.0);
    double longest = 1.0;
    for (size_t i = 1; i < s.stretch.size(); ++i)
    {
        CHECK(s.stretch[i] >= 1.0);
        CHECK(s.stretch[i] <= 1.5);
        longest = std::max(longest, s.stretch[i]);
    }
    CHECK(longest > 1.1);

    // Past every bone's max the chain stops at its longest, 45 cm, pointing at the pin.
    p.effectors.front().position = glm::dvec3(0.0, 60.0, 0.0);
    const fbik::Solution far = fbik::solve(p);
    CHECK(far.error == doctest::Approx(15.0).epsilon(1e-3));
    for (size_t i = 1; i < far.stretch.size(); ++i)
        CHECK(far.stretch[i] == doctest::Approx(1.5));
    checkNear(glm::normalize(endOf(p, far)), glm::dvec3(0, 1, 0), 1e-3);

    // Without a max above 1 nothing stretches.
    fbik::Problem rigid = stretchyChain(1.0);
    rigid.effectors.push_back({.bone = 3, .position = glm::dvec3(0.0, 40.0, 0.0)});
    CHECK(fbik::solve(rigid).error == doctest::Approx(10.0).epsilon(1e-3));
}

TEST_CASE("fbik stretch: fullBodyIk stretches the joints of an IK handle with stretch on, only when asked")
{
    const glm::dvec3 target(25.0, 5.0, 0.0);
    stretchyArm(true);
    // Off by default: the arm reaches as far as it can, 20 cm.
    json r = run("fullBodyIk", {{"pins", pinHand(target)}, {"moveRoot", false}});
    CHECK(r["error"].get<double>() == doctest::Approx(glm::length(target) - 20.0).epsilon(1e-3));
    checkNear(node("lower").as<glm::dvec3>(Transform::kTranslate), glm::dvec3(10, 0, 0), 1e-9);

    stretchyArm(true);
    r = run("fullBodyIk", {{"pins", pinHand(target)}, {"moveRoot", false}, {"stretch", true}, {"key", true}});
    CHECK(r["error"].get<double>() < 1e-3);
    checkNear(pos("hand"), target, 1e-3);
    // The start joint keeps its place; the bones after it got longer, by no more than the handle's maxStretch.
    checkNear(node("upper").as<glm::dvec3>(Transform::kTranslate), glm::dvec3(0.0), 1e-9);
    const double lower = glm::length(node("lower").as<glm::dvec3>(Transform::kTranslate));
    const double hand = glm::length(node("hand").as<glm::dvec3>(Transform::kTranslate));
    CHECK(lower + hand >= glm::length(target) - 1e-4);
    CHECK(lower <= 14.0 + 1e-9);
    CHECK(hand <= 14.0 + 1e-9);
    CHECK(run("keyframe", {{"args", {"hand.translate"}}, {"query", true}, {"keyCount", true}}).get<int>() > 0);

    // A handle without stretch stretches nothing; maxStretch names joints to stretch anyway.
    stretchyArm(false);
    r = run("fullBodyIk", {{"pins", pinHand(target)}, {"moveRoot", false}, {"stretch", true}});
    CHECK(r["error"].get<double>() > 1.0);
    stretchyArm(false);
    r = run("fullBodyIk", {{"pins", pinHand(target)}, {"moveRoot", false}, {"stretch", true}, {"maxStretch", {{"hand", 1.6}}}});
    CHECK(r["error"].get<double>() < 1e-3);
    checkNear(node("lower").as<glm::dvec3>(Transform::kTranslate), glm::dvec3(10, 0, 0), 1e-9);
    CHECK(glm::length(node("hand").as<glm::dvec3>(Transform::kTranslate)) > 10.0);
}
