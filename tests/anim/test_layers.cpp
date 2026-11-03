#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/RotationCurve.h"
#include "scene/Take.h"
#include "scene/Transform.h"

#include <filesystem>

using namespace lila;
using namespace animtest;
using json = nlohmann::json;
using scene::Graph;

namespace
{
glm::dvec3 rotateOf(const std::string& node)
{
    Graph& g = Graph::current();
    g.evaluate();
    return g.find(node)->as<glm::dvec3>(scene::Transform::kRotate);
}

void checkMat(const glm::dmat4& a, const glm::dmat4& b)
{
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            CHECK(a[c][r] == doctest::Approx(b[c][r]).epsilon(1e-9));
}

void fresh()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("spaceLocator", {{"name", "loc"}});
}
} // namespace

TEST_CASE("additive quaternion layering equals the matrix product")
{
    using namespace scene;
    const glm::dvec3 base(30, -20, 45), layer(10, 60, -35);
    for (RotateOrder order : {RotateOrder::XYZ, RotateOrder::ZXY})
    {
        glm::dvec3 r = anim::blendRotation(base, layer, 1.0, anim::LayerMode::Additive, anim::Accumulation::Quaternion, order);
        checkMat(eulerToMatrix(r, order), eulerToMatrix(base, order) * eulerToMatrix(layer, order));
    }

    fresh();
    run("setAttr", {{"args", {"loc.rotate", 30, -20, 45}}});
    run("setKeyframe", {{"args", {"loc.rotate"}}, {"time", 0.0}});
    std::string l = run("createAnimLayer", {{"args", {"loc.rotate"}}, {"name", "Add"}});
    CHECK(l == "Add");
    // Unkeyed members change nothing.
    checkMat(eulerToMatrix(rotateOf("loc"), RotateOrder::XYZ), eulerToMatrix(base, RotateOrder::XYZ));
    Graph& g = Graph::current();
    NodeId blend = anim::blendFor(g, {g.find("loc")->id(), Transform::kRotate, -1}, g.find("Add")->id());
    REQUIRE(blend);
    run("setAttr", {{"args", {g.node(blend)->name() + ".inputB", 10, 60, -35}}});
    checkMat(eulerToMatrix(rotateOf("loc"), RotateOrder::XYZ), eulerToMatrix(base, RotateOrder::XYZ) * eulerToMatrix(layer, RotateOrder::XYZ));

    // Half weight is half the layer's rotation, in quaternion space.
    run("editAnimLayer", {{"args", {"Add"}}, {"weight", 0.5}});
    glm::dvec3 half = anim::blendRotation(base, layer, 0.5, anim::LayerMode::Additive, anim::Accumulation::Quaternion, RotateOrder::XYZ);
    checkMat(eulerToMatrix(rotateOf("loc"), RotateOrder::XYZ), eulerToMatrix(half, RotateOrder::XYZ));
    run("editAnimLayer", {{"args", {"Add"}}, {"mute", true}});
    checkMat(eulerToMatrix(rotateOf("loc"), RotateOrder::XYZ), eulerToMatrix(base, RotateOrder::XYZ));
}

TEST_CASE("keying a member on the active layer creates the layer curve and the plug reads the keyed value")
{
    fresh();
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 1.0}, {"value", 10.0}});
    run("createAnimLayer", {{"name", "Over"}, {"empty", true}});
    at(1.0);
    run("setKeyframe", {{"args", {"loc.ty"}}, {"value", 12.0}});
    CHECK(exists("loc_translateY_Over"));
    CHECK(get("loc.ty") == doctest::Approx(12));
    at(0.5);
    // Layer key 2 everywhere (one key) over base 5.
    CHECK(get("loc.ty") == doctest::Approx(7));
    json layers = run("listAnimLayers");
    REQUIRE(layers.size() == 2);
    CHECK(layers[1]["members"] == json({"loc.translate"}));

    run("editAnimLayer", {{"args", {"Over"}}, {"lock", true}});
    CHECK_THROWS(run("setKeyframe", {{"args", {"loc.ty"}}, {"value", 1.0}}));
    run("editAnimLayer", {{"args", {"Over"}}, {"lock", false}});

    // Base keys while layered still land on the base curve.
    run("setActiveAnimLayer", {{"args", {"BaseAnimation"}}});
    at(1.0);
    run("setKeyframe", {{"args", {"loc.ty"}}, {"value", 20.0}});
    CHECK(get("loc.ty") == doctest::Approx(20));
    run("editAnimLayer", {{"args", {"Over"}}, {"mute", true}});
    CHECK(get("loc.ty") == doctest::Approx(18));

    run("deleteAnimLayer", {{"args", {"Over"}}});
    CHECK_FALSE(exists("loc_translateY_Over"));
    CHECK(get("loc.ty") == doctest::Approx(18));
}

