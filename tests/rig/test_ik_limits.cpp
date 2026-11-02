#include "TestUtil.h"

#include "scene/FullBodyIk.h"
#include "scene/Ik.h"
#include "scene/IkSolver.h"
#include "scene/Rig.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <numbers>

using namespace rigtest;
using namespace lila::scene;

namespace
{
constexpr double kLimitDeg = std::numbers::pi / 180.0;

/** joints 10 cm apart along +X, unrotated, from the origin. */
ik::Problem straightChain(int joints)
{
    ik::Problem p;
    for (int i = 0; i < joints; ++i)
        p.world.push_back(glm::translate(glm::dmat4(1.0), glm::dvec3(10.0 * i, 0, 0)));
    p.maxIterations = 200;
    p.tolerance = 1e-6;
    return p;
}

const ik::Solver& solverNamed(const char* name)
{
    const ik::Solver* s = ik::findSolver(name);
    REQUIRE(s);
    return *s;
}

/** Joint positions for a solution of a straightChain, bones lengthened by stretch. */
std::vector<glm::dvec3> chainPositions(const ik::Result& r, double stretch = 1.0)
{
    std::vector<glm::dvec3> pos{glm::dvec3(0.0)};
    glm::dmat3 frame(1.0);
    for (const glm::dquat& q : r.rotation)
    {
        frame = frame * glm::mat3_cast(q);
        pos.push_back(pos.back() + frame * glm::dvec3(10.0 * stretch, 0, 0));
    }
    return pos;
}

/** The signed angle of q about axis, when q turns about axis only. */
double angleAbout(const glm::dquat& q, const glm::dvec3& axis)
{
    const glm::dquat r = q.w < 0.0 ? -q : q;
    return 2.0 * std::atan2(glm::dot(glm::dvec3(r.x, r.y, r.z), axis), r.w);
}

ik::JointLimit hingeLimit(double minDeg, double maxDeg)
{
    ik::JointLimit l;
    l.kind = ik::JointLimit::Kind::Hinge;
    l.axis = glm::dvec3(0, 0, 1);
    l.minAngle = minDeg * kLimitDeg;
    l.maxAngle = maxDeg * kLimitDeg;
    return l;
}
} // namespace

TEST_CASE("ik limits: clampRotate holds a hinge to its axis and range and a cone to its angle")
{
    const glm::dquat bent = glm::angleAxis(50 * kLimitDeg, glm::normalize(glm::dvec3(0.2, 0, 1)));
    const glm::dquat h = ik::clampRotate(hingeLimit(-10, 30), bent);
    CHECK(angleAbout(h, {0, 0, 1}) == doctest::Approx(30 * kLimitDeg));
    CHECK(glm::length(glm::dvec3(h.x, h.y, 0.0)) < 1e-12);

    ik::JointLimit cone;
    cone.kind = ik::JointLimit::Kind::Cone;
    cone.cone = 20 * kLimitDeg;
    const glm::dquat twistAndSwing = glm::angleAxis(70 * kLimitDeg, glm::dvec3(0, 1, 0)) * glm::angleAxis(40 * kLimitDeg, glm::dvec3(1, 0, 0));
    const glm::dquat c = ik::clampRotate(cone, twistAndSwing);
    const glm::dvec3 bone = glm::mat3_cast(c) * glm::dvec3(1, 0, 0);
    CHECK(std::acos(std::clamp(bone.x, -1.0, 1.0)) == doctest::Approx(20 * kLimitDeg));
    // The twist about the bone is kept.
    const glm::dquat inside = glm::angleAxis(40 * kLimitDeg, glm::dvec3(1, 0, 0));
    CHECK(glm::length(glm::dvec4(ik::clampRotate(cone, inside).x, ik::clampRotate(cone, inside).y, ik::clampRotate(cone, inside).z,
                                 ik::clampRotate(cone, inside).w) -
                      glm::dvec4(inside.x, inside.y, inside.z, inside.w)) < 1e-12);
}

