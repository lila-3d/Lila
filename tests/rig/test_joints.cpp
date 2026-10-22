#include "TestUtil.h"

#include "scene/Rig.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace rigtest;
using namespace lila::scene;

TEST_CASE("jointOrient sits between rotate and the parent")
{
    run("newScene");
    std::string root = run("joint", {{"name", "root"}, {"orientation", v3(0, 0, 90)}});
    std::string tip = run("joint", {{"name", "tip"}});
    run("setAttr", {{"args", {tip + ".translate", v3(10, 0, 0)}}});
    // jointOrient 90 about Z turns the child's +X offset into world +Y.
    checkNear(pos(tip), {0, 10, 0});

    run("setAttr", {{"args", {root + ".rotate", v3(90, 0, 0)}}});
    glm::dmat4 expected = eulerToMatrix({0, 0, 90}, RotateOrder::XYZ) * eulerToMatrix({90, 0, 0}, RotateOrder::XYZ);
    glm::dmat4 w = world(root);
    for (int c = 0; c < 3; ++c)
        checkNear(glm::dvec3(w[c]), glm::dvec3(expected[c]));

    // setLocalMatrix solves rotate around jointOrient, so the manipulators and parenting stay exact.
    glm::dmat4 want = glm::translate(glm::dmat4(1.0), glm::dvec3(1, 2, 3)) * eulerToMatrix({10, 20, 30}, RotateOrder::XYZ);
    Transform::setLocalMatrix(graph(), node(root).id(), want);
    glm::dmat4 got = world(root);
    for (int c = 0; c < 4; ++c)
        checkNear(glm::dvec3(got[c]), glm::dvec3(want[c]));
    checkNear(glm::dvec3(node(root).as<glm::dvec3>(Joint::kJointOrient)), {0, 0, 90});
}

TEST_CASE("joint builds chains under the selection and keeps world positions")
{
    run("newScene");
    std::string a = run("joint", {{"position", v3(0, 0, 0)}, {"name", "upper_arm_l"}});
    std::string b = run("joint", {{"position", v3(10, 0, 0)}, {"name", "lower_arm_l"}});
    std::string c = run("joint", {{"position", v3(10, 10, 0)}, {"name", "hand_l"}});
    CHECK(graph().parent(node(b).id()) == node(a).id());
    CHECK(graph().parent(node(c).id()) == node(b).id());
    checkNear(pos(b), {10, 0, 0});
    checkNear(pos(c), {10, 10, 0});

    run("orientJoint", {{"args", {a}}});
    checkNear(pos(b), {10, 0, 0});
    checkNear(pos(c), {10, 10, 0});
    checkNear(node(b).as<glm::dvec3>(Transform::kRotate), {0, 0, 0});
    // Each joint's X points at its child.
    checkNear(glm::normalize(glm::dvec3(world(b)[0])), {0, 1, 0});
    checkNear(glm::normalize(glm::dvec3(world(a)[0])), {1, 0, 0});
    // The leaf takes its parent's orientation.
    checkNear(glm::normalize(glm::dvec3(world(c)[0])), {0, 1, 0});
    // And the child sits along X at the bone length.
    checkNear(node(c).as<glm::dvec3>(Transform::kTranslate), {10, 0, 0});

    CHECK(run("jointLength", {{"args", {b}}}).get<double>() == doctest::Approx(10));
    run("jointLength", {{"args", {b}}, {"length", 20}});
    checkNear(pos(c), {10, 20, 0});
    run("undo");

    json inserted = run("insertJoint", {{"args", {a}}, {"count", 1}});
    REQUIRE(inserted.size() == 1);
    checkNear(pos(inserted[0]), {5, 0, 0});
    CHECK(graph().parent(node(b).id()) == node(inserted[0]).id());
    checkNear(pos(b), {10, 0, 0});

    run("rerootSkeleton", {{"args", {c}}});
    CHECK(graph().parent(node(c).id()) == kNoNode);
    checkNear(pos(a), {0, 0, 0});
    checkNear(pos(b), {10, 0, 0});
}

TEST_CASE("mirrorJoints swaps names and sides and reflects positions")
{
    run("newScene");
    std::string hip = run("joint", {{"position", v3(0, 100, 0)}, {"name", "pelvis"}});
    std::string leg = run("joint", {{"position", v3(10, 90, 0)}, {"name", "upper_leg_l"}});
    std::string knee = run("joint", {{"position", v3(12, 50, 5)}, {"name", "lower_leg_l"}});
    run("setAttr", {{"args", {leg + ".side", 1}}});
    run("orientJoint", {{"args", {leg}}});
    run("setAttr", {{"args", {leg + ".rotate", v3(0, 0, 25)}}});

    json made = run("mirrorJoints", {{"args", {leg}}});
    REQUIRE(made.size() == 2);
    CHECK(made[0] == "upper_leg_r");
    CHECK(made[1] == "lower_leg_r");
    CHECK(graph().parent(node("upper_leg_r").id()) == node(hip).id());
    CHECK(node("upper_leg_r").as<int64_t>(Joint::kSide) == Joint::kRight);
    glm::dvec3 l = pos(knee), r = pos("lower_leg_r");
    checkNear(r, {-l.x, l.y, l.z});
    checkNear(pos("upper_leg_r"), {-pos(leg).x, pos(leg).y, pos(leg).z});
    // Behavior: same rotate values, all three axes flipped through the mirror.
    checkNear(node("upper_leg_r").as<glm::dvec3>(Transform::kRotate), {0, 0, 25});
    glm::dvec3 lx = glm::dvec3(world(leg)[0]), rx = glm::dvec3(world("upper_leg_r")[0]);
    checkNear(rx, {lx.x, -lx.y, -lx.z});

    CHECK(rig::mirrorName("upper_leg_l", "_l", "_r") == "upper_leg_r");
    CHECK(rig::mirrorName("l_hand", "l_", "r_") == "r_hand");
    CHECK(rig::mirrorName("finger_l_01", "_l", "_r") == "finger_r_01");
    CHECK(rig::mirrorName("spine_01", "_l", "_r") == "spine_01");
}
