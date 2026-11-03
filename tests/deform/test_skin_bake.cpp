#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "deform/Deform.h"
#include "deform/Ssdr.h"
#include "fbx/Fbx.h"
#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Simulation.h"
#include "scene/Skin.h"
#include "scene/SkinBake.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
using namespace deformtest;

namespace
{
double volume(const std::string& mesh)
{
    MeshPtr m = outMesh(mesh);
    std::vector<uint32_t> tris = deform::trianglePoints(*m);
    double v = 0.0;
    for (size_t t = 0; t < tris.size(); t += 3)
        v += glm::dot(m->position(tris[t]), glm::cross(m->position(tris[t + 1]), m->position(tris[t + 2]))) / 6.0;
    return std::abs(v);
}

/** A closed tube skinned to a three joint chain, bone_b at the middle (an elbow). */
std::string skinnedArm(const std::string& method = "linear")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Arm"}, {"radius", 5}, {"height", 100}, {"subdivisionsHeight", 12}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}, {"skinningMethod", method}});
    return mesh;
}

void bend(double degrees) { run("setAttr", {{"args", {"bone_b.rotateZ", degrees}}}); }

NodeId skinOf(const std::string& mesh) { return skin::find(graph(), graph().find(mesh)->id()); }

int64_t methodOf(const std::string& mesh) { return graph().node(skinOf(mesh))->as<int64_t>(SkinClusterNode::kSkinningMethod); }
} // namespace

TEST_CASE("ssdrFit recovers weights that linear skinning can reproduce")
{
    deform::SsdrProblem p;
    const int columns = 21;
    std::vector<double> truth;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < columns; ++c)
        {
            const double x = 100.0 * c / (columns - 1);
            p.rest.push_back({x, r * 5.0, 0.0});
            const double t = x / 100.0;
            truth.push_back(t * t * (3.0 - 2.0 * t));
        }
    for (int c = 0; c + 1 < columns; ++c)
        p.polygons.push_back({c, c + 1, columns + c + 1, columns + c});
    p.bones = 2;
    p.frames = 8;
    const size_t n = p.rest.size();
    for (int f = 0; f < p.frames; ++f)
    {
        const glm::dmat4 b1 = glm::translate(glm::dmat4(1.0), glm::dvec3(50, 0, 0)) * glm::rotate(glm::dmat4(1.0), glm::radians(12.0 * f), glm::dvec3(0, 0, 1)) *
                              glm::translate(glm::dmat4(1.0), glm::dvec3(-50, 0, 0));
        p.transforms.push_back(glm::dmat4(1.0));
        p.transforms.push_back(b1);
        for (size_t i = 0; i < n; ++i)
            p.targets.push_back(glm::dvec3((glm::dmat4(1.0) * (1.0 - truth[i]) + b1 * truth[i]) * glm::dvec4(p.rest[i], 1.0)));
    }
    for (size_t i = 0; i < n; ++i)
    {
        p.influences.push_back(p.rest[i].x < 50.0 ? 0 : 1);
        p.weights.push_back(1.0f);
        p.offsets.push_back(uint32_t(p.influences.size()));
    }
    p.maxInfluences = 2;
    p.iterations = 4;
    p.smoothness = 1e-8;
    const deform::SsdrResult r = deform::ssdrFit(p);
    MESSAGE("rmse " << r.rmse << ", worst " << r.maxError);
    CHECK(r.rmse < 0.05);
    CHECK(r.maxError < 0.2);
    // The same input gives the same weights.
    const deform::SsdrResult again = deform::ssdrFit(p);
    CHECK(again.weights == r.weights);
}

