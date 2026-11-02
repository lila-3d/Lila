#include "TestUtil.h"

#include "curve/CurveNodes.h"
#include "scene/Rig.h"
#include "scene/Skin.h"

#include <algorithm>
#include <cstdio>
#include <tuple>

using namespace rigtest;
using namespace lila::scene;
using D3 = glm::dvec3;

namespace
{
/** An arm up Y (shoulder, elbow bowed toward +x, wrist, hand) under a clavicle, with a skinned tube along it. */
std::string skinnedArm()
{
    curve::registerTypes();
    if (!ops::find("curve"))
        curve::registerCommands();
    run("newScene");
    const std::string tube = run("polyCylinder", {{"name", "Arm"}, {"radius", 4}, {"height", 60}, {"subdivisionsHeight", 30}, {"caps", false}})[0];
    run("move", {{"args", {tube}}, {"by", v3(0, 30, 0)}});
    run("select", {{"args", json::array()}});
    for (auto [name, y, x] : {std::tuple{"clav", -5.0, 0.0}, std::tuple{"shoulder", 0.0, 0.0}, std::tuple{"elbow", 30.0, 3.0},
                              std::tuple{"wrist", 60.0, 0.0}, std::tuple{"hand", 70.0, 0.0}})
        run("joint", {{"position", v3(x, y, 0)}, {"name", name}});
    run("skinCluster", {{"args", {"clav", tube}}});
    return tube;
}

std::vector<D3> deformed(const std::string& mesh)
{
    const glm::dmat4 w = world(mesh);
    std::vector<D3> out;
    for (const auto& p : outMesh(mesh)->positions())
        out.emplace_back(w * glm::dvec4(D3(p), 1.0));
    return out;
}

std::vector<std::string> hoseJoints(int n)
{
    std::vector<std::string> out;
    for (int i = 1; i <= n; ++i)
    {
        char s[8];
        std::snprintf(s, sizeof(s), "_%02d", i);
        out.push_back(std::string("shoulder_hose") + s);
    }
    return out;
}

double polylineDistance(const D3& p, const std::vector<D3>& line)
{
    double best = 1e300;
    for (size_t k = 0; k + 1 < line.size(); ++k)
    {
        const D3 ab = line[k + 1] - line[k];
        const double f = std::clamp(glm::dot(p - line[k], ab) / glm::dot(ab, ab), 0.0, 1.0);
        best = std::min(best, glm::length(p - (line[k] + ab * f)));
    }
    return best;
}

/** The centre of the tube's ring at rest height y (world), deformed. */
D3 ringCentre(const std::string& mesh, const std::vector<D3>& rest, double y)
{
    const std::vector<D3> now = deformed(mesh);
    D3 sum(0.0);
    int n = 0;
    for (size_t i = 0; i < rest.size(); ++i)
        if (std::abs(rest[i].y - y) < 0.5)
            sum += now[i], ++n;
    REQUIRE(n > 0);
    return sum / double(n);
}

double weightOn(const std::string& mesh, const char* joint)
{
    const NodeId sc = skin::find(graph(), node(mesh).id());
    const int64_t id = skin::indexOf(skin::influences(graph(), sc), joint);
    if (id < 0)
        return 0.0;
    const WeightTable t = skin::table(graph(), sc);
    double sum = 0.0;
    for (size_t p = 0; p < t.points.size(); ++p)
        sum += t.get(p, uint32_t(id));
    return sum;
}
} // namespace

