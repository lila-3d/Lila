#include "TestUtil.h"

#include "scene/AutoPose.h"
#include "scene/FullBodyIk.h"
#include "scene/Rig.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace rigtest;
using namespace lila::scene;

namespace
{
/** A stick biped in cm: hips at 100, three spine joints, a head, arms out to the sides, legs down. */
void biped()
{
    run("newScene");
    auto j = [](const char* name, double x, double y, double z, const char* parent) {
        run("joint", {{"position", v3(x, y, z)}, {"name", name}, {"parent", parent}, {"skipSelect", true}});
    };
    j("hips", 0, 100, 0, "");
    j("spine_01", 0, 110, 0, "hips");
    j("spine_02", 0, 122, 0, "spine_01");
    j("spine_03", 0, 134, 0, "spine_02");
    j("head", 0, 160, 0, "spine_03");
    for (const char* side : {"l", "r"})
    {
        const double s = side[0] == 'l' ? 1.0 : -1.0;
        auto n = [&](const char* base) { return std::string(base) + "_" + side; };
        j(n("upperarm").c_str(), 18 * s, 140, 0, "spine_03");
        j(n("lowerarm").c_str(), 46 * s, 140, -2, n("upperarm").c_str());
        j(n("hand").c_str(), 72 * s, 140, 0, n("lowerarm").c_str());
        j(n("thigh").c_str(), 10 * s, 95, 0, "hips");
        j(n("calf").c_str(), 10 * s, 52, 2, n("thigh").c_str());
        j(n("foot").c_str(), 10 * s, 8, 0, n("calf").c_str());
    }
}

double length(const char* a, const char* b) { return glm::length(pos(b) - pos(a)); }

json pinAt(const char* joint, const glm::dvec3& p) { return {{"joint", joint}, {"position", v3(p.x, p.y, p.z)}}; }

/** A straight chain of four 10 cm bones along X, as a bare problem. */
fbik::Problem chain()
{
    fbik::Problem p;
    for (int i = 0; i < 4; ++i)
    {
        fbik::Bone b;
        b.parent = i - 1;
        b.before = glm::translate(glm::dmat4(1.0), glm::dvec3(i == 0 ? 0.0 : 10.0, 0.0, 0.0));
        p.bones.push_back(b);
    }
    p.lengthScale = 10.0;
    return p;
}
} // namespace

TEST_CASE("the full-body solver reaches a pin and keeps bone lengths")
{
    fbik::Problem p = chain();
    p.effectors.push_back({.bone = 3, .position = glm::dvec3(15.0, 15.0, 5.0)});
    const fbik::Solution s = fbik::solve(p);
    CHECK(s.error < 1e-3);
    const std::vector<glm::dmat4> w = fbik::forward(p, s.rotation, s.offset);
    for (int i = 0; i < 3; ++i)
        CHECK(glm::length(glm::dvec3(w[i + 1][3]) - glm::dvec3(w[i][3])) == doctest::Approx(10.0));
    checkNear(glm::dvec3(w[0][3]), glm::dvec3(0.0));
}

TEST_CASE("the full-body solver meets a rotation pin")
{
    fbik::Problem p = chain();
    const glm::dquat want = glm::angleAxis(glm::radians(70.0), glm::normalize(glm::dvec3(0.2, 1.0, 0.3)));
    p.effectors.push_back({.bone = 3, .position = glm::dvec3(18.0, 12.0, 0.0), .rotation = want});
    const fbik::Solution s = fbik::solve(p);
    CHECK(s.error < 1e-3);
    const glm::dmat3 got(fbik::forward(p, s.rotation, s.offset)[3]);
    const glm::dmat3 diff = glm::transpose(glm::dmat3(glm::mat3_cast(want))) * got;
    CHECK(glm::angle(glm::quat_cast(diff)) < 1e-3);
}

TEST_CASE("an unreachable pin stretches the chain toward it without blowing up")
{
    fbik::Problem p = chain();
    p.effectors.push_back({.bone = 3, .position = glm::dvec3(0.0, 100.0, 0.0)});
    const fbik::Solution s = fbik::solve(p);
    const glm::dvec3 end(fbik::forward(p, s.rotation, s.offset)[3][3]);
    CHECK(s.error == doctest::Approx(70.0).epsilon(1e-3));
    checkNear(glm::normalize(end), glm::dvec3(0, 1, 0), 1e-3);
}