TEST_CASE("ik limits: a hinge elbow holds the two-bone and FABRIK solves to its range")
{
    for (const char* name : {"ik2BoneSolver", "ikFabrikSolver"})
    {
        CAPTURE(name);
        ik::Problem p = straightChain(3);
        p.target = glm::dvec3(8, 8, 0);
        p.limits = {ik::JointLimit{}, hingeLimit(0, 30)};
        const ik::Result r = ik::solve(solverNamed(name), p);
        REQUIRE(r.rotation.size() == 2);
        const double elbow = angleAbout(r.rotation[1], {0, 0, 1});
        CHECK(elbow >= -1e-9);
        CHECK(elbow <= 30 * kLimitDeg + 1e-9);
        CHECK(glm::length(glm::dvec3(r.rotation[1].x, r.rotation[1].y, 0.0)) < 1e-9);
        // Free, the elbow would fold well past 30 degrees to reach.
        ik::Problem free = p;
        free.limits.clear();
        CHECK(std::abs(angleAbout(ik::solve(solverNamed(name), free).rotation[1], {0, 0, 1})) > 60 * kLimitDeg);
        // Within its range the chain still gets as close as it can: straight-line reach shortened by the 30 degree elbow.
        const glm::dvec3 end = chainPositions(r).back();
        CHECK(glm::length(end) == doctest::Approx(glm::length(glm::dvec3(10, 0, 0) + glm::dvec3(std::cos(30 * kLimitDeg), std::sin(30 * kLimitDeg), 0) * 10.0))
                                      .epsilon(1e-3));
    }

    // A long chain of hinges: every joint stays in range.
    ik::Problem p = straightChain(5);
    p.target = glm::dvec3(5, 25, 0);
    p.limits = {hingeLimit(-25, 25), hingeLimit(-25, 25), hingeLimit(-25, 25), hingeLimit(-25, 25)};
    const ik::Result r = ik::solve(solverNamed("ikFabrikSolver"), p);
    for (const glm::dquat& q : r.rotation)
        CHECK(std::abs(angleAbout(q, {0, 0, 1})) <= 25 * kLimitDeg + 1e-9);
}

TEST_CASE("ik limits: a cone keeps the bone within its angle")
{
    ik::Problem p = straightChain(4);
    p.target = glm::dvec3(5, 22, 0);
    ik::JointLimit cone;
    cone.kind = ik::JointLimit::Kind::Cone;
    cone.cone = 30 * kLimitDeg;
    p.limits = {cone};
    const ik::Result r = ik::solve(solverNamed("ikFabrikSolver"), p);
    const glm::dvec3 bone = glm::mat3_cast(r.rotation[0]) * glm::dvec3(1, 0, 0);
    CHECK(std::acos(std::clamp(bone.x, -1.0, 1.0)) <= 30 * kLimitDeg + 1e-9);
    // The joints past it still bring the end to the target.
    CHECK(glm::length(chainPositions(r).back() - p.target) < 1e-3);
}

TEST_CASE("ik limits: a stiff joint keeps its rotation and the rest of the chain reaches")
{
    ik::Problem p = straightChain(4);
    p.target = glm::dvec3(15, 15, 0);
    ik::JointLimit stiff;
    stiff.stiffness = 1.0;
    p.limits = {stiff};
    const ik::Result r = ik::solve(solverNamed("ikFabrikSolver"), p);
    CHECK(std::abs(r.rotation[0].w) == doctest::Approx(1.0));
    CHECK(glm::length(chainPositions(r).back() - p.target) < 1e-3);
}

TEST_CASE("ik limits: the preferred angle picks which way a straight chain bends")
{
    for (const char* name : {"ik2BoneSolver", "ikFabrikSolver"})
    {
        CAPTURE(name);
        double side[2] = {0.0, 0.0};
        for (int k = 0; k < 2; ++k)
        {
            ik::Problem p = straightChain(3);
            p.target = glm::dvec3(15, 0, 0);
            ik::JointLimit elbow;
            elbow.preferred = glm::angleAxis((k == 0 ? 40.0 : -40.0) * kLimitDeg, glm::dvec3(0, 0, 1));
            p.limits = {ik::JointLimit{}, elbow};
            const ik::Result r = ik::solve(solverNamed(name), p);
            const auto pos = chainPositions(r);
            CHECK(glm::length(pos.back() - p.target) < 1e-4);
            side[k] = pos[1].y;
        }
        // Bending the elbow toward +Y puts the elbow on -Y once the hand is back on the line, and the other way round.
        CHECK(side[0] < -1.0);
        CHECK(side[1] > 1.0);
    }
}

TEST_CASE("ik limits: stretch reaches past the chain's length up to its max, and soft IK eases out")
{
    for (const char* name : {"ik2BoneSolver", "ikFabrikSolver"})
    {
        CAPTURE(name);
        ik::Problem p = straightChain(3);
        p.target = glm::dvec3(0, 30, 0);
        p.maxStretch = 2.0;
        ik::Result r = ik::solve(solverNamed(name), p);
        CHECK(r.stretch == doctest::Approx(1.5));
        CHECK(glm::length(chainPositions(r, r.stretch).back() - p.target) < 1e-4);

        p.maxStretch = 1.25;
        r = ik::solve(solverNamed(name), p);
        CHECK(r.stretch == doctest::Approx(1.25));
        CHECK(glm::length(chainPositions(r, r.stretch).back()) == doctest::Approx(25.0));

        // Soft: at full reach the end still falls short, well inside it nothing changes.
        ik::Problem soft = straightChain(3);
        soft.softness = 3.0;
        soft.target = glm::dvec3(0, 20, 0);
        r = ik::solve(solverNamed(name), soft);
        CHECK(r.stretch == 1.0);
        const double reached = glm::length(chainPositions(r).back());
        CHECK(reached < 19.9);
        CHECK(reached > 17.0);
        soft.target = glm::dvec3(0, 12, 0);
        CHECK(glm::length(chainPositions(ik::solve(solverNamed(name), soft)).back() - soft.target) < 1e-4);

        // Soft and stretch together reach the target again.
        soft.maxStretch = 2.0;
        soft.target = glm::dvec3(0, 20, 0);
        r = ik::solve(solverNamed(name), soft);
        CHECK(r.stretch > 1.0);
        CHECK(glm::length(chainPositions(r, r.stretch).back() - soft.target) < 1e-4);
    }
}

