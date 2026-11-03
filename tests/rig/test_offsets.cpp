#include "TestUtil.h"

#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Offsets.h"
#include "scene/Rig.h"
#include "scene/RigElements.h"

#include <glm/gtc/quaternion.hpp>

#include <map>

using namespace rigtest;
using namespace lila::scene;

namespace
{
void at(double seconds) { run("currentTime", {{"args", {seconds}}}); }
double sec(double frame) { return frame / kFps; }

glm::dquat rotOf(const glm::dmat4& m)
{
    glm::dmat3 r(m);
    for (int i = 0; i < 3; ++i)
        r[i] = glm::normalize(r[i]);
    return glm::normalize(glm::quat_cast(r));
}

double angleDeg(const glm::dquat& q) { return glm::degrees(2.0 * std::atan2(glm::length(glm::dvec3(q.x, q.y, q.z)), std::abs(q.w))); }

/** q close to r as rotations (either sign). */
double quatDiff(const glm::dquat& q, const glm::dquat& r) { return angleDeg(glm::inverse(q) * r); }

/** root > spine > head, with the arm shoulder > elbow > wrist under the spine; root, spine and head keyed over a second. */
void keyedCharacter()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"fps", kFps}, {"minTime", 0.0}, {"maxTime", 1.0}});
    run("joint", {{"name", "root"}, {"position", v3(0, 0, 0)}});
    run("joint", {{"name", "spine"}, {"position", v3(0, 100, 0)}, {"orientation", v3(0, 0, 20)}});
    run("joint", {{"name", "head"}, {"position", v3(0, 150, 0)}, {"orientation", v3(10, 30, 0)}});
    run("select", {{"args", {"spine"}}});
    run("joint", {{"name", "shoulder"}, {"position", v3(30, 140, 0)}});
    run("joint", {{"name", "elbow"}, {"position", v3(55, 135, 5)}});
    run("joint", {{"name", "wrist"}, {"position", v3(70, 100, 0)}});
    run("select", {{"args", json::array()}});

    run("setKeyframe", {{"args", {"root.translate", "root.rotate", "spine.rotate", "head.rotate"}}, {"time", 0.0}});
    at(1.0);
    run("setAttr", {{"args", {"root.translate", v3(40, 0, -10)}}});
    run("setAttr", {{"args", {"root.rotate", v3(0, 60, 0)}}});
    run("setAttr", {{"args", {"spine.rotate", v3(10, -20, 30)}}});
    run("setAttr", {{"args", {"head.rotate", v3(-20, 40, 10)}}});
    run("setKeyframe", {{"args", {"root.translate", "root.rotate", "spine.rotate", "head.rotate"}}, {"time", 1.0}});
    at(0.5);
    run("setAttr", {{"args", {"spine.rotate", v3(-15, 25, -10)}}});
    run("setKeyframe", {{"args", {"spine.rotate"}}, {"time", 0.5}});
    at(0.0);
}

const std::vector<double> kOffsetTimes = {0.0, sec(7), 0.5, sec(22), 1.0};

std::map<double, glm::dmat4> worldsByTime(const std::string& n)
{
    std::map<double, glm::dmat4> out;
    for (double t : kOffsetTimes)
    {
        at(t);
        out[t] = world(n);
    }
    return out;
}

/** The joint's rotation relative to its parent's axes. */
glm::dquat local(const std::string& joint, const std::string& parent) { return rotOf(glm::inverse(world(parent)) * world(joint)); }

} // namespace