TEST_CASE("centres of rotation and direct delta mush: exact at rest, rigid under rigid motion, finite when bent")
{
    for (const char* method : {"centresOfRotation", "directDeltaMush"})
    {
        CAPTURE(method);
        std::string mesh = skinnedArm("linear");
        const std::vector<glm::dvec3> rest = points(mesh);
        bend(70.0);
        const std::vector<glm::dvec3> linear = points(mesh);
        const double linearVolume = volume(mesh);
        bend(0.0);

        run("skinningMethod", {{"args", {mesh}}, {"method", method}});
        CHECK(maxDistance(points(mesh), rest) < 1e-6);

        // The whole chain turning together is a rigid motion of every point.
        run("setAttr", {{"args", {"bone_a.rotateZ", 30.0}}});
        const glm::dmat4 r = glm::translate(glm::dmat4(1.0), glm::dvec3(0, -50, 0)) * glm::rotate(glm::dmat4(1.0), glm::radians(30.0), glm::dvec3(0, 0, 1)) *
                             glm::translate(glm::dmat4(1.0), glm::dvec3(0, 50, 0));
        std::vector<glm::dvec3> rigid;
        for (const glm::dvec3& p : rest)
            rigid.push_back(glm::dvec3(r * glm::dvec4(p, 1.0)));
        CHECK(maxDistance(points(mesh), rigid) < 1e-6);
        run("setAttr", {{"args", {"bone_a.rotateZ", 0.0}}});

        bend(70.0);
        const std::vector<glm::dvec3> bent = points(mesh);
        for (const glm::dvec3& p : bent)
            REQUIRE((std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)));
        CHECK(maxDistance(bent, linear) > 0.1);
        if (std::string(method) == "centresOfRotation")
        {
            // Volume is what the centres of rotation keep where linear blending collapses the elbow.
            const double restVolume = 3.14159265358979 * 25.0 * 100.0;
            CHECK(std::abs(volume(mesh) - restVolume) < std::abs(linearVolume - restVolume));
        }
        CHECK(methodOf(mesh) == (std::string(method) == "directDeltaMush" ? SkinClusterNode::kDirectDeltaMush : SkinClusterNode::kCentresOfRotation));
    }
}

TEST_CASE("bakeSkinWeights: SSDR fits a delta mush rig, goes linear and turns the delta mush off")
{
    std::string mesh = skinnedArm();
    bend(45.0);
    const std::vector<glm::dvec3> linear = points(mesh);
    bend(0.0);
    run("deltaMush", {{"args", {mesh}}, {"iterations", 20}});
    bend(45.0);
    const std::vector<glm::dvec3> mushed = points(mesh);
    bend(0.0);

    json stored = run("bakeSkinWeights", {{"args", {mesh}}, {"keep", true}});
    CHECK(stored["frames"].get<int>() > 10);
    CHECK(graph().node(skinOf(mesh))->as<DataPtr>(SkinClusterNode::kExportWeights) != nullptr);
    CHECK(run("deformers", {{"args", {mesh}}}).size() == 1);

    json r = run("bakeSkinWeights", {{"args", {mesh}}});
    MESSAGE("SSDR rmse " << r["rmse"].get<double>() << " cm, worst " << r["maxError"].get<double>() << ", linear skinning rmse "
                         << r["linearRmse"].get<double>());
    // Three joints can't bend like a delta mush, so weights alone only get closer; helpers (below) close the gap.
    CHECK(r["rmse"].get<double>() < r["linearRmse"].get<double>());
    CHECK(r["disabled"].size() == 1);
    CHECK(methodOf(mesh) == SkinClusterNode::kLinear);

    // Linear skinning with the fitted weights now stands in for the delta mush at a pose of the range of motion. The fit
    // minimises the error over every pose, so one pose is compared by RMS, not by its worst point.
    bend(45.0);
    auto rms = [](const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b) {
        double sum = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            sum += glm::dot(a[i] - b[i], a[i] - b[i]);
        return std::sqrt(sum / double(a.size()));
    };
    const double fitted = rms(points(mesh), mushed), own = rms(linear, mushed);
    MESSAGE("RMS at the sampled pose: fitted " << fitted << ", the skin's own weights " << own);
    CHECK(fitted < own);
}

TEST_CASE("bakeCorrectiveMorphs reproduces the stack at every sampled pose")
{
    std::string mesh = skinnedArm("directDeltaMush");
    Graph& g = graph();
    const NodeId skin = skinOf(mesh);
    skinbake::RomOptions rom;
    rom.angle = 40.0;
    rom.axes = "xz";
    rom.randomPoses = 0;
    const std::vector<skinbake::Pose> poses = skinbake::romPoses(g, skinbake::poseableJoints(g, skin), rom);
    REQUIRE(poses.size() == 12);
    std::vector<std::vector<glm::dvec3>> before;
    auto apply = [&](const skinbake::Pose& p) {
        for (auto& [j, e] : p)
            g.setValue({j, Transform::kRotate, -1}, e);
    };
    std::vector<glm::dvec3> rest = points(mesh);
    std::map<NodeId, glm::dvec3> restRot;
    for (auto& p : poses)
        restRot[p[0].first] = g.node(p[0].first)->as<glm::dvec3>(Transform::kRotate);
    auto reset = [&] {
        for (auto& [j, e] : restRot)
            g.setValue({j, Transform::kRotate, -1}, e);
    };
    for (const auto& p : poses)
    {
        apply(p);
        before.push_back(points(mesh));
        reset();
    }

    json r = run("bakeCorrectiveMorphs", {{"args", {mesh}}, {"angle", 40.0}, {"axes", "xz"}});
    CHECK(r["targets"].size() > 0);
    CHECK(methodOf(mesh) == SkinClusterNode::kLinear);
    CHECK(maxDistance(points(mesh), rest) < 1e-5);
    for (size_t k = 0; k < poses.size(); ++k)
    {
        CAPTURE(k);
        apply(poses[k]);
        CHECK(maxDistance(points(mesh), before[k]) < 1e-4);
        reset();
    }
}