TEST_CASE("hose reskin: the limb's weights move onto the hose joints, the skin unchanged at rest and following the bent hose")
{
    const std::string mesh = skinnedArm();
    const std::vector<D3> rest = deformed(mesh);
    const double onLimb = weightOn(mesh, "shoulder") + weightOn(mesh, "elbow") + weightOn(mesh, "wrist");
    REQUIRE(onLimb > 1.0);

    const std::string hose = run("hoseLimb", {{"args", {"shoulder", "wrist"}}, {"joints", 10}}).get<std::string>();
    for (const char* j : {"shoulder", "elbow", "wrist"})
        CHECK(weightOn(mesh, j) == doctest::Approx(0.0));
    double onHose = 0.0;
    for (const std::string& j : hoseJoints(10))
        onHose += weightOn(mesh, j.c_str());
    CHECK(onHose == doctest::Approx(onLimb).epsilon(1e-4));

    const std::vector<D3> after = deformed(mesh);
    REQUIRE(after.size() == rest.size());
    double drift = 0.0;
    for (size_t i = 0; i < rest.size(); ++i)
        drift = std::max(drift, glm::length(after[i] - rest[i]));
    CHECK(drift < 1e-4);

    // Folding the elbow: the forearm's ring rides the hose, not the jointed forearm.
    run("setAttr", {{"args", {"elbow.rotate", v3(0, 0, 100)}}});
    std::vector<D3> line;
    for (const std::string& j : hoseJoints(10))
        line.push_back(pos(j));
    const D3 jointed = glm::mix(pos("elbow"), pos("wrist"), 0.5);
    const D3 ring = ringCentre(mesh, rest, 44.0);
    MESSAGE("forearm ring: " << polylineDistance(ring, line) << " cm from the hose, " << glm::length(ring - jointed)
                             << " cm from the jointed forearm's middle");
    const double onHoseLine = polylineDistance(ring, line);
    CHECK(onHoseLine < 2.0);
    CHECK(glm::length(ring - D3(0, 44, 0)) > 5.0);

    // Without the reskin the tube keeps folding with the jointed arm, away from the hose.
    skinnedArm();
    run("hoseLimb", {{"args", {"shoulder", "wrist"}}, {"joints", 10}, {"reskin", false}});
    CHECK(weightOn(mesh, "elbow") > 1.0);
    run("setAttr", {{"args", {"elbow.rotate", v3(0, 0, 100)}}});
    std::vector<D3> line2;
    for (const std::string& j : hoseJoints(10))
        line2.push_back(pos(j));
    const double offHoseLine = polylineDistance(ringCentre(mesh, rest, 44.0), line2);
    MESSAGE("without reskin: " << offHoseLine << " cm from the hose");
    CHECK(offHoseLine > 2.0 * onHoseLine);
}

TEST_CASE("hose reskin: the command does for an existing hose what hoseLimb does, and undoes")
{
    std::string mesh = skinnedArm();
    run("hoseLimb", {{"args", {"shoulder", "wrist"}}, {"joints", 8}});
    run("setAttr", {{"args", {"elbow.rotate", v3(0, 0, 60)}}});
    const std::vector<D3> made = deformed(mesh);

    mesh = skinnedArm();
    const std::vector<D3> rest = deformed(mesh);
    const std::string hose = run("hoseLimb", {{"args", {"shoulder", "wrist"}}, {"joints", 8}, {"reskin", false}}).get<std::string>();
    CHECK(run("hoseReskin", {{"args", {hose}}}).get<int>() == 1);
    CHECK(weightOn(mesh, "elbow") == doctest::Approx(0.0));
    run("setAttr", {{"args", {"elbow.rotate", v3(0, 0, 60)}}});
    const std::vector<D3> later = deformed(mesh);
    REQUIRE(later.size() == made.size());
    for (size_t i = 0; i < made.size(); ++i)
        CHECK(glm::length(later[i] - made[i]) < 1e-4);

    run("undo");
    run("undo");
    CHECK(weightOn(mesh, "elbow") > 1.0);
    CHECK(weightOn(mesh, "shoulder_hose_04") == doctest::Approx(0.0));
    const std::vector<D3> back = deformed(mesh);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(glm::length(back[i] - rest[i]) < 1e-4);
    CHECK_THROWS_AS(run("hoseReskin", {{"args", {"elbow"}}}), CommandError);
}