TEST_CASE("an offset starts as nothing, and posing on it moves one value for the whole take")
{
    keyedCharacter();
    auto spineBase = worldsByTime("spine");
    std::map<double, glm::dquat> localBase;
    for (double t : kOffsetTimes)
    {
        at(t);
        localBase[t] = local("spine", "root");
    }

    std::string l = run("createOffset", {{"args", {"spine"}}, {"channels", "rotate"}});
    CHECK(l == "Offset_spine");
    CHECK(anim::activeLayer(graph()) == node(l).id());
    for (double t : kOffsetTimes)
    {
        at(t);
        CHECK(quatDiff(rotOf(world("spine")), rotOf(spineBase[t])) < 1e-6);
    }

    // Pose on the layer at one frame: it reads exactly there, and in the parent's axes the change is the same everywhere.
    at(0.3);
    const double posed = std::get<glm::dvec3>(graph().value({node("spine").id(), Transform::kRotate, -1})).z + 25.0;
    run("setKeyframe", {{"args", {"spine.rotateZ"}}, {"value", posed}});
    CHECK(std::get<glm::dvec3>(graph().value({node("spine").id(), Transform::kRotate, -1})).z == doctest::Approx(posed).epsilon(1e-6));
    const NodeId blend = offsets::blendOf(graph(), node(l).id(), node("spine").id());
    REQUIRE(blend);
    CHECK_FALSE(graph().source({blend, anim::BlendNode::kInputB, -1}));
    for (int c = 0; c < 3; ++c)
        CHECK_FALSE(graph().source({blend, anim::BlendNode::kInputB, c}));

    std::vector<glm::dquat> deltas;
    for (double t : kOffsetTimes)
    {
        at(t);
        deltas.push_back(local("spine", "root") * glm::inverse(localBase[t]));
    }
    for (const glm::dquat& d : deltas)
        CHECK(quatDiff(d, deltas.front()) < 1e-6);
    CHECK(angleDeg(deltas.front()) > 5.0);

    // The pivot stays put.
    for (double t : kOffsetTimes)
    {
        at(t);
        checkNear(glm::dvec3(world("spine")[3]), glm::dvec3(spineBase[t][3]), 1e-6);
    }
}

TEST_CASE("world and node spaces hold the offset in their axes")
{
    keyedCharacter();
    auto base = worldsByTime("head");
    std::string l = run("createOffset", {{"args", {"head"}}, {"space", "world"}, {"channels", "rotate"}});
    run("editOffset", {{"args", {l}}, {"joint", "head"}, {"rotate", v3(0, 0, 30)}});
    const glm::dquat want = glm::angleAxis(glm::radians(30.0), glm::dvec3(0, 0, 1));
    for (double t : kOffsetTimes)
    {
        at(t);
        CHECK(quatDiff(rotOf(world("head")) * glm::inverse(rotOf(base[t])), want) < 1e-6);
        checkNear(glm::dvec3(world("head")[3]), glm::dvec3(base[t][3]), 1e-6);
    }
    run("deleteOffset", {{"args", {l}}});

    std::string n = run("createOffset", {{"args", {"head"}}, {"space", "root"}, {"channels", "rotate"}});
    run("editOffset", {{"args", {n}}, {"joint", "head"}, {"rotate", v3(0, 0, 30)}});
    for (double t : kOffsetTimes)
    {
        at(t);
        const glm::dquat r = rotOf(world("root"));
        CHECK(quatDiff(rotOf(world("head")) * glm::inverse(rotOf(base[t])), r * want * glm::inverse(r)) < 1e-6);
    }
    // A space that moves with the joint would be a cycle.
    CHECK_THROWS(run("createOffset", {{"args", {"spine"}}, {"space", "head"}}));

    // Translation in world axes.
    auto spine = worldsByTime("spine");
    std::string tr = run("createOffset", {{"args", {"spine"}}, {"space", "world"}, {"channels", "translate"}});
    run("editOffset", {{"args", {tr}}, {"joint", "spine"}, {"translate", v3(0, 0, 10)}});
    for (double t : kOffsetTimes)
    {
        at(t);
        checkNear(glm::dvec3(world("spine")[3]), glm::dvec3(spine[t][3]) + glm::dvec3(0, 0, 10), 1e-6);
    }
}

