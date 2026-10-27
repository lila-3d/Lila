#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "scene/Deformers.h"
#include "scene/Drivers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"

#include <glm/gtc/matrix_transform.hpp>
#include <ufbx.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

Graph& graph() { return Graph::current(); }

std::vector<glm::dvec3> points(const std::string& mesh)
{
    Graph& g = graph();
    g.evaluate();
    Node* n = g.find(mesh);
    REQUIRE(n);
    MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
    return {m->positions().begin(), m->positions().end()};
}

/** World space points of a mesh node. */
std::vector<glm::dvec3> worldPoints(const std::string& mesh)
{
    std::vector<glm::dvec3> p = points(mesh);
    glm::dmat4 w = graph().find(mesh)->as<glm::dmat4>(Transform::kWorldMatrix);
    for (glm::dvec3& x : p)
        x = glm::dvec3(w * glm::dvec4(x, 1.0));
    return p;
}

bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6) { return glm::length(a - b) <= eps; }

double maxDistance(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b)
{
    REQUIRE(a.size() == b.size());
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        d = std::max(d, glm::length(a[i] - b[i]));
    return d;
}

void editPoints(const std::string& mesh, const std::function<glm::dvec3(geo::Index, const glm::dvec3&)>& offset)
{
    Graph& g = graph();
    NodeId id = g.find(mesh)->id();
    PlugRef plug = modifiers::editPlug(g, id);
    auto m = std::make_shared<geo::Mesh>(*std::get<MeshPtr>(g.value(plug)));
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        m->setPosition(p, m->position(p) + offset(p, m->position(p)));
    g.setValue(plug, MeshPtr(m));
}

double plugValue(const std::string& plug) { return std::get<double>(graph().value(graph().plug(plug))); }

fs::path tempFile(const std::string& name)
{
    fs::path dir = fs::temp_directory_path() / "lila_deform_tests";
    fs::create_directories(dir);
    return dir / name;
}

/** A tube skinned to a two joint chain, bone_b at the middle (an elbow). */
std::string skinnedTube(int method = 0)
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Arm"}, {"radius", 5}, {"height", 100}, {"subdivisionsHeight", 10}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    std::string skin = run("skinCluster", {{"args", {"bone_a", mesh}}});
    run("setAttr", {{"args", {skin + ".skinningMethod", method}}});
    return mesh;
}
} // namespace

TEST_CASE("morph shape coefficients interpolate in-betweens linearly and extrapolate the ends")
{
    using morphs::shapeCoefficients;
    auto eq = [](const std::vector<double>& a, std::vector<double> b) {
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i)
            CHECK(a[i] == doctest::Approx(b[i]));
    };
    eq(shapeCoefficients({1.0}, 0.3), {0.3});
    eq(shapeCoefficients({1.0}, 1.4), {1.4});
    eq(shapeCoefficients({1.0}, -0.5), {-0.5});
    eq(shapeCoefficients({0.5, 1.0}, 0.0), {0.0, 0.0});
    eq(shapeCoefficients({0.5, 1.0}, 0.25), {0.5, 0.0});
    eq(shapeCoefficients({0.5, 1.0}, 0.5), {1.0, 0.0});
    eq(shapeCoefficients({0.5, 1.0}, 0.75), {0.5, 0.5});
    eq(shapeCoefficients({0.5, 1.0}, 1.0), {0.0, 1.0});
    eq(shapeCoefficients({0.5, 1.0}, 1.5), {-1.0, 2.0});
    eq(shapeCoefficients({0.25, 0.5, 1.0}, 0.375), {0.5, 0.5, 0.0});
}

