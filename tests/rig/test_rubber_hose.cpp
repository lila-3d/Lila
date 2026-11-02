#include "TestUtil.h"

#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "scene/Rig.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cstdio>

using namespace rigtest;
using namespace lila::scene;
using D3 = glm::dvec3;

namespace
{
void hoseCurves()
{
    curve::registerTypes();
    if (!ops::find("curve"))
        curve::registerCommands();
}

std::string hoseJoint(const char* name, const D3& at)
{
    return run("joint", {{"position", v3(at.x, at.y, at.z)}, {"name", name}}).get<std::string>();
}

/** clav, shoulder, elbow (bent back in z) and wrist with a hand under it, along x. */
void hoseArm()
{
    run("newScene");
    run("select", {{"args", json::array()}});
    hoseJoint("clav", D3(-5, 0, 0));
    hoseJoint("shoulder", D3(0, 0, 0));
    hoseJoint("elbow", D3(30, 0, -5));
    hoseJoint("wrist", D3(60, 0, 0));
    hoseJoint("hand", D3(70, 0, 0));
}

std::vector<std::string> hoseChainNames(const std::string& base, int n)
{
    std::vector<std::string> out;
    for (int i = 1; i <= n; ++i)
    {
        char s[8];
        std::snprintf(s, sizeof(s), "_%02d", i);
        out.push_back(base + s);
    }
    return out;
}

double hoseSegDist(const D3& p, const D3& a, const D3& b)
{
    const D3 ab = b - a;
    const double f = std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0, 1.0);
    return glm::length(p - (a + ab * f));
}

double hoseCurveLength(const std::string& c)
{
    graph().evaluate();
    return curve::length(curve::splinesOf(node(c).value(curve::CurveShape::kWorldSpace)).front());
}

std::string hoseText(const std::string& n, const char* attr) { return node(n).as<std::string>(node(n).type().attrIndex(attr)); }
} // namespace

TEST_CASE("rubber hose limb: hose joints follow the jointed limb or a smooth hose that bows into an arc keeping its length")
{
    hoseCurves();
    hoseArm();
    const std::string hose = run("hoseLimb", {{"args", {"shoulder", "wrist"}}, {"joints", 10}, {"hose", 0.0}}).get<std::string>();
    CHECK(node(hose).type().name == "hoseLimb");
    const auto js = hoseChainNames("shoulder_hose", 10);
    for (const std::string& j : js)
        REQUIRE(graph().find(j));
    CHECK(graph().parent(node(js[0]).id()) == node("clav").id());
    for (const char* j : {"shoulder", "elbow", "wrist"})
        CHECK_FALSE(node(j).as<bool>(Joint::kDeform));
    CHECK(node("hand").as<bool>(Joint::kDeform));
    for (const std::string& j : js)
        CHECK(node(j).as<bool>(Joint::kDeform));

    // Jointed: the hose joints lie on the upper and lower arm.
    const D3 s = pos("shoulder"), e = pos("elbow"), w = pos("wrist");
    checkNear(pos(js.front()), s, 1e-6);
    checkNear(pos(js.back()), w, 1e-4);
    for (const std::string& j : js)
        CHECK(std::min(hoseSegDist(pos(j), s, e), hoseSegDist(pos(j), e, w)) < 1e-3);

    // Folding the elbow: the hose keeps the arm's length, bowing toward the elbow, and still ends at the wrist.
    run("setAttr", {{"args", {hose + ".hose", 1.0}}});
    const std::string curveName = hoseText(hoseText(hose, "chain"), "curve");
    const double length = glm::length(e - s) + glm::length(w - e);
    CHECK(hoseCurveLength(curveName) == doctest::Approx(length).epsilon(1e-3));
    run("setAttr", {{"args", {"elbow.rotate", v3(0, 140, 0)}}});
    const D3 w2 = pos("wrist"), e2 = pos("elbow");
    CHECK(glm::length(w2 - s) < 0.6 * length);
    CHECK(hoseCurveLength(curveName) == doctest::Approx(length).epsilon(1e-3));
    checkNear(pos(js.back()), w2, 1e-3);
    const D3 chord = glm::normalize(w2 - s);
    auto across = [&](const D3& p) { const D3 v = p - s; return v - chord * glm::dot(v, chord); };
    const D3 bow = across(pos(js[5]));
    CHECK(glm::length(bow) > 10.0);
    CHECK(glm::dot(bow, across(e2)) > 0.0);

    // Squash: the arc keeps its length, so the joints keep their thickness; stretching the arm past its length thins them.
    checkNear(node(js[4]).as<D3>(Transform::kScale), D3(1.0), 1e-2);
    run("setAttr", {{"args", {"elbow.rotate", v3(0, 0, 0)}}});
    run("setAttr", {{"args", {"wrist.translate", v3(45, 0, 5)}}});
    graph().evaluate();
    const D3 sc = node(js[4]).as<D3>(Transform::kScale);
    CHECK(sc.y < 0.95);

    // The bend control pushes the middle of the hose.
    run("setAttr", {{"args", {"wrist.translate", v3(30, 0, 5)}}});
    const D3 mid = pos(js[5]);
    run("move", {{"args", {"shoulder_hoseBend"}}, {"by", v3(0, 8, 0)}});
    CHECK(pos(js[5]).y - mid.y > 5.0);
}

