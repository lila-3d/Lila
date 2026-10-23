#include "TestUtil.h"

#include "scene/Ik.h"
#include "scene/IkSolver.h"
#include "scene/Rig.h"

#include <filesystem>

using namespace rigtest;
using namespace lila::scene;

namespace
{
/** Three joints bent slightly toward +Z: 10 cm bones from the origin up Y. */
std::string limb()
{
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "upper"}});
    run("joint", {{"position", v3(0, 10, 1)}, {"name", "lower"}});
    run("joint", {{"position", v3(0, 20, 0)}, {"name", "hand"}});
    return run("ikHandle", {{"startJoint", "upper"}, {"endEffector", "hand"}, {"name", "armIk"}}).get<std::string>();
}

double boneLength(const std::string& a, const std::string& b) { return glm::length(pos(b) - pos(a)); }

/** The elbow's offset from the shoulder-to-hand line, normalized. */
glm::dvec3 bendOf(const std::string& root, const std::string& mid, const std::string& end)
{
    glm::dvec3 axis = glm::normalize(pos(end) - pos(root));
    glm::dvec3 e = pos(mid) - pos(root);
    return glm::normalize(e - glm::dot(e, axis) * axis);
}
} // namespace

TEST_CASE("an ik handle starts on the end joint without moving the chain")
{
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "upper"}});
    run("joint", {{"position", v3(0, 10, 1)}, {"name", "lower"}});
    run("joint", {{"position", v3(0, 20, 0)}, {"name", "hand"}});
    glm::dvec3 before[3] = {pos("upper"), pos("lower"), pos("hand")};
    std::string h = run("ikHandle", {{"startJoint", "upper"}, {"endEffector", "hand"}}).get<std::string>();
    checkNear(pos(h), before[2]);
    checkNear(pos("upper"), before[0]);
    checkNear(pos("lower"), before[1]);
    checkNear(pos("hand"), before[2]);
    CHECK(ik::chainOf(graph(), node(h).id()).size() == 3);
    CHECK(node(h).as<std::string>(IkHandle::kSolver).empty());

    run("undo");
    CHECK(graph().find(h) == nullptr);
    CHECK(node("upper").as<double>(Joint::kIkBlend) == 0.0);
    run("setAttr", {{"args", {"upper.rotate", v3(0, 0, 90)}}});
    checkNear(pos("hand"), glm::dvec3(-20, 0, 0), 1e-6);
}

TEST_CASE("the two-bone solver reaches targets and keeps bone lengths")
{
    std::string h = limb();
    double a = boneLength("upper", "lower"), b = boneLength("lower", "hand");
    for (glm::dvec3 target : {glm::dvec3(5, 12, 3), glm::dvec3(-8, 4, 6), glm::dvec3(0, 2, 15), glm::dvec3(3, -10, 0)})
    {
        run("setAttr", {{"args", {h + ".translate", v3(target.x, target.y, target.z)}}});
        checkNear(pos("hand"), target);
        CHECK(boneLength("upper", "lower") == doctest::Approx(a));
        CHECK(boneLength("lower", "hand") == doctest::Approx(b));
        checkNear(pos("upper"), glm::dvec3(0.0));
    }
    // FK rotate stays the user's; the solution lives in solvedRotate.
    checkNear(node("upper").as<glm::dvec3>(Transform::kRotate), glm::dvec3(0.0));

    // Out of reach: the chain points straight at the target.
    run("setAttr", {{"args", {h + ".translate", v3(0, 0, 100)}}});
    checkNear(glm::normalize(pos("hand") - pos("upper")), glm::dvec3(0, 0, 1));
    CHECK(glm::length(pos("hand")) == doctest::Approx(a + b));
}