TEST_CASE("morph in-betweens: the shape passes through the in-between and exports as its own target")
{
    run("newScene");
    std::string base = run("polyCube", {{"name", "Base"}})[0];
    std::string full = run("polyCube", {{"name", "Smile"}})[0];
    std::string half = run("polyCube", {{"name", "SmileHalf"}})[0];
    editPoints(full, [](geo::Index p, const glm::dvec3&) { return p == 0 ? glm::dvec3(0, 10, 0) : glm::dvec3(0.0); });
    editPoints(half, [](geo::Index p, const glm::dvec3&) { return p == 0 ? glm::dvec3(4, 0, 0) : glm::dvec3(0.0); });
    std::vector<glm::dvec3> rest = points(base);
    std::string m = run("morph", {{"args", {full, base}}});
    std::string t = run("morphTargets", {{"args", {base}}})[0]["node"];
    run("addInBetween", {{"args", {t, half}}, {"weight", 0.5}});
    CHECK(run("morphTargets", {{"args", {base}}})[0]["inBetweens"] == json::array({0.5}));

    auto at = [&](double w) {
        run("setAttr", {{"args", {t + ".weight", w}}});
        return points(base)[0] - rest[0];
    };
    CHECK(near(at(0.25), {2, 0, 0}));
    CHECK(near(at(0.5), {4, 0, 0}));
    CHECK(near(at(0.75), {2, 5, 0}));
    CHECK(near(at(1.0), {0, 10, 0}));

    run("setAttr", {{"args", {t + ".weight", 0.75}}});
    std::vector<morphs::Target> ex = morphs::targets(graph(), graph().find(m)->id());
    REQUIRE(ex.size() == 2);
    CHECK(ex[0].name == "Smile");
    CHECK(ex[1].name == "Smile_50");
    CHECK(ex[0].weight == doctest::Approx(0.5));
    CHECK(ex[1].weight == doctest::Approx(0.5));
    CHECK(morphs::weights(graph(), graph().find(m)->id()) == std::vector<double>{ex[0].weight, ex[1].weight});

    // An in-between made without a mesh starts as the shape the target has there, so nothing moves.
    run("removeInBetween", {{"args", {t}}, {"weight", 0.5}});
    glm::dvec3 before = at(0.3);
    run("addInBetween", {{"args", {t}}, {"weight", 0.3}});
    CHECK(near(at(0.3), before));
    run("undo");
    CHECK(run("morphTargets", {{"args", {base}}})[0]["inBetweens"].empty());

    // In-betweens survive saving.
    run("addInBetween", {{"args", {t, half}}, {"weight", 0.5}});
    for (const char* ext : {".lila", ".lilaa"})
    {
        std::string path = tempFile(std::string("inbetween") + ext).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        run("setAttr", {{"args", {t + ".weight", 0.5}}});
        CHECK(near(points(base)[0] - rest[0], {4, 0, 0}));
    }
}

TEST_CASE("morph target point weights scale every shape of the target, in evaluation and in exports")
{
    run("newScene");
    std::string base = run("polyCube", {{"name", "Base"}})[0];
    std::string full = run("polyCube", {{"name", "Lift"}})[0];
    editPoints(full, [](geo::Index p, const glm::dvec3&) { return p <= 1 ? glm::dvec3(0, 10, 0) : glm::dvec3(0.0); });
    std::vector<glm::dvec3> rest = points(base);
    std::string m = run("morph", {{"args", {full, base}}});
    std::string t = run("morphTargets", {{"args", {base}}})[0]["node"];
    run("setAttr", {{"args", {t + ".weight", 0.5}}});
    auto w = std::make_shared<PointWeights>();
    w->values = {1.0f, 0.25f};
    morphs::setPointWeights(graph(), graph().find(t)->id(), w);
    std::vector<glm::dvec3> now = points(base);
    CHECK(near(now[0] - rest[0], {0, 5, 0}));
    CHECK(near(now[1] - rest[1], {0, 1.25, 0}));

    std::vector<morphs::Target> ex = morphs::targets(graph(), graph().find(m)->id());
    REQUIRE(ex.size() == 1);
    REQUIRE(ex[0].indices.size() == 2);
    CHECK(near(ex[0].deltas[0], {0, 10, 0}));
    CHECK(near(ex[0].deltas[1], {0, 2.5, 0}));
}