TEST_CASE("ik limits: joint attributes reach the handle through ikLimit, and a stretching handle lengthens the bones")
{
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "upper"}});
    run("joint", {{"position", v3(0, 10, 1)}, {"name", "lower"}});
    run("joint", {{"position", v3(0, 20, 0)}, {"name", "hand"}});
    const std::string h = run("ikHandle", {{"startJoint", "upper"}, {"endEffector", "hand"}, {"name", "armIk"}}).get<std::string>();

    run("ikLimit", {{"args", {"lower"}}, {"type", "hinge"}, {"axis", "z"}, {"min", 0.0}, {"max", 0.0}, {"stiffness", 0.25}});
    CHECK(node("lower").as<int64_t>(Joint::kLimitType) == Joint::kHinge);
    CHECK(node("lower").as<double>(Joint::kStiffness) == doctest::Approx(0.25));
    const auto& handle = static_cast<const IkHandle&>(node(h));
    CHECK(handle.slotLimit(1).kind == ik::JointLimit::Kind::Hinge);
    CHECK(handle.slotLimit(1).stiffness == doctest::Approx(0.25));

    // A locked elbow: wherever the handle goes the elbow keeps its rest rotate.
    run("setAttr", {{"args", {h + ".translate", v3(4, 12, 6)}}});
    graph().evaluate();
    checkNear(node("lower").as<glm::dvec3>(Joint::kSolvedRotate), glm::dvec3(0.0), 1e-6);
    run("undo");
    run("undo");
    CHECK(node("lower").as<int64_t>(Joint::kLimitType) == Joint::kNoLimit);
    CHECK(handle.slotLimit(1).kind == ik::JointLimit::Kind::None);

    const double length = glm::length(pos("lower") - pos("upper")) + glm::length(pos("hand") - pos("lower"));
    const glm::dvec3 fk = node("lower").as<glm::dvec3>(Transform::kTranslate);
    run("setAttr", {{"args", {h + ".stretch", true}}});
    run("setAttr", {{"args", {h + ".maxStretch", 3.0}}});
    run("setAttr", {{"args", {h + ".translate", v3(0, 0, 30)}}});
    checkNear(pos("hand"), glm::dvec3(0, 0, 30), 1e-6);
    CHECK(glm::length(pos("lower") - pos("upper")) + glm::length(pos("hand") - pos("lower")) == doctest::Approx(30.0));
    CHECK(length < 25.0);
    // FK translate is untouched; ikBlend 0 is the unstretched chain.
    checkNear(node("lower").as<glm::dvec3>(Transform::kTranslate), fk);
    run("setAttr", {{"args", {h + ".ikBlend", 0.0}}});
    CHECK(glm::length(pos("lower") - pos("upper")) + glm::length(pos("hand") - pos("lower")) == doctest::Approx(length));
}

TEST_CASE("ik limits: full-body IK holds a hinge joint to its range")
{
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "hip"}});
    run("joint", {{"position", v3(0, -10, 0)}, {"name", "knee"}});
    run("joint", {{"position", v3(0, -20, 0)}, {"name", "ankle"}});
    run("ikLimit", {{"args", {"knee"}}, {"type", "hinge"}, {"axis", "z"}, {"min", 0.0}, {"max", 20.0}});

    std::vector<NodeId> index;
    fbik::PoseOptions opts;
    opts.moveRoot = false;
    fbik::Problem p = fbik::problemFor(graph(), node("hip").id(), opts, index);
    REQUIRE(index.size() == 3);
    CHECK(p.bones[1].limit.kind == ik::JointLimit::Kind::Hinge);
    p.effectors.push_back({.bone = 2, .position = glm::dvec3(6, -12, 0)});
    const fbik::Solution s = fbik::solve(p);
    const double knee = angleAbout(s.rotation[1], {0, 0, 1});
    CHECK(knee >= -1e-9);
    CHECK(knee <= 20 * kLimitDeg + 1e-9);
    CHECK(glm::length(glm::dvec3(s.rotation[1].x, s.rotation[1].y, 0.0)) < 1e-9);
}
