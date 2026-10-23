#include "TestUtil.h"

#include "scene/Anim.h"
#include "scene/Rig.h"
#include "scene/Skin.h"

using namespace rigtest;
using namespace lila::scene;

namespace
{
const char* kJoints[] = {"bone_a", "bone_b", "bone_c"};
const double kTimes[] = {0.0, 1.0, 2.0};

std::string skinnedCylinder()
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 10}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}});
    return mesh;
}

void key(const std::string& plug, double t, double v) { run("setKeyframe", {{"args", {plug}}, {"time", t}, {"value", v}}); }

/** Euler keys on bone_a and bone_b at uneven times, a rotation curve on bone_c, and translate keys on bone_a. */
void animate()
{
    for (const char* j : kJoints)
        run("setAttr", {{"args", {std::string(j) + ".rotationInterpolation", 0}}});
    key("bone_a.rx", 0, 0);
    key("bone_a.rx", 1, 20);
    key("bone_a.rz", 0, 5);
    key("bone_a.rz", 2, -15);
    key("bone_a.tx", 0, 0);
    key("bone_a.tx", 2, 5);
    key("bone_b.ry", 0, 0);
    key("bone_b.ry", 1, 30);
    key("bone_b.rz", 0, 10);
    key("bone_b.rz", 1, 45);
    key("bone_c.rx", 0, 0);
    key("bone_c.rx", 1, 30);
    key("bone_c.rx", 2, 0);
    key("bone_c.ry", 0, 0);
    key("bone_c.ry", 1, 10);
    key("bone_c.ry", 2, 40);
    key("bone_c.rz", 0, 0);
    key("bone_c.rz", 1, 0);
    key("bone_c.rz", 2, 20);
    run("convertRotationCurves", {{"args", {"bone_c"}}, {"to", "smooth"}});
}

using Worlds = std::vector<std::vector<glm::dmat4>>;

Worlds worldsAtKeys()
{
    Worlds out;
    for (double t : kTimes)
    {
        graph().setTime(t);
        std::vector<glm::dmat4> w;
        for (const char* j : kJoints)
            w.push_back(world(j));
        out.push_back(w);
    }
    graph().setTime(0.0);
    return out;
}

void checkMatrix(const glm::dmat4& a, const glm::dmat4& b, double eps = 1e-6)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(a[c][r] == doctest::Approx(b[c][r]).epsilon(eps).scale(1.0));
}

std::vector<glm::dvec3> points(const std::string& mesh)
{
    MeshPtr m = outMesh(mesh);
    REQUIRE(m);
    return {m->positions().begin(), m->positions().end()};
}

void checkPoints(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b)
{
    REQUIRE(a.size() == b.size());
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, glm::length(a[i] - b[i]));
    CHECK(worst < 1e-6);
}
} // namespace

TEST_CASE("moving and reorienting skinned joints keeps the bind mesh")
{
    std::string mesh = skinnedCylinder();
    std::vector<glm::dvec3> bind = points(mesh);
    glm::dvec3 cBefore = pos("bone_c");

    run("moveSkinnedJoints", {{"args", {"bone_b"}}, {"position", v3(4, 2, 0)}, {"jointOrient", v3(0, 0, 30)}, {"rotateAxis", v3(10, 0, 0)}});
    checkNear(pos("bone_b"), glm::dvec3(4, 2, 0));
    checkNear(pos("bone_c"), cBefore);
    CHECK(graph().find("skeletonEdit") == nullptr);
    // World pose is kept, so bone_b keeps its old world rotation; its bind pose is the reoriented one.
    run("goToBindPose", {{"args", {mesh}}});
    checkPoints(points(mesh), bind);
    checkNear(pos("bone_b"), glm::dvec3(4, 2, 0));

    // Posing still deforms, bending at the moved joint.
    run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 40)}}});
    CHECK(glm::length(points(mesh)[0] - bind[0]) + glm::length(points(mesh).back() - bind.back()) > 1.0);

    run("undo");
    run("undo");
    run("undo");
    checkNear(pos("bone_b"), glm::dvec3(0.0));
    checkPoints(points(mesh), bind);
}

TEST_CASE("an orient and rotate-axis change keeps every joint's world matrix at every key")
{
    skinnedCylinder();
    animate();
    Worlds before = worldsAtKeys();

    run("moveSkinnedJoints", {{"args", {"bone_b"}}, {"jointOrient", v3(15, -20, 30)}, {"rotateAxis", v3(0, 25, 0)}});
    run("moveSkinnedJoints", {{"args", {"bone_c"}}, {"jointOrient", v3(0, 90, 0)}, {"rotateAxis", v3(-30, 0, 10)}});
    CHECK(node("bone_b").as<glm::dvec3>(Joint::kJointOrient).z == doctest::Approx(30));
    Worlds after = worldsAtKeys();
    for (size_t t = 0; t < before.size(); ++t)
        for (size_t j = 0; j < 3; ++j)
            checkMatrix(after[t][j], before[t][j]);

    // The rotation curve is exact between keys too.
    for (double t : {0.25, 1.5})
    {
        graph().setTime(t);
        glm::dmat4 c = world("bone_c");
        glm::dmat4 b = world("bone_b");
        run("undo");
        run("undo");
        graph().setTime(t);
        glm::dmat4 c0 = world("bone_c"), b0 = world("bone_b");
        run("redo");
        run("redo");
        // bone_b's Euler keys are only exact at keys; bone_c is relative to it, so compare its local frame.
        checkMatrix(glm::inverse(b) * c, glm::inverse(b0) * c0);
    }
}