TEST_CASE("morph targets rename, flip through the symmetry map and edit in edit mode")
{
    run("newScene");
    std::string base = run("polyCube", {{"name", "Face"}, {"width", 10}, {"height", 10}, {"depth", 10}, {"subdivisionsX", 2}})[0];
    std::string shape = run("polyCube", {{"name", "Brow_L"}, {"width", 10}, {"height", 10}, {"depth", 10}, {"subdivisionsX", 2}})[0];
    editPoints(shape, [](geo::Index, const glm::dvec3& p) { return p.x > 1.0 ? glm::dvec3(1, 2, 0) : glm::dvec3(0.0); });
    std::vector<glm::dvec3> rest = points(base);
    run("morph", {{"args", {shape, base}}});
    std::string t = run("morphTargets", {{"args", {base}}})[0]["node"];

    std::string renamed = run("renameMorphTarget", {{"args", {t, "BrowUp_L"}}});
    CHECK(renamed == "BrowUp_LTarget");
    CHECK(run("morphTargets", {{"args", {base}}})[0]["name"] == "BrowUp_L");
    run("undo");
    CHECK(run("morphTargets", {{"args", {base}}})[0]["name"] == "Brow_L");
    renamed = run("renameMorphTarget", {{"args", {t, "BrowUp_L"}}});

    std::string flipped = run("mirrorMorphTarget", {{"args", {renamed}}});
    json list = run("morphTargets", {{"args", {base}}});
    REQUIRE(list.size() == 2);
    CHECK(list[1]["name"] == "BrowUp_R");
    run("setAttr", {{"args", {flipped + ".weight", 1.0}}});
    std::vector<glm::dvec3> out = points(base);
    for (size_t i = 0; i < rest.size(); ++i)
    {
        CAPTURE(i);
        if (rest[i].x < -1.0)
            CHECK(near(out[i] - rest[i], {-1, 2, 0}));
        else
            CHECK(near(out[i], rest[i]));
    }
    run("setAttr", {{"args", {flipped + ".weight", 0.0}}});

    // Symmetrize copies the left side onto the right in place.
    run("mirrorMorphTarget", {{"args", {renamed}}, {"mode", "symmetrize"}});
    run("setAttr", {{"args", {renamed + ".weight", 1.0}}});
    out = points(base);
    for (size_t i = 0; i < rest.size(); ++i)
        if (rest[i].x < -1.0)
            CHECK(near(out[i] - rest[i], {-1, 2, 0}));

    // Edit mode: sculpt the edit mesh, the morph follows live, turning it off bakes the edit in.
    std::string edit = run("editMorphTarget", {{"args", {renamed}}, {"on", true}});
    CHECK_FALSE(std::get<bool>(graph().value(graph().plug(base + ".visibility"))));
    editPoints(edit, [](geo::Index p, const glm::dvec3&) { return p == 0 ? glm::dvec3(0, 0, 3) : glm::dvec3(0.0); });
    CHECK(near(points(base)[0] - rest[0], glm::dvec3(0, 0, 3) + (rest[0].x > 1.0 || rest[0].x < -1.0 ? glm::dvec3(rest[0].x > 0 ? 1 : -1, 2, 0) : glm::dvec3(0.0))));
    run("editMorphTarget", {{"args", {renamed}}, {"on", false}});
    CHECK(graph().find(edit) == nullptr);
    CHECK(std::get<bool>(graph().value(graph().plug(base + ".visibility"))));
    std::vector<glm::dvec3> baked = points(base);
    CHECK(near(baked[0], out[0] + glm::dvec3(0, 0, 3)));
    run("undo");
    CHECK(graph().find(edit) != nullptr);
}