TEST_CASE("a range eases the offset in and out, and posing works where it applies")
{
    keyedCharacter();
    auto base = worldsByTime("spine");
    std::map<double, glm::dmat4> baseAt;
    for (double f : {3.0, 6.0, 9.0, 10.5, 15.0, 19.5, 24.0, 27.0})
    {
        at(sec(f));
        baseAt[f] = world("spine");
    }
    std::string l = run("createOffset", {{"args", {"spine"}}, {"channels", "rotate"}, {"range", {9, 21}}, {"ease", 3}, {"space", "world"}});
    json info = run("listOffsets")[0];
    CHECK(info["range"][0].get<double>() == doctest::Approx(9));
    CHECK(info["range"][1].get<double>() == doctest::Approx(21));
    CHECK(info["ease"].get<double>() == doctest::Approx(3));
    run("editOffset", {{"args", {l}}, {"joint", "spine"}, {"rotate", v3(0, 0, 40)}});
    auto angleAt = [&](double f) {
        at(sec(f));
        return angleDeg(rotOf(world("spine")) * glm::inverse(rotOf(baseAt[f])));
    };
    CHECK(angleAt(6) < 1e-6);
    CHECK(angleAt(24) < 1e-6);
    CHECK(angleAt(15) == doctest::Approx(40).epsilon(1e-6));
    const double easing = angleAt(10.5);
    CHECK(easing > 1.0);
    CHECK(easing < 39.0);
    CHECK(angleAt(19.5) == doctest::Approx(easing).epsilon(1e-4));

    // No weight at frame 6, so there is nothing to pose there.
    at(sec(6));
    CHECK_THROWS(run("setKeyframe", {{"args", {"spine.rotateX"}}, {"value", 50.0}}));
    // Halfway up the ease the pose still lands exactly, and the full-strength frames carry more.
    at(sec(10.5));
    const double posed = std::get<glm::dvec3>(graph().value({node("spine").id(), Transform::kRotate, -1})).x + 8.0;
    run("setKeyframe", {{"args", {"spine.rotateX"}}, {"value", posed}});
    at(sec(10.5));
    CHECK(std::get<glm::dvec3>(graph().value({node("spine").id(), Transform::kRotate, -1})).x == doctest::Approx(posed).epsilon(1e-6));

    // Whole take again: constant everywhere.
    run("editOffset", {{"args", {l}}, {"range", "whole"}});
    CHECK(run("listOffsets")[0]["range"].is_null());
    CHECK(angleAt(3) == doctest::Approx(angleAt(27)).epsilon(1e-6));
    CHECK(angleAt(3) > 1.0);
}

TEST_CASE("compensation: children keep their world rotation, IK hands keep their contact")
{
    keyedCharacter();
    auto head = worldsByTime("head");
    std::string l = run("createOffset", {{"args", {"spine"}}, {"channels", "rotate"}, {"keepChildren", true}});
    json info = run("listOffsets")[0];
    CHECK(info["compensated"].size() == 2); // head and shoulder
    run("editOffset", {{"args", {l}}, {"joint", "spine"}, {"rotate", v3(0, 0, 15)}});
    for (double t : kOffsetTimes)
    {
        at(t);
        CHECK(quatDiff(rotOf(world("head")), rotOf(head[t])) < 1e-6);
        CHECK(glm::length(glm::dvec3(world("head")[3]) - glm::dvec3(head[t][3])) > 1.0);
    }
    run("undo");
    run("undo");
    CHECK(run("listOffsets").empty());

    // An IK chain under the offset part re-solves to its goal, so the hand stays planted wherever the goal is reachable.
    at(0.0);
    run("ikHandle", {{"startJoint", "shoulder"}, {"endEffector", "wrist"}, {"name", "armIk"}});
    run("parent", {{"args", {"armIk", "root"}}});
    auto shoulder = worldsByTime("shoulder");
    auto goal = worldsByTime("armIk");
    auto reached = worldsByTime("wrist");
    std::string o = run("createOffset", {{"args", {"spine"}}, {"channels", "rotate"}});
    run("editOffset", {{"args", {o}}, {"joint", "spine"}, {"rotate", v3(0, 0, -10)}});
    int planted = 0;
    for (double t : kOffsetTimes)
    {
        at(t);
        CHECK(glm::length(glm::dvec3(world("shoulder")[3]) - glm::dvec3(shoulder[t][3])) > 1.0);
        if (glm::length(glm::dvec3(reached[t][3]) - glm::dvec3(goal[t][3])) > 1e-3)
            continue;
        checkNear(glm::dvec3(world("wrist")[3]), glm::dvec3(goal[t][3]), 1e-3);
        ++planted;
    }
    CHECK(planted >= 2);
}