TEST_CASE("merging layers bakes down to the evaluated result")
{
    fresh();
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 1.0}, {"value", 10.0}});
    run("setKeyframe", {{"args", {"loc.rx"}}, {"time", 0.0}, {"value", 0.0}});
    run("createAnimLayer", {{"name", "A"}, {"args", {"loc"}}});
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 0.0}, {"value", 1.0}});
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 1.0}, {"value", 15.0}});
    run("setKeyframe", {{"args", {"loc.rx"}}, {"time", 0.5}, {"value", 40.0}});
    run("setKeyframe", {{"args", {"loc.rz"}}, {"time", 0.5}, {"value", 25.0}});
    run("editAnimLayer", {{"args", {"A"}}, {"weight", 0.7}});

    std::vector<glm::dvec3> before, rotBefore;
    for (int f = 0; f <= 30; ++f)
    {
        at(f / 30.0);
        before.push_back({get("loc.tx"), get("loc.ty"), get("loc.tz")});
        rotBefore.push_back(rotateOf("loc"));
    }
    CHECK(run("mergeAnimLayers", {{"args", {"A"}}}) == "BaseAnimation");
    CHECK_FALSE(exists("A"));
    CHECK(run("listAnimLayers").size() == 1);
    for (int f = 0; f <= 30; ++f)
    {
        at(f / 30.0);
        CHECK(get("loc.ty") == doctest::Approx(before[size_t(f)].y));
        checkMat(scene::eulerToMatrix(rotateOf("loc"), scene::RotateOrder::XYZ),
                 scene::eulerToMatrix(rotBefore[size_t(f)], scene::RotateOrder::XYZ));
    }
    run("undo");
    CHECK(exists("A"));
    at(0.5);
    CHECK(get("loc.ty") == doctest::Approx(before[15].y));
}

TEST_CASE("extract, duplicate and reorder keep the layered result")
{
    fresh();
    run("setKeyframe", {{"args", {"loc.tx"}}, {"time", 0.0}, {"value", 1.0}});
    run("setKeyframe", {{"args", {"loc.tx"}}, {"time", 1.0}, {"value", 3.0}});
    std::string ex = run("extractAnimLayer", {{"args", {"loc.tx"}}, {"layer", "BaseAnimation"}, {"name", "Ex"}});
    at(0.5);
    CHECK(get("loc.tx") == doctest::Approx(2));
    run("createAnimLayer", {{"name", "Add"}, {"args", {"loc.tx"}}, {"mode", "additive"}});
    run("setKeyframe", {{"args", {"loc.tx"}}, {"value", 5.0}});
    CHECK(get("loc.tx") == doctest::Approx(5));
    run("duplicateAnimLayer", {{"args", {"Add"}}, {"name", "Add2"}});
    CHECK(get("loc.tx") == doctest::Approx(8)); // the additive offset (3) applies twice
    // Moving the override layer to the top hides the additive ones under it.
    run("reorderAnimLayer", {{"args", {ex}}, {"index", 2}});
    CHECK(get("loc.tx") == doctest::Approx(2));
    run("undo");
    CHECK(get("loc.tx") == doctest::Approx(8));
}

TEST_CASE("constrained channels go through a blend and can be keyed and blended")
{
    fresh();
    run("spaceLocator", {{"name", "drv"}});
    run("setAttr", {{"args", {"drv.translate", 5, 0, 0}}});
    run("pointConstraint", {{"args", {"drv", "loc"}}, {"maintainOffset", false}});
    CHECK(get("loc.tx") == doctest::Approx(5));
    run("setKeyframe", {{"args", {"loc.tx"}}, {"time", 0.0}, {"value", 1.0}});
    CHECK(get("loc.tx") == doctest::Approx(5));
    run("setAttr", {{"args", {"loc_translate_constraintBlend.weight", 0.0}}});
    CHECK(get("loc.tx") == doctest::Approx(1));
    run("setAttr", {{"args", {"loc_translate_constraintBlend.weight", 0.5}}});
    CHECK(get("loc.tx") == doctest::Approx(3));
    run("removeConstraint", {{"args", {"loc"}}});
    CHECK(get("loc.tx") == doctest::Approx(1));
}