TEST_CASE("pose driver: falloff from the target pose and twist")
{
    using drivers::Falloff;
    CHECK(drivers::falloffWeight(0.0, 90.0, Falloff::Linear) == doctest::Approx(1.0));
    CHECK(drivers::falloffWeight(45.0, 90.0, Falloff::Linear) == doctest::Approx(0.5));
    CHECK(drivers::falloffWeight(67.5, 90.0, Falloff::Smooth) == doctest::Approx(0.15625));
    CHECK(drivers::falloffWeight(45.0, 90.0, Falloff::Smoother) == doctest::Approx(0.5));
    CHECK(drivers::falloffWeight(120.0, 90.0, Falloff::Smooth) == 0.0);

    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "elbow"}});
    run("setAttr", {{"args", {"elbow.rotateZ", 90.0}}});
    std::string d = run("poseDriver", {{"args", {"elbow"}}, {"falloff", "linear"}});
    CHECK(plugValue(d + ".radius") == doctest::Approx(90.0));
    CHECK(plugValue(d + ".output") == doctest::Approx(1.0));
    run("setAttr", {{"args", {"elbow.rotateZ", 45.0}}});
    CHECK(plugValue(d + ".output") == doctest::Approx(0.5));
    run("setAttr", {{"args", {"elbow.rotateZ", 0.0}}});
    CHECK(plugValue(d + ".output") == doctest::Approx(0.0));
    // Bending past the pose falls off again; twist about the bone axis is ignored until twistWeight says otherwise.
    run("setAttr", {{"args", {"elbow.rotateZ", 135.0}}});
    CHECK(plugValue(d + ".output") == doctest::Approx(0.5));
    run("setAttr", {{"args", {"elbow.rotateZ", 90.0}}});
    run("setAttr", {{"args", {"elbow.rotateX", 30.0}}});
    CHECK(plugValue(d + ".output") == doctest::Approx(1.0).epsilon(1e-6));
    run("setAttr", {{"args", {d + ".twistWeight", 1.0}}});
    CHECK(plugValue(d + ".output") == doctest::Approx(1.0 - 30.0 / 90.0).epsilon(1e-6));
}

TEST_CASE("corrective: the posed result matches the sculpt, applied before the skin, zero at rest")
{
    for (int method : {0, 1})
    {
        CAPTURE(method);
        std::string mesh = skinnedTube(method);
        std::vector<glm::dvec3> rest = points(mesh);
        run("setAttr", {{"args", {"bone_b.rotateZ", 90.0}}});
        std::string sculpt = run("sculptCorrective", {{"args", {mesh}}});
        CHECK(sculpt == mesh + "_corrective");
        // Push the inside of the elbow out and lift a few points, as a fix would.
        editPoints(sculpt, [](geo::Index p, const glm::dvec3& x) {
            if (std::abs(x.y) < 15.0 && std::abs(x.x) < 20.0)
                return glm::dvec3(1.5, 0.5, double(p % 3) * 0.25);
            return glm::dvec3(0.0);
        });
        std::vector<glm::dvec3> want = worldPoints(sculpt);
        json made = run("addCorrective", {{"args", {sculpt}}, {"driver", "bone_b"}, {"falloff", "linear"}});
        CHECK(made["residual"].get<double>() < 1e-6);
        CHECK(graph().find(sculpt) == nullptr);
        CHECK(maxDistance(worldPoints(mesh), want) < 1e-4);

        // The fix lives in the morph under the skin and fades out on the way to rest.
        json stack = run("modifiers", {{"args", {mesh}}});
        CHECK(graph().find(stack[0].get<std::string>())->type().isA("morph"));
        run("setAttr", {{"args", {"bone_b.rotateZ", 0.0}}});
        CHECK(maxDistance(points(mesh), rest) < 1e-9);
        run("setAttr", {{"args", {"bone_b.rotateZ", 45.0}}});
        json t = run("morphTargets", {{"args", {mesh}}})[0];
        CHECK(t["weight"].get<double>() == doctest::Approx(0.5));
        CHECK(t["driver"].get<std::string>() == made["drivers"][0].get<std::string>() + ".output");

        // Undo takes the whole corrective back, sculpt included.
        run("undo");
        run("undo");
        run("undo");
        CHECK(graph().find(sculpt) != nullptr);
        CHECK(run("deformers", {{"args", {mesh}}}).empty());
    }
}