TEST_CASE("moving an animated joint shifts its keys and its children keep their place")
{
    skinnedCylinder();
    animate();
    Worlds before = worldsAtKeys();
    glm::dvec3 bT = node("bone_b").as<glm::dvec3>(Transform::kTranslate);

    run("moveSkinnedJoints", {{"args", {"bone_b"}}, {"translate", v3(bT.x + 3, bT.y - 4, bT.z + 1)}});
    checkNear(node("bone_b").as<glm::dvec3>(Transform::kTranslate), bT + glm::dvec3(3, -4, 1));
    Worlds after = worldsAtKeys();
    for (size_t t = 0; t < before.size(); ++t)
    {
        checkMatrix(after[t][0], before[t][0]);
        checkMatrix(after[t][2], before[t][2]);
        // The moved joint keeps its world rotation.
        for (int c = 0; c < 3; ++c)
            checkNear(glm::dvec3(after[t][1][c]), glm::dvec3(before[t][1][c]));
    }
}

TEST_CASE("editSkeleton holds the animation in bind pose and restores it re-expressed")
{
    std::string mesh = skinnedCylinder();
    std::vector<glm::dvec3> bind = points(mesh);
    animate();
    graph().setTime(1.0);
    Worlds before = worldsAtKeys();
    graph().setTime(1.0);
    std::vector<glm::dvec3> posed = points(mesh);

    CHECK(run("editSkeleton", {{"args", {mesh}}}) == true);
    REQUIRE(graph().find("skeletonEdit"));
    // Bind pose, no animation, mesh at its bind shape.
    checkPoints(points(mesh), bind);
    checkNear(pos("bone_c"), glm::dvec3(0, 50, 0));
    graph().setTime(2.0);
    checkNear(pos("bone_c"), glm::dvec3(0, 50, 0));
    CHECK(anim::curveFor(graph(), graph().plug("bone_a.rx")) == kNoNode);

    run("setAttr", {{"args", {"bone_b.jointOrient", v3(0, 0, -20)}}});
    run("setAttr", {{"args", {"bone_a.rotateAxis", v3(5, 5, 0)}}});
    CHECK(run("editSkeleton", {{"on", false}}) == false);
    CHECK(graph().find("skeletonEdit") == nullptr);
    CHECK(anim::curveFor(graph(), graph().plug("bone_a.rx")) != kNoNode);
    Worlds after = worldsAtKeys();
    for (size_t t = 0; t < before.size(); ++t)
        for (size_t j = 0; j < 3; ++j)
            checkMatrix(after[t][j], before[t][j]);

    // Undo goes back into the mode, then out of it to the original rig.
    run("undo");
    CHECK(graph().find("skeletonEdit"));
    run("undo");
    run("undo");
    run("undo");
    CHECK(graph().find("skeletonEdit") == nullptr);
    graph().setTime(1.0);
    checkPoints(points(mesh), posed);
}

TEST_CASE("preserve channels keeps raw key values")
{
    skinnedCylinder();
    animate();
    json keysBefore = run("keyframe", {{"args", {"bone_b"}}, {"query", true}, {"valueChange", true}});
    run("moveSkinnedJoints", {{"args", {"bone_b"}}, {"jointOrient", v3(0, 0, 30)}, {"preserve", "channels"}});
    CHECK(run("keyframe", {{"args", {"bone_b"}}, {"query", true}, {"valueChange", true}}) == keysBefore);
}

TEST_CASE("preserve channels keeps unkeyed children in place when a parent is reoriented")
{
    skinnedCylinder();
    run("setAttr", {{"args", {"bone_b.rotationInterpolation", 0}}});
    key("bone_b.rz", 0, 10);
    key("bone_b.rz", 1, 45);
    graph().setTime(0.0);
    glm::dmat4 cBefore = world("bone_c");
    json keysBefore = run("keyframe", {{"args", {"bone_b"}}, {"query", true}, {"valueChange", true}});

    run("moveSkinnedJoints", {{"args", {"bone_b"}}, {"jointOrient", v3(0, 0, 30)}, {"preserve", "channels"}});
    CHECK(run("keyframe", {{"args", {"bone_b"}}, {"query", true}, {"valueChange", true}}) == keysBefore);
    CHECK(anim::curveFor(graph(), graph().plug("bone_c.tx")) == kNoNode);
    CHECK(anim::curveFor(graph(), graph().plug("bone_c.rx")) == kNoNode);
    graph().setTime(0.0);
    checkMatrix(world("bone_c"), cBefore);
}