TEST_CASE("a locked hand keeps its contact under an offset of the body")
{
    keyedCharacter();
    const NodeId chain = rigel::createIkChain(graph(), node("shoulder").id(), node("wrist").id());
    REQUIRE(chain);
    rigel::setLock(graph(), chain, 0.0, true);
    at(0.0);
    const glm::dvec3 planted(world("wrist")[3]);
    const glm::dvec3 shoulder(world("shoulder")[3]);
    std::string o = run("createOffset", {{"args", {"spine"}}, {"channels", "rotate"}});
    run("editOffset", {{"args", {o}}, {"joint", "spine"}, {"rotate", v3(0, 0, -10)}});
    at(0.0);
    CHECK(glm::length(glm::dvec3(world("shoulder")[3]) - shoulder) > 1.0);
    checkNear(glm::dvec3(world("wrist")[3]), planted, 1e-3);
}

TEST_CASE("weight, mute, undo, stacking, delete and bake")
{
    keyedCharacter();
    auto base = worldsByTime("spine");
    std::string l = run("createOffset", {{"args", {"spine"}}, {"channels", "rotate"}, {"space", "world"}});
    run("editOffset", {{"args", {l}}, {"joint", "spine"}, {"rotate", v3(0, 30, 0)}});
    auto angle = [&](double t) {
        at(t);
        return angleDeg(rotOf(world("spine")) * glm::inverse(rotOf(base[t])));
    };
    CHECK(angle(0.5) == doctest::Approx(30).epsilon(1e-6));
    run("editOffset", {{"args", {l}}, {"weight", 0.5}});
    CHECK(angle(0.5) == doctest::Approx(15).epsilon(1e-6));
    run("editOffset", {{"args", {l}}, {"mute", true}});
    CHECK(angle(0.5) < 1e-6);
    run("undo");
    run("undo");
    CHECK(angle(0.5) == doctest::Approx(30).epsilon(1e-6));
    run("undo");
    CHECK(angle(0.5) < 1e-6);
    run("redo");
    CHECK(angle(0.5) == doctest::Approx(30).epsilon(1e-6));

    // A second offset stacks on the first; deleting it leaves the first alone.
    std::string second = run("createOffset", {{"args", {"spine"}}, {"channels", "rotate"}, {"space", "world"}});
    CHECK(second != l);
    run("editOffset", {{"args", {second}}, {"joint", "spine"}, {"rotate", v3(0, 10, 0)}});
    CHECK(angle(0.5) == doctest::Approx(40).epsilon(1e-6));
    run("deleteOffset", {{"args", {second}}});
    CHECK(angle(0.5) == doctest::Approx(30).epsilon(1e-6));
    CHECK(run("listOffsets").size() == 1);

    // Baking keeps the look on every frame and leaves no offset behind.
    std::map<double, glm::dmat4> offsetWorld;
    for (double t : kOffsetTimes)
    {
        at(t);
        offsetWorld[t] = world("spine");
    }
    run("bakeOffset", {{"args", {l}}});
    CHECK(run("listOffsets").empty());
    CHECK(graph().find(l) == nullptr);
    for (double t : kOffsetTimes)
    {
        at(t);
        CHECK(quatDiff(rotOf(world("spine")), rotOf(offsetWorld[t])) < 1e-3);
    }
    run("undo");
    CHECK(run("listOffsets").size() == 1);
    CHECK(angle(0.5) == doctest::Approx(30).epsilon(1e-6));
}