TEST_CASE("fullBodyIk moves a hand with the feet held and leaves the rest of the pose")
{
    biped();
    const glm::dvec3 footL = pos("foot_l"), footR = pos("foot_r"), handR = pos("hand_r");
    const double upper = length("upperarm_l", "lowerarm_l"), thigh = length("thigh_l", "calf_l");
    const glm::dvec3 target = pos("hand_l") + glm::dvec3(-20.0, -40.0, 25.0);
    const json r = run("fullBodyIk", {{"pins", {pinAt("hand_l", target)}}, {"hold", {"foot_l", "foot_r"}}});

    CHECK(r["error"].get<double>() < 0.01);
    checkNear(pos("hand_l"), target, 1e-4);
    checkNear(pos("foot_l"), footL, 1e-4);
    checkNear(pos("foot_r"), footR, 1e-4);
    CHECK(length("upperarm_l", "lowerarm_l") == doctest::Approx(upper));
    CHECK(length("thigh_l", "calf_l") == doctest::Approx(thigh));
    // Writing FK: the solve lands in the joints' own rotate.
    CHECK(glm::length(node("upperarm_l").as<glm::dvec3>(Transform::kRotate)) > 1.0);
    // The other arm rides along with the spine, but keeps its own pose.
    checkNear(node("lowerarm_r").as<glm::dvec3>(Transform::kRotate), glm::dvec3(0.0), 1e-9);
    CHECK(glm::length(pos("hand_r") - handR) < 25.0);

    run("undo");
    checkNear(pos("hand_l"), glm::dvec3(72, 140, 0), 1e-9);
}

TEST_CASE("fullBodyIk moves the hips when the legs have to bend to reach")
{
    biped();
    const glm::dvec3 footL = pos("foot_l"), footR = pos("foot_r");
    // Down by the knee: only bending at the hips and knees gets the hand there.
    const glm::dvec3 target(40.0, 60.0, 30.0);
    const json r = run("fullBodyIk", {{"pins", {pinAt("hand_l", target)}}, {"hold", {"foot_l", "foot_r"}}});
    CHECK(r["error"].get<double>() < 0.01);
    checkNear(pos("foot_l"), footL, 1e-4);
    checkNear(pos("foot_r"), footR, 1e-4);
    CHECK(pos("hips").y < 99.0);

    run("undo");
    biped();
    run("fullBodyIk", {{"pins", {pinAt("hand_l", target)}}, {"hold", {"foot_l", "foot_r"}}, {"moveRoot", false}});
    checkNear(pos("hips"), glm::dvec3(0, 100, 0), 1e-9);
}

TEST_CASE("a stiff joint turns less and the pin is still reached")
{
    auto bendOf = [](const char* j) { return glm::length(node(j).as<glm::dvec3>(Transform::kRotate)); };
    const glm::dvec3 target(15.0, 152.0, 10.0);
    biped();
    run("fullBodyIk", {{"pins", {pinAt("head", target)}}, {"hold", {"foot_l", "foot_r"}}, {"moveRoot", false}});
    const double free1 = bendOf("spine_01");
    CHECK(free1 > 1.0);

    biped();
    run("fullBodyIk", {{"pins", {pinAt("head", target)}}, {"hold", {"foot_l", "foot_r"}}, {"moveRoot", false},
                       {"stiffness", {{"spine_01", 0.95}}}});
    CHECK(bendOf("spine_01") < free1 * 0.5);
    checkNear(pos("head"), target, 1e-4);
}

TEST_CASE("fullBodyIk keys what it posed when asked")
{
    biped();
    run("fullBodyIk", {{"pins", {pinAt("hand_l", glm::dvec3(60, 120, 30))}}, {"hold", {"foot_l", "foot_r"}}, {"key", true}});
    CHECK(run("keyframe", {{"args", {"upperarm_l.rotate"}}, {"query", true}, {"keyCount", true}}).get<int>() > 0);
}

TEST_CASE("fullBodyIk rejects pins outside the skeleton")
{
    biped();
    run("joint", {{"position", v3(0, 0, 50)}, {"name", "other"}, {"parent", ""}});
    CHECK_THROWS(run("fullBodyIk", {{"root", "hips"}, {"pins", {pinAt("other", glm::dvec3(0.0))}}}));
    CHECK_THROWS(run("fullBodyIk", json::object()));
}