TEST_CASE("combination corrective: the drivers multiply")
{
    std::string mesh = skinnedTube();
    run("setAttr", {{"args", {"bone_a.rotateZ", 30.0}}});
    run("setAttr", {{"args", {"bone_b.rotateZ", 60.0}}});
    std::string sculpt = run("sculptCorrective", {{"args", {mesh}}});
    editPoints(sculpt, [](geo::Index, const glm::dvec3& x) { return std::abs(x.y) < 10.0 ? glm::dvec3(0, 0, 2) : glm::dvec3(0.0); });
    std::vector<glm::dvec3> want = worldPoints(sculpt);
    json made = run("addCorrective", {{"args", {sculpt, mesh}}, {"driver", {"bone_a", "bone_b"}}, {"falloff", "linear"}});
    REQUIRE(made["drivers"].size() == 2);
    CHECK(maxDistance(worldPoints(mesh), want) < 1e-4);
    std::string target = made["target"];
    CHECK(plugValue(target + ".weight") == doctest::Approx(1.0));
    run("setAttr", {{"args", {"bone_a.rotateZ", 15.0}}});
    CHECK(plugValue(target + ".weight") == doctest::Approx(0.5));
    run("setAttr", {{"args", {"bone_b.rotateZ", 30.0}}});
    CHECK(plugValue(target + ".weight") == doctest::Approx(0.25));
    run("setAttr", {{"args", {"bone_a.rotateZ", 0.0}}});
    CHECK(plugValue(target + ".weight") == doctest::Approx(0.0));
}

TEST_CASE("driven channels: key interpolation, infinity, edits and removal")
{
    run("newScene");
    run("spaceLocator", {{"name", "ctrl"}});
    run("polyCube", {{"name", "Box"}});
    json made = run("driveChannel", {{"args", {"Box.translateY"}}, {"driver", "ctrl.translateX"}, {"keys", {{0, 0}, {10, 5}, {20, 5}}}});
    REQUIRE(made.size() == 1);
    std::string ch = made[0];
    auto ty = [&](double x) {
        run("setAttr", {{"args", {"ctrl.translateX", x}}});
        return plugValue("Box.translateY");
    };
    CHECK(ty(0.0) == doctest::Approx(0.0));
    CHECK(ty(5.0) == doctest::Approx(2.5));
    CHECK(ty(15.0) == doctest::Approx(5.0));
    CHECK(ty(-10.0) == doctest::Approx(0.0));
    run("editDrivenChannel", {{"args", {ch}}, {"preInfinity", "linear"}});
    CHECK(ty(-10.0) == doctest::Approx(-5.0));
    run("editDrivenChannel", {{"args", {ch}}, {"interpolation", "step"}});
    CHECK(ty(5.0) == doctest::Approx(0.0));
    CHECK(ty(12.0) == doctest::Approx(5.0));
    run("editDrivenChannel", {{"args", {ch}}, {"interpolation", "spline"}});
    double mid = ty(5.0);
    CHECK(mid > 0.0);
    CHECK(mid < 5.0);

    // A key at the driver's value now.
    run("editDrivenChannel", {{"args", {ch}}, {"keys", {{0, 0}, {10, 5}}}});
    run("setAttr", {{"args", {"ctrl.translateX", 20.0}}});
    run("setDrivenKey", {{"args", {ch}}, {"value", 9.0}});
    CHECK(ty(15.0) == doctest::Approx(7.0));
    json info = run("drivenChannels", {{"args", {"Box"}}});
    REQUIRE(info.size() == 1);
    CHECK(info[0]["keys"].size() == 3);
    CHECK(info[0]["driver"] == "ctrl.translateX");
    CHECK(info[0]["driven"] == json::array({"Box.translateY"}));

    // Removing leaves the driven plug where it is.
    ty(15.0);
    run("removeDrivenChannel", {{"args", {ch}}});
    CHECK(run("drivenChannels").empty());
    CHECK(plugValue("Box.translateY") == doctest::Approx(7.0));
    run("setAttr", {{"args", {"ctrl.translateX", 0.0}}});
    CHECK(plugValue("Box.translateY") == doctest::Approx(7.0));
    run("undo");
    run("undo");
    CHECK(run("drivenChannels").size() == 1);
}