TEST_CASE("rubber hose limb: undo removes the hose and gives the limb back its joints")
{
    hoseCurves();
    hoseArm();
    const D3 before = pos("wrist");
    run("hoseLimb", {{"args", {"shoulder", "wrist"}}, {"joints", 8}});
    REQUIRE(graph().find("shoulder_hose_08"));
    run("undo");
    CHECK_FALSE(graph().find("shoulder_hose_08"));
    CHECK_FALSE(graph().find("shoulder_hose"));
    CHECK(node("elbow").as<bool>(Joint::kDeform));
    checkNear(pos("wrist"), before);
}

TEST_CASE("rubber hose fingers: curl bends each finger along an arc, the hand adds curl, spread and fist")
{
    hoseCurves();
    run("newScene");
    run("select", {{"args", json::array()}});
    hoseJoint("palm", D3(0, 0, 0));
    for (const char* f : {"index", "middle"})
    {
        const double z = std::string(f) == "index" ? 2.0 : -2.0;
        run("select", {{"args", {"palm"}}});
        for (int i = 0; i < 4; ++i)
            hoseJoint((std::string(f) + std::to_string(i)).c_str(), D3(10 + 3 * i, 0, z));
    }
    const json made = run("hoseFingers", {{"args", {"index0", "middle0"}}});
    REQUIRE(made.size() == 3);
    const std::string hand = made[0], index = made[1], middle = made[2];
    CHECK(node(hand).type().name == "hoseHand");
    checkNear(pos("index3"), D3(19, 0, 2), 1e-6);

    // A third of 270 degrees: a quarter circle of the finger's 9 cm, curling about z toward +y.
    run("setAttr", {{"args", {index + ".curl", 1.0 / 3.0}}});
    const double radius = 9.0 / (glm::half_pi<double>());
    const D3 centre(10, radius, 2);
    for (int i = 0; i < 4; ++i)
    {
        const D3 p = pos("index" + std::to_string(i));
        CHECK(glm::length(p - centre) == doctest::Approx(radius).epsilon(1e-3));
        if (i > 0)
            CHECK(glm::length(p - pos("index" + std::to_string(i - 1))) == doctest::Approx(3.0).epsilon(1e-6));
    }
    CHECK(pos("index3").y > 4.0);
    checkNear(pos("middle3"), D3(19, 0, -2), 1e-6);

    // The hand's curl does the same to every finger.
    std::vector<D3> curled;
    for (int i = 0; i < 4; ++i)
        curled.push_back(pos("index" + std::to_string(i)));
    run("setAttr", {{"args", {index + ".curl", 0.0}}});
    run("setAttr", {{"args", {hand + ".curl", 1.0 / 3.0}}});
    for (int i = 0; i < 4; ++i)
    {
        checkNear(pos("index" + std::to_string(i)), curled[size_t(i)], 1e-6);
        const D3 m = pos("middle" + std::to_string(i));
        checkNear(m, curled[size_t(i)] - D3(0, 0, 4), 1e-6);
    }

    // Fist closes every finger to its fist curl; spread fans them apart in opposite directions.
    run("setAttr", {{"args", {hand + ".curl", 0.0}}});
    run("setAttr", {{"args", {hand + ".fist", 1.0}}});
    const double fistRadius = 9.0 / (1.5 * glm::pi<double>());
    CHECK(glm::length(pos("index2") - D3(10, fistRadius, 2)) == doctest::Approx(fistRadius).epsilon(1e-3));
    run("setAttr", {{"args", {hand + ".fist", 0.0}}});
    run("setAttr", {{"args", {hand + ".spread", 1.0}}});
    const double zi = pos("index3").z - 2.0, zm = pos("middle3").z + 2.0;
    CHECK(std::abs(zi) > 1.0);
    CHECK(zi * zm < 0.0);
}

TEST_CASE("rubber hose limb: a stretched, squashed and bowed hose bakes to a skeleton that passes the Unreal check")
{
    hoseCurves();
    hoseArm();
    const std::string hose = run("hoseLimb", {{"args", {"shoulder", "wrist"}}, {"joints", 12}}).get<std::string>();
    run("setAttr", {{"args", {"wrist.translate", v3(45, 0, 5)}}});
    run("setAttr", {{"args", {"shoulder.rotate", v3(0, 0, 30)}}});
    run("move", {{"args", {"shoulder_hoseBend"}}, {"by", v3(0, 6, 3)}});
    graph().evaluate();
    CHECK(node("shoulder_hose_06").as<D3>(Transform::kScale).y < 0.95);
    const json baked = run("bakeToDeformSkeleton", {{"args", {"clav"}}, {"start", 0.0}, {"end", 0.0}, {"fps", 30.0}});
    REQUIRE(baked.size() == 1);
    for (const char* j : {"shoulder_hose_01", "shoulder_hose_06", "shoulder_hose_12", "hand"})
    {
        const glm::dmat4 a = world(j), b = world(std::string(j) + "_bake");
        for (int k = 0; k < 4; ++k)
            checkNear(D3(a[k]), D3(b[k]), 1e-6);
    }
    const json check = run("unrealCheck", {{"args", {baked[0]}}, {"start", 0.0}, {"end", 0.0}});
    CHECK(check["joints"].get<int>() > 12);
    CHECK(check["mismatches"].empty());
    // Without the scale-safe layout the squash would shear in Unreal, which the check catches.
    const json plain = run("bakeToDeformSkeleton", {{"args", {"clav"}}, {"start", 0.0}, {"end", 0.0}, {"suffix", "_plain"}, {"scaleSafe", false}});
    CHECK_FALSE(run("unrealCheck", {{"args", {plain[0]}}, {"start", 0.0}, {"end", 0.0}, {"suffix", "_plain"}})["mismatches"].empty());
}
