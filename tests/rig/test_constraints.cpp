#include "TestUtil.h"

#include "scene/Constraint.h"

using namespace rigtest;
using namespace lila::scene;

TEST_CASE("parentConstraint follows its target with the offset kept")
{
    run("newScene");
    run("spaceLocator", {{"name", "Driver"}});
    run("setAttr", {{"args", {"Driver.translate", v3(10, 0, 0)}}});
    run("spaceLocator", {{"name", "Follower"}});
    run("setAttr", {{"args", {"Follower.translate", v3(10, 5, 0)}}});
    std::string c = run("parentConstraint", {{"args", {"Driver", "Follower"}}});
    checkNear(pos("Follower"), {10, 5, 0});

    run("setAttr", {{"args", {"Driver.translate", v3(20, 0, 0)}}});
    checkNear(pos("Follower"), {20, 5, 0});
    // Rotating the driver swings the follower around it like a child.
    run("setAttr", {{"args", {"Driver.rotate", v3(0, 0, 90)}}});
    checkNear(pos("Follower"), {15, 0, 0});
    checkNear(node("Follower").as<glm::dvec3>(Transform::kRotate), {0, 0, 90});

    // Second target at half weight blends halfway.
    run("spaceLocator", {{"name", "Other"}});
    run("setAttr", {{"args", {"Driver.rotate", v3(0, 0, 0)}}});
    run("setAttr", {{"args", {"Driver.translate", v3(10, 0, 0)}}});
    run("parentConstraint", {{"args", {"Other", "Follower"}}, {"maintainOffset", false}});
    run("setAttr", {{"args", {"Other.translate", v3(0, 20, 0)}}});
    // Driver alone puts it at (10, 5, 0); Other alone (no offset) at (0, 20, 0).
    checkNear(pos("Follower"), {5, 12.5, 0});

    run("removeConstraint", {{"args", {"Follower"}}});
    checkNear(pos("Follower"), {5, 12.5, 0});
    CHECK_FALSE(graph().source({node("Follower").id(), Transform::kTranslate, -1}));
    run("undo");
    CHECK(graph().source({node("Follower").id(), Transform::kTranslate, -1}));
}

TEST_CASE("point, orient and aim constraints")
{
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("spaceLocator", {{"name", "B"}});
    run("setAttr", {{"args", {"B.translate", v3(0, 10, 0)}}});
    run("spaceLocator", {{"name", "P"}});
    run("pointConstraint", {{"args", {"A", "B", "P"}}, {"maintainOffset", false}});
    checkNear(pos("P"), {0, 5, 0});
    run("setAttr", {{"args", {"A.translate", v3(10, 0, 0)}}});
    checkNear(pos("P"), {5, 5, 0});
    run("setAttr", {{"args", {"P_pointConstraint.target1Weight", 3.0}}});
    checkNear(pos("P"), {2.5, 7.5, 0});

    // Skipped axes stay at their rest value.
    run("spaceLocator", {{"name", "S"}});
    run("setAttr", {{"args", {"S.translate", v3(0, 0, 7)}}});
    run("pointConstraint", {{"args", {"B", "S"}}, {"maintainOffset", false}, {"skip", {"z"}}});
    checkNear(pos("S"), {0, 10, 7});

    run("spaceLocator", {{"name", "O"}});
    run("setAttr", {{"args", {"O.rotate", v3(0, 30, 0)}}});
    run("orientConstraint", {{"args", {"A", "O"}}});
    checkNear(node("O").as<glm::dvec3>(Transform::kRotate), {0, 30, 0});
    run("setAttr", {{"args", {"A.rotate", v3(0, 0, 45)}}});
    glm::dmat3 expect = glm::dmat3(eulerToMatrix({0, 0, 45}, RotateOrder::XYZ)) * glm::dmat3(eulerToMatrix({0, 30, 0}, RotateOrder::XYZ));
    glm::dmat4 got = world("O");
    for (int k = 0; k < 3; ++k)
        checkNear(glm::dvec3(got[k]), expect[k]);

    // Aim: the follower's +X points at the target, its +Y leans to world up.
    run("spaceLocator", {{"name", "Eye"}});
    run("setAttr", {{"args", {"Eye.translate", v3(0, 0, -20)}}});
    run("spaceLocator", {{"name", "Look"}});
    run("aimConstraint", {{"args", {"Look", "Eye"}}, {"maintainOffset", false}});
    run("setAttr", {{"args", {"Look.translate", v3(0, 0, 30)}}});
    checkNear(glm::dvec3(world("Eye")[0]), {0, 0, 1});
    checkNear(glm::dvec3(world("Eye")[1]), {0, 1, 0});
    run("setAttr", {{"args", {"Look.translate", v3(20, 0, -20)}}});
    checkNear(glm::dvec3(world("Eye")[0]), {1, 0, 0});
}

TEST_CASE("constraints work under moving parents and on joints")
{
    run("newScene");
    run("spaceLocator", {{"name", "Target"}});
    run("setAttr", {{"args", {"Target.translate", v3(5, 5, 5)}}});
    run("group", {{"empty", true}, {"name", "Rig"}});
    run("select", {{"args", json::array()}});
    std::string j = run("joint", {{"name", "hand_l"}, {"parent", "Rig"}, {"orientation", v3(0, 0, 30)}});
    run("setAttr", {{"args", {"Rig.translate", v3(100, 0, 0)}}});
    run("setAttr", {{"args", {"Rig.rotate", v3(0, 90, 0)}}});
    run("parentConstraint", {{"args", {"Target", j}}, {"maintainOffset", false}});
    checkNear(pos(j), {5, 5, 5});
    checkNear(glm::normalize(glm::dvec3(world(j)[0])), {1, 0, 0});
    run("setAttr", {{"args", {"Rig.translate", v3(-50, 0, 0)}}});
    checkNear(pos(j), {5, 5, 5});

    // A constraint can't drive something its target depends on.
    run("spaceLocator", {{"name", "Kid"}, {"skipSelect", true}});
    run("parent", {{"args", {"Kid", "Target"}}});
    CHECK_THROWS(run("pointConstraint", {{"args", {"Kid", "Target"}}}));
}