TEST_CASE("takes switch curve connections, undo, and save")
{
    fresh();
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 1.0}, {"value", 10.0}});
    CHECK(run("createTake", {{"name", "Walk"}, {"start", 0.0}, {"end", 1.0}}) == "Walk");
    CHECK(run("createTake", {{"name", "Run"}, {"start", 0.0}, {"end", 2.0}, {"fps", 24.0}}) == "Run");
    run("setActiveTake", {{"args", {"Run"}}});
    CHECK(scene::anim::playback().fps == doctest::Approx(24));
    CHECK(scene::anim::playback().maxTime == doctest::Approx(2));
    at(0.0);
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 0.0}, {"value", -4.0}});
    at(0.5);
    CHECK(get("loc.ty") == doctest::Approx(-4));
    run("setActiveTake", {{"args", {"Walk"}}});
    CHECK(get("loc.ty") == doctest::Approx(5));
    CHECK(scene::anim::playback().maxTime == doctest::Approx(1));
    run("undo");
    CHECK(get("loc.ty") == doctest::Approx(-4));
    CHECK(scene::anim::playback().maxTime == doctest::Approx(2));
    run("redo");
    CHECK(get("loc.ty") == doctest::Approx(5));

    run("duplicateTake", {{"args", {"Run"}}, {"name", "Sprint"}});
    run("renameTake", {{"args", {"Walk", "Stroll"}}});
    json list = run("listTakes");
    REQUIRE(list.size() == 3);
    CHECK(list[0]["name"] == "Stroll");
    CHECK(list[0]["active"] == true);
    CHECK(list[2]["channels"] == 1);

    for (const char* file : {"takes.lilaa", "takes.lila"})
    {
        std::string path = tempPath(file);
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        at(0.5);
        CHECK(get("loc.ty") == doctest::Approx(5));
        run("setActiveTake", {{"args", {"Sprint"}}});
        CHECK(get("loc.ty") == doctest::Approx(-4));
        run("setActiveTake", {{"args", {"Stroll"}}});
    }

    run("deleteTake", {{"args", {"Sprint"}}});
    CHECK(run("listTakes").size() == 2);
}

TEST_CASE("FBX export writes one AnimStack per take")
{
    fresh();
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 1.0}, {"value", 10.0}});
    run("createTake", {{"name", "Idle"}, {"start", 0.0}, {"end", 1.0}});
    run("createTake", {{"name", "Jump"}, {"start", 0.0}, {"end", 0.5}});
    run("setActiveTake", {{"args", {"Jump"}}});
    run("setKeyframe", {{"args", {"loc.tx"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"loc.tx"}}, {"time", 0.5}, {"value", 3.0}});

    Graph& g = Graph::current();
    fbx::ExportOptions opts;
    opts.kind = fbx::ExportKind::Animation;
    opts.takes = {"all"};
    fbx::ExportScene snap = fbx::gatherExport(g, {g.find("loc")->id()}, opts);
    REQUIRE(snap.take);
    REQUIRE(snap.extraTakes.size() == 1);
    CHECK(snap.take->name == "Idle");
    CHECK(snap.extraTakes[0].name == "Jump");
    CHECK(snap.take->times.size() == 31);
    CHECK(snap.extraTakes[0].times.size() == 16);
    // The scene is left on the take that was active.
    at(0.5);
    CHECK(get("loc.tx") == doctest::Approx(3));
    std::string bytes = fbx::encodeFbx(snap, opts);
    size_t stacks = 0;
    for (size_t p = bytes.find("AnimationStack"); p != std::string::npos; p = bytes.find("AnimationStack", p + 1))
        ++stacks;
    // One in Definitions, one object per take.
    CHECK(stacks >= 3);
    CHECK(bytes.find("Idle") != std::string::npos);
    CHECK(bytes.find("Jump") != std::string::npos);
}