TEST_CASE("a full-body solve of a biped is interactive")
{
    biped();
    std::vector<NodeId> index;
    fbik::PoseOptions opts;
    opts.pins = {{.joint = node("hand_l").id(), .position = glm::dvec3(40, 60, 30)},
                 {.joint = node("foot_l").id(), .position = pos("foot_l")},
                 {.joint = node("foot_r").id(), .position = pos("foot_r")}};
    const fbik::Problem p = fbik::problemFor(graph(), node("hips").id(), opts, index);
    const auto start = std::chrono::steady_clock::now();
    const fbik::Solution s = fbik::solve(p);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("biped solve: " << ms << " ms, " << s.iterations << " iterations, error " << s.error);
    CHECK(s.error < 0.01);
    CHECK(ms < 20.0);
}

TEST_CASE("soft effectors shape the pose and the pins are still met exactly")
{
    auto solved = [](bool soft) {
        fbik::Problem p = chain();
        p.effectors.push_back({.bone = 3, .position = glm::dvec3(20.0, 15.0, 0.0)});
        if (soft)
            p.effectors.push_back({.bone = 1, .position = glm::dvec3(10.0, 0.0, 10.0), .weight = 0.5, .soft = true});
        const fbik::Solution s = fbik::solve(p);
        CHECK(s.error < 1e-3);
        return glm::dvec3(fbik::forward(p, s.rotation, s.offset)[2][3]);
    };
    const glm::dvec3 plain = solved(false), shaped = solved(true);
    // The soft pull bends the middle of the chain out toward +Z.
    CHECK(shaped.z > plain.z + 1.0);
}

namespace
{
/** A one-layer model that moves every point by bias (in leg lengths) whatever the input. */
std::filesystem::path writeModel(float bias)
{
    const auto path = std::filesystem::temp_directory_path() / "lila_rig_tests" / "test.lilanet";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    out.write("LAPN", 4);
    u32(1);
    u32(1);
    u32(autopose::kPointCount);
    const uint32_t rows = autopose::kPointCount * 3, cols = autopose::kPointCount * 7;
    u32(rows);
    u32(cols);
    u32(0);
    const std::vector<float> w(size_t(rows) * cols, 0.0f);
    std::vector<float> b(rows, 0.0f);
    for (uint32_t r = 1; r < rows; r += 3)
        b[r] = bias;
    out.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size() * 4));
    out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size() * 4));
    return path;
}
} // namespace

TEST_CASE("an auto-pose model loads and predicts in the body frame")
{
    std::string error;
    const auto model = autopose::Model::load(writeModel(0.5f), &error);
    REQUIRE(model);
    autopose::Points current;
    for (int i = 0; i < autopose::kPointCount; ++i)
        current[size_t(i)] = glm::dvec3(i, 100.0 + i, 0.0);
    // Legs of 40 + 40 cm, so half a leg length is 40 cm.
    current[8] = {10, 90, 0}, current[9] = {10, 50, 0}, current[10] = {10, 10, 0};
    current[15] = {-10, 90, 0}, current[16] = {-10, 50, 0}, current[17] = {-10, 10, 0};
    const autopose::Points out = model->predict(current, {});
    for (int i = 0; i < autopose::kPointCount; ++i)
        checkNear(out[size_t(i)], current[size_t(i)] + glm::dvec3(0.0, 40.0, 0.0), 1e-4);

    std::ofstream(std::filesystem::temp_directory_path() / "lila_rig_tests" / "bad.lilanet") << "nope";
    CHECK_FALSE(autopose::Model::load(std::filesystem::temp_directory_path() / "lila_rig_tests" / "bad.lilanet", &error));
    CHECK(error.find("not an auto-pose model") != std::string::npos);
}

TEST_CASE("fullBodyIk with autoPose on a skeleton it cannot read poses as without it")
{
    biped();
    const glm::dvec3 target(60, 120, 30);
    const json r = run("fullBodyIk", {{"pins", {pinAt("hand_l", target)}}, {"hold", {"foot_l", "foot_r"}}, {"autoPose", true}});
    CHECK(r["error"].get<double>() < 0.01);
    checkNear(pos("hand_l"), target, 1e-4);
}