TEST_CASE("the pole vector and pole angle choose the bend plane")
{
    std::string h = limb();
    run("setAttr", {{"args", {h + ".translate", v3(0, 15, 0)}}});
    checkNear(bendOf("upper", "lower", "hand"), glm::dvec3(0, 0, 1));

    run("setAttr", {{"args", {h + ".poleVector", v3(1, 0, 0)}}});
    checkNear(pos("hand"), glm::dvec3(0, 15, 0));
    checkNear(bendOf("upper", "lower", "hand"), glm::dvec3(1, 0, 0));

    // A quarter turn about the shoulder-to-hand axis (+Y) takes +X to -Z.
    run("setAttr", {{"args", {h + ".poleAngle", 90.0}}});
    checkNear(pos("hand"), glm::dvec3(0, 15, 0));
    checkNear(bendOf("upper", "lower", "hand"), glm::dvec3(0, 0, -1));
}

TEST_CASE("ikBlend crossfades between the FK rotate and the solution")
{
    std::string h = limb();
    glm::dvec3 fkHand = pos("hand");
    run("setAttr", {{"args", {h + ".translate", v3(8, 10, 0)}}});
    checkNear(pos("hand"), glm::dvec3(8, 10, 0));
    glm::dmat3 ik = rigtest::world("upper");

    run("setAttr", {{"args", {h + ".ikBlend", 0.0}}});
    checkNear(pos("hand"), fkHand);

    run("setAttr", {{"args", {h + ".ikBlend", 0.5}}});
    glm::dmat3 half = rigtest::world("upper");
    double full = glm::angle(glm::quat_cast(ik));
    double mid = glm::angle(glm::quat_cast(half));
    CHECK(mid == doctest::Approx(full * 0.5).epsilon(1e-6));
}

TEST_CASE("FABRIK solves longer chains and handles survive save and load")
{
    run("newScene");
    run("select", {{"args", json::array()}});
    const char* names[] = {"s0", "s1", "s2", "s3", "s4"};
    for (int i = 0; i < 5; ++i)
        run("joint", {{"position", v3(0, 10.0 * i, i % 2 ? 0.5 : 0.0)}, {"name", names[i]}});
    std::string h = run("ikHandle", {{"startJoint", "s0"}, {"endEffector", "s4"}, {"poleVector", v3(0, 0, 1)}}).get<std::string>();
    std::vector<double> lens;
    for (int i = 0; i < 4; ++i)
        lens.push_back(boneLength(names[i], names[i + 1]));

    glm::dvec3 target(12, 25, 4);
    run("setAttr", {{"args", {h + ".translate", v3(target.x, target.y, target.z)}}});
    checkNear(pos("s4"), target, 1e-6);
    for (int i = 0; i < 4; ++i)
        CHECK(boneLength(names[i], names[i + 1]) == doctest::Approx(lens[size_t(i)]));

    auto path = (std::filesystem::temp_directory_path() / "lila_rig_tests" / "ik.lilaa").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    checkNear(pos("s4"), target, 1e-6);
    run("setAttr", {{"args", {h + ".translate", v3(-5, 30, 2)}}});
    checkNear(pos("s4"), glm::dvec3(-5, 30, 2), 1e-6);
}

TEST_CASE("solvers are registered by name and can be chosen")
{
    CHECK(ik::findSolver("ik2BoneSolver"));
    CHECK(ik::findSolver("ikFabrikSolver"));
    limb();
    CHECK_THROWS(run("ikHandle", {{"startJoint", "upper"}, {"endEffector", "hand"}}));
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "a"}});
    run("joint", {{"position", v3(0, 10, 0)}, {"name", "b"}});
    run("joint", {{"position", v3(0, 20, 0)}, {"name", "c"}});
    run("joint", {{"position", v3(0, 30, 0)}, {"name", "d"}});
    CHECK_THROWS(run("ikHandle", {{"startJoint", "a"}, {"endEffector", "d"}, {"solver", "ik2BoneSolver"}}));
    std::string h = run("ikHandle", {{"startJoint", "a"}, {"endEffector", "d"}, {"solver", "ikFabrikSolver"}}).get<std::string>();
    run("setAttr", {{"args", {h + ".translate", v3(10, 20, 0)}}});
    checkNear(pos("d"), glm::dvec3(10, 20, 0), 1e-6);
}