TEST_CASE("rotation curve channels layer, constrain, merge and duplicate as rotation curves")
{
    using namespace scene;
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("select", {{"args", json::array()}});
    run("joint", {{"name", "j"}});
    Graph& g = Graph::current();
    run("setAttr", {{"args", {"j.rotate", 20, 0, 0}}});
    run("setKeyframe", {{"args", {"j.rotate"}}, {"time", 0.0}});
    run("setAttr", {{"args", {"j.rotate", 80, 10, 0}}});
    run("setKeyframe", {{"args", {"j.rotate"}}, {"time", 1.0}});
    NodeId j = g.find("j")->id();
    auto src = g.source({j, Transform::kRotate, -1});
    REQUIRE(src);
    CHECK(anim::isRotationCurve(*g.node(src->node)));

    run("createAnimLayer", {{"name", "Lay"}, {"empty", true}});
    at(0.5);
    glm::dvec3 basePose = rotateOf("j");
    run("setKeyframe", {{"args", {"j.rz"}}, {"value", basePose.z + 30.0}});
    NodeId blend = anim::blendFor(g, {j, Transform::kRotate, -1}, g.find("Lay")->id());
    REQUIRE(blend);
    auto layerCurve = g.source({blend, anim::BlendNode::kInputB, -1});
    REQUIRE(layerCurve);
    CHECK(anim::isRotationCurve(*g.node(layerCurve->node)));
    auto baseCurve = g.source({blend, anim::BlendNode::kInputA, -1});
    REQUIRE(baseCurve);
    CHECK(anim::isRotationCurve(*g.node(baseCurve->node)));
    glm::dvec3 keyed = rotateOf("j");
    CHECK(keyed.x == doctest::Approx(basePose.x));
    CHECK(keyed.z == doctest::Approx(basePose.z + 30.0));

    // Base keys under the layer go to the base rotation curve.
    run("setActiveAnimLayer", {{"args", {"BaseAnimation"}}});
    at(1.0);
    run("setKeyframe", {{"args", {"j.ry"}}, {"value", rotateOf("j").y + 5.0}});
    CHECK(anim::isRotationCurve(*g.node(g.source({blend, anim::BlendNode::kInputA, -1})->node)));

    run("duplicateAnimLayer", {{"args", {"Lay"}}, {"name", "Lay2"}});
    NodeId blend2 = anim::blendFor(g, {j, Transform::kRotate, -1}, g.find("Lay2")->id());
    REQUIRE(blend2);
    CHECK(anim::isRotationCurve(*g.node(g.source({blend2, anim::BlendNode::kInputB, -1})->node)));
    run("deleteAnimLayer", {{"args", {"Lay2"}}});

    std::vector<glm::dmat4> before;
    for (int f = 0; f <= 30; ++f)
    {
        at(f / 30.0);
        before.push_back(eulerToMatrix(rotateOf("j"), RotateOrder::XYZ));
    }
    run("mergeAnimLayers", {{"args", {"Lay"}}});
    auto merged = g.source({j, Transform::kRotate, -1});
    REQUIRE(merged);
    CHECK(anim::isRotationCurve(*g.node(merged->node)));
    for (int f = 0; f <= 30; ++f)
    {
        at(f / 30.0);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                CHECK(eulerToMatrix(rotateOf("j"), RotateOrder::XYZ)[c][r] == doctest::Approx(before[size_t(f)][c][r]).epsilon(1e-6));
    }

    // Under an orient constraint, base keys still go to the rotation curve, and the blend weight mixes them.
    run("spaceLocator", {{"name", "aim"}});
    run("orientConstraint", {{"args", {"aim", "j"}}, {"maintainOffset", false}});
    at(0.0);
    run("setKeyframe", {{"args", {"j.rx"}}, {"value", 45.0}});
    NodeId cblend = anim::blendChain(g, {j, Transform::kRotate, -1}).back();
    CHECK(anim::isRotationCurve(*g.node(g.source({cblend, anim::BlendNode::kInputA, -1})->node)));
    run("setAttr", {{"args", {g.node(cblend)->name() + ".weight", 0.0}}});
    CHECK(rotateOf("j").x == doctest::Approx(45.0));
}

TEST_CASE("take roots survive a rename")
{
    fresh();
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 0.0}, {"value", 3.0}});
    run("createTake", {{"name", "Hero"}, {"root", "loc"}});
    run("rename", {{"args", {"loc", "hero"}}});
    json list = run("listTakes");
    REQUIRE(list.size() == 1);
    CHECK(list[0]["root"] == "hero");
    CHECK(list[0]["channels"] == 1);
    run("createTake", {{"name", "Other"}, {"root", "hero"}, {"activate", true}});
    CHECK(run("listTakes")[0]["channels"] == 1);
    run("setActiveTake", {{"args", {"Hero"}}});
    CHECK(get("hero.ty") == doctest::Approx(3));
}