TEST_CASE("a material parameter driven by a joint's rotation, baked into a named curve on export")
{
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(0, 10, 0)}, {"name", "brow"}});
    std::string mat = run("createMaterial", {{"args", {"Skin"}}});
    run("setAttr", {{"args", {"brow.rotateZ", 40.0}}});
    std::string d = run("poseDriver", {{"args", {"brow"}}, {"falloff", "linear"}});
    run("setAttr", {{"args", {"brow.rotateZ", 0.0}}});
    json made = run("driveChannel", {{"args", {mat + ".roughness"}}, {"driver", d + ".output"}, {"keys", {{0, 0.3}, {1, 0.9}}}});
    REQUIRE(made.size() == 1);
    json info = run("drivenChannels", {{"args", {mat}}});
    CHECK(info[0]["curveName"] == "Roughness_mat");
    CHECK(plugValue(mat + ".roughness") == doctest::Approx(0.3));
    run("setAttr", {{"args", {"brow.rotateZ", 20.0}}});
    CHECK(plugValue(mat + ".roughness") == doctest::Approx(0.6));
    run("setAttr", {{"args", {"brow.rotateZ", 40.0}}});
    CHECK(plugValue(mat + ".roughness") == doctest::Approx(0.9));

    // A curve only channel: no Lila plug, just a curve for an Unreal material parameter.
    run("driveChannel", {{"driver", d + ".output"}, {"curveName", "Wrinkle_mat"}});

    run("setKeyframe", {{"args", {"brow.rotateZ"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"brow.rotateZ"}}, {"time", 1.0}, {"value", 40.0}});
    std::string path = tempFile("driven_curves.fbx").string();
    run("exportFbx", {{"args", {path, "root"}}, {"kind", "animation"}, {"start", 0}, {"end", 30}, {"profile", "generic"}});

    ufbx_load_opts opts{};
    ufbx_error err{};
    ufbx_scene* fbx = ufbx_load_file(path.c_str(), &opts, &err);
    REQUIRE(fbx);
    const ufbx_node* root = ufbx_find_node(fbx, "root");
    REQUIRE(root);
    REQUIRE(fbx->anim_layers.count >= 1);
    for (const char* curve : {"Roughness_mat", "Wrinkle_mat"})
    {
        CAPTURE(curve);
        const ufbx_anim_prop* prop = ufbx_find_anim_prop(fbx->anim_layers.data[0], &root->element, curve);
        REQUIRE(prop);
        const ufbx_anim_curve* c = prop->anim_value->curves[0];
        REQUIRE(c);
        const double start = std::string(curve) == "Roughness_mat" ? 0.3 : 0.0, end = std::string(curve) == "Roughness_mat" ? 0.9 : 1.0;
        CHECK(ufbx_evaluate_curve(c, 0.0, 0.0) == doctest::Approx(start).epsilon(1e-4));
        CHECK(ufbx_evaluate_curve(c, 1.0, 0.0) == doctest::Approx(end).epsilon(1e-4));
    }
    ufbx_free_scene(fbx);

    // Drivers and channels save, load and describe themselves for Unreal.
    std::string scenePath = tempFile("drivers.lilaa").string();
    run("saveScene", {{"args", {scenePath}}});
    run("newScene");
    run("openScene", {{"args", {scenePath}}});
    run("currentTime", {{"args", {0.5}}});
    CHECK(plugValue(mat + ".roughness") == doctest::Approx(0.6).epsilon(1e-3));
    std::string jsonPath = tempFile("drivers.json").string();
    json data = run("exportDriverData", {{"args", {jsonPath}}});
    CHECK(data["poseDrivers"].size() == 1);
    CHECK(data["drivenChannels"].size() == 2);
    CHECK(fs::exists(jsonPath));
}