TEST_CASE("jiggle: lags, settles, and scrubbing matches playing")
{
    run("newScene");
    std::string mesh = run("polyCube", {{"name", "Belly"}, {"width", 10}, {"height", 10}, {"depth", 10}})[0];
    Graph& g = graph();
    const NodeId id = g.find(mesh)->id();
    const double fps = anim::playback().fps;
    anim::setKey(g, {id, Transform::kTranslate, 0}, 0.0, 0.0);
    anim::setKey(g, {id, Transform::kTranslate, 0}, 10.0 / fps, 50.0);
    std::string jig = run("jiggle", {{"args", {mesh}}, {"stiffness", 80.0}, {"damping", 3.0}})[0];

    std::vector<std::vector<glm::dvec3>> played;
    for (int f = 0; f <= 40; ++f)
    {
        g.setTime(f / fps);
        g.evaluate();
        played.push_back(points(mesh));
    }
    // Mid move the points trail the cube, so in object space they sit behind it.
    CHECK(played[8][0].x < -0.5);
    CHECK(maxDistance(played[0], played[40]) < 5.0);

    sim::clearCache(g);
    g.setTime(25 / fps);
    g.evaluate();
    CHECK(maxDistance(points(mesh), played[25]) == 0.0);
    g.setTime(13 / fps);
    g.evaluate();
    CHECK(maxDistance(points(mesh), played[13]) == 0.0);
    CHECK(sim::info(g, g.find(jig)->id()).error.empty());
}

TEST_CASE("rangeOfMotion keys one pose per frame into its own take")
{
    std::string mesh = skinnedArm();
    json r = run("rangeOfMotion", {{"args", {mesh}}, {"axes", "z"}, {"randomPoses", 3}});
    CHECK(r["poses"].get<int>() == 3 * 2 + 3);
    CHECK(r["end"].get<int>() == 9);
    Graph& g = graph();
    g.setTime(0.0);
    const std::vector<glm::dvec3> rest = points(mesh);
    g.setTime(1.0 / anim::playback().fps);
    CHECK(maxDistance(points(mesh), rest) > 1.0);
}

TEST_CASE("skeletal FBX export fits linear weights for a direct delta mush skin")
{
    std::string mesh = skinnedArm("directDeltaMush");
    Graph& g = graph();
    fbx::ExportOptions o;
    o.kind = fbx::ExportKind::Skeletal;
    o.animation = false;
    fbx::ExportScene s = fbx::gatherExport(g, {g.find(mesh)->id(), g.find("bone_a")->id()}, o);
    bool noted = false;
    for (const std::string& w : s.warnings)
        noted = noted || w.find("fitted linear weights") != std::string::npos;
    CHECK(noted);
    bool skinned = false;
    for (const fbx::ExportNode& e : s.nodes)
        skinned = skinned || (e.skin && !e.skin->clusters.empty());
    CHECK(skinned);
}

TEST_CASE("bakeSkinWeights over animation with helper joints captures what weights alone can't")
{
    std::string mesh = skinnedArm();
    run("deltaMush", {{"args", {mesh}}, {"iterations", 20}});
    Graph& g = graph();
    const double fps = anim::playback().fps;
    const NodeId b = g.find("bone_b")->id();
    anim::setKey(g, {b, Transform::kRotate, 2}, 0.0, 0.0);
    anim::setKey(g, {b, Transform::kRotate, 2}, 10.0 / fps, 80.0);
    anim::setKey(g, {b, Transform::kRotate, 2}, 20.0 / fps, -40.0);
    g.setTime(10.0 / fps);
    const std::vector<glm::dvec3> mushed = points(mesh);

    json r = run("bakeSkinWeights", {{"args", {mesh}}, {"source", "animation"}, {"start", 0}, {"end", 20}, {"helpers", 4}});
    MESSAGE("with 4 helpers: rmse " << r["rmse"].get<double>() << " cm, linear skinning " << r["linearRmse"].get<double>());
    CHECK(r["rmse"].get<double>() < r["linearRmse"].get<double>() * 0.25);
    REQUIRE(r["helpers"].size() == 4);
    CHECK(run("skinInfo", {{"args", {mesh}}})["influences"].size() == 7);
    // The helpers are keyed, so the baked rig plays the fitted motion back.
    g.setTime(10.0 / fps);
    CHECK(maxDistance(points(mesh), mushed) < r["maxError"].get<double>() * 3.0);
}