TEST_CASE("animation exports carry morph weights as curves; skeletal exports write in-betweens as targets")
{
    std::string mesh = skinnedTube();
    run("setAttr", {{"args", {"bone_b.rotateZ", 90.0}}});
    std::string sculpt = run("sculptCorrective", {{"args", {mesh}}});
    editPoints(sculpt, [](geo::Index, const glm::dvec3& x) { return std::abs(x.y) < 10.0 ? glm::dvec3(2, 0, 0) : glm::dvec3(0.0); });
    json made = run("addCorrective", {{"args", {sculpt}}, {"driver", "bone_b"}, {"name", "elbowFix"}, {"falloff", "linear"}});
    run("setKeyframe", {{"args", {"bone_b.rotateZ"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"bone_b.rotateZ"}}, {"time", 1.0}, {"value", 90.0}});

    std::string anim = tempFile("corrective_anim.fbx").string();
    run("exportFbx", {{"args", {anim, "bone_a"}}, {"kind", "animation"}, {"start", 0}, {"end", 30}, {"profile", "generic"}});
    ufbx_load_opts opts{};
    ufbx_error err{};
    ufbx_scene* fbx = ufbx_load_file(anim.c_str(), &opts, &err);
    REQUIRE(fbx);
    const ufbx_node* root = ufbx_find_node(fbx, "bone_a");
    REQUIRE(root);
    const ufbx_anim_prop* prop = ufbx_find_anim_prop(fbx->anim_layers.data[0], &root->element, "elbowFix");
    REQUIRE(prop);
    CHECK(ufbx_evaluate_curve(prop->anim_value->curves[0], 0.5, 0.0) == doctest::Approx(0.5).epsilon(1e-3));
    CHECK(ufbx_evaluate_curve(prop->anim_value->curves[0], 1.0, 0.0) == doctest::Approx(1.0).epsilon(1e-3));
    ufbx_free_scene(fbx);

    // Skeletal: the corrective goes out as an ordinary morph target with its baked weight curve.
    std::string target = made["target"];
    run("addInBetween", {{"args", {target}}, {"weight", 0.5}});
    std::string skel = tempFile("corrective_skel.fbx").string();
    run("exportFbx", {{"args", {skel, mesh, "bone_a"}}, {"kind", "skeletal"}, {"start", 0}, {"end", 30}, {"profile", "generic"}});
    fbx = ufbx_load_file(skel.c_str(), &opts, &err);
    REQUIRE(fbx);
    REQUIRE(fbx->blend_deformers.count == 1);
    const ufbx_blend_deformer* bd = fbx->blend_deformers.data[0];
    REQUIRE(bd->channels.count == 2);
    CHECK(std::string(bd->channels.data[0]->name.data) == "elbowFix");
    CHECK(std::string(bd->channels.data[1]->name.data) == "elbowFix_50");
    CHECK(ufbx_find_anim_prop(fbx->anim_layers.data[0], &bd->channels.data[0]->element, "DeformPercent") != nullptr);
    ufbx_free_scene(fbx);
}

TEST_CASE("scene files with the older morph type names load as morphs")
{
    run("newScene");
    std::string base = run("polyCube", {{"name", "Base"}})[0];
    std::string full = run("polyCube", {{"name", "Smile"}})[0];
    editPoints(full, [](geo::Index p, const glm::dvec3&) { return p == 0 ? glm::dvec3(0, 10, 0) : glm::dvec3(0.0); });
    std::vector<glm::dvec3> rest = points(base);
    run("morph", {{"args", {full, base}}, {"name", "Faces"}});
    std::string t = run("morphTargets", {{"args", {base}}})[0]["node"];
    run("setAttr", {{"args", {t + ".weight", 0.5}}});
    std::string path = tempFile("old_names.lilaa").string();
    run("saveScene", {{"args", {path}}});

    std::stringstream text;
    text << std::ifstream(path, std::ios::binary).rdbuf();
    std::string s = text.str();
    auto replaceAll = [&](const std::string& a, const std::string& b) {
        size_t n = 0;
        for (size_t at = s.find(a); at != std::string::npos; at = s.find(a, at + b.size()), ++n)
            s.replace(at, a.size(), b);
        return n;
    };
    const std::string oldTarget = std::string("blend") + "Target", oldMorph = std::string("blend") + "Shape", oldDeltas = std::string("blend") + "Deltas";
    CHECK(replaceAll("node morphTarget ", "node " + oldTarget + " ") == 1);
    CHECK(replaceAll("node morph ", "node " + oldMorph + " ") == 1);
    CHECK(replaceAll("\"morphDeltas\"", "\"" + oldDeltas + "\"") >= 1);
    std::ofstream(path, std::ios::binary) << s;

    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(graph().find("Faces")->type().isA("morph"));
    CHECK(near(points(base)[0], rest[0] + glm::dvec3(0, 5, 0)));
}
