#include <doctest/doctest.h>

#include "../../extensions/Overlap/OverlapSolver.h"

#include "cmds/Command.h"
#include "extensions/ExtensionManager.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/RotationCurve.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/DrawOverrides.h"
#include "ui/Menus.h"
#include "ui/Panels.h"

#include <tbb/global_control.h>

#include <cstring>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
using Approx = doctest::Approx;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

/** A root, hips keyed side to side over frames 0..30, and a four-joint tail behind the hips. */
void tailScene(bool loop)
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 1.0}, {"fps", 30.0}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(0, 100, 0)}, {"name", "hips"}});
    for (int i = 1; i <= 4; ++i)
        run("joint", {{"position", v3(0, 100, -10.0 * i)}, {"name", "tail_0" + std::to_string(i)}});
    const double xs[] = {0, 40, 0, -40, 0};
    for (int k = 0; k < 5; ++k)
        run("setKeyframe", {{"args", {"hips.tx"}}, {"time", k * 7.5 / 30.0}, {"value", xs[k]}});
    if (loop)
        run("setLoopKey", {{"args", {"hips"}}, {"time", 1.0}});
}

/** Keys of every curve on a layer, as text, in member order. */
std::string layerKeys(const std::string& layer)
{
    Graph& g = Graph::current();
    const NodeId l = g.find(layer)->id();
    std::string out;
    for (const PlugRef& m : anim::layerMembers(g, l))
    {
        const NodeId b = anim::blendFor(g, m, l);
        std::vector<PlugRef> srcs;
        if (auto s = g.source({b, anim::BlendNode::kInputB, -1}))
            srcs.push_back(*s);
        for (int k = 0; k < 3; ++k)
            if (auto s = g.source({b, anim::BlendNode::kInputB, k}))
                srcs.push_back(*s);
        for (const PlugRef& s : srcs)
        {
            const Node& c = *g.node(s.node);
            out += g.plugName(m) + ":" + c.as<std::string>(c.type().attrIndex("keys")) + "\n";
        }
    }
    return out;
}

std::string overlapPackage() { return LILA_TEST_OVERLAP_PACKAGE; }

json uiRegistries()
{
    return {{"actions", ui::actions::registry().ids()}, {"drawOverrides", ui::drawOverrides::registry().ids()},
            {"items", ui::menus::itemRegistry().ids()}, {"panels", ui::panels::registry().ids()},
            {"types", types::registry().ids()}, {"commands", cmds::registry().ids()}, {"solvers", sim::solverTypes().ids()}};
}

void loadOverlap()
{
    extensions::ExtensionManager& em = extensions::ExtensionManager::get();
    if (!sim::solverTypes().find("overlap"))
        REQUIRE(em.load(overlapPackage()) == "Overlap");
}
} // namespace

TEST_CASE("overlap solver: the implicit spring matches a hand-worked step")
{
    glm::dvec3 x(1, 0, 0), v(0);
    overlap::solveSpring(x, v, glm::dvec3(0), 100.0, 10.0, 0.1);
    // v = (0 + (0 - 100 * 1) * 0.1) / (1 + 0.1 * 10 + 0.01 * 100) = -10 / 3; x = 1 + v * 0.1.
    CHECK(v.x == Approx(-10.0 / 3.0).epsilon(1e-15));
    CHECK(x.x == Approx(2.0 / 3.0).epsilon(1e-15));

    // Steady force settles at F / k exactly: backward Euler's fixed point.
    glm::dvec3 y(0), w(0);
    for (int i = 0; i < 4000; ++i)
        overlap::solveSpring(y, w, glm::dvec3(0, -980, 0), 250.0, 12.0, 1.0 / 60.0);
    CHECK(y.y == Approx(-980.0 / 250.0).epsilon(1e-12));
}

TEST_CASE("overlap solver: the soft limit saturates with tanh and bleeds only outward velocity")
{
    glm::dvec3 x(10, 0, 0), v(5, 2, 0);
    overlap::clampDeviation(x, v, 10.0);
    // Above half the limit: half + range * tanh((size - half) / range) = 5 + 5 tanh(1); fully saturated, so the outward
    // component goes and the sideways one stays.
    CHECK(x.x == Approx(5.0 + 5.0 * std::tanh(1.0)).epsilon(1e-15));
    CHECK(v.x == Approx(0.0));
    CHECK(v.y == Approx(2.0));
    glm::dvec3 a(4, 0, 0), b(-3, 0, 0);
    overlap::clampDeviation(a, b, 10.0);
    CHECK(a.x == 4.0);
    CHECK(b.x == -3.0);
    glm::dvec3 c(7.5, 0, 0), d(-1, 0, 0);
    overlap::clampDeviation(c, d, 10.0);
    CHECK(d.x == -1.0); // moving inward keeps its velocity
}

TEST_CASE("overlap solver: inertia, feed-forward and exaggeration")
{
    overlap::BoneSettings b;
    b.response.velocityInfluence = 0.5;
    overlap::BoneState s;
    overlap::reset(s, glm::dvec3(0), glm::dquat(1, 0, 0, 0));
    overlap::Context ctx;
    overlap::integrate(b, s, glm::dvec3(1, 0, 0), glm::dquat(1, 0, 0, 0), 0.1, ctx);
    // The target jumps 1 in 0.1 s from rest: velocity 10, acceleration 100, so the pseudo-force is -100.
    const double vx = (-100.0 * 0.1) / (1.0 + 0.1 * 12.0 + 0.01 * 250.0);
    CHECK(s.velocity.x == Approx(vx).epsilon(1e-15));
    CHECK(s.offset.x == Approx(vx * 0.1).epsilon(1e-15));
    CHECK(s.feedForward.x == Approx(-5.0));

    overlap::BoneState fixed;
    fixed.offset = glm::dvec3(2, 0, 0);
    b.style.exaggeration = 1.5;
    overlap::Pose p = overlap::compose(b, fixed, glm::dvec3(10, 0, 0), glm::dquat(1, 0, 0, 0), 1.0, glm::dvec3(1, 0, 0));
    CHECK(p.position.x == Approx(13.0));
    // Half alpha lands halfway.
    p = overlap::compose(b, fixed, glm::dvec3(10, 0, 0), glm::dquat(1, 0, 0, 0), 0.5, glm::dvec3(1, 0, 0));
    CHECK(p.position.x == Approx(11.5));
}

TEST_CASE("overlap solver: chain distribution, tip scales and global multipliers")
{
    overlap::ChainBias even;
    for (double a : {0.0, 0.5, 1.0})
        CHECK(even.distribution(a, 3) == Approx(1.0 / 3.0));
    overlap::ChainBias root;
    root.bias = 1.0; // fades out toward the root: weights 0, 0.5, 1 over a total of 1.5
    CHECK(root.distribution(0.0, 3) == Approx(0.0));
    CHECK(root.distribution(1.0, 3) == Approx(2.0 / 3.0));
    root.distribute = false;
    CHECK(root.distribution(0.5, 3) == Approx(0.5));

    overlap::ChainBias tip;
    tip.tipStiffnessScale = 4.0;
    tip.tipExaggerationScale = 2.0;
    overlap::BoneSettings base;
    overlap::BoneSettings at = overlap::chainBone(base, tip, 1.0, 2);
    CHECK(at.translation.stiffness == Approx(1000.0));
    CHECK(at.rotation.damping == Approx(24.0)); // damping follows sqrt(stiffness)
    CHECK(at.style.exaggeration == Approx(2.0));
    CHECK(overlap::chainBone(base, tip, 0.0, 2).translation.stiffness == Approx(250.0));

    overlap::Global g;
    g.stiffness = 2.0;
    g.maxAngle = 0.5;
    g.weight = 0.25;
    overlap::BoneSettings applied = g.apply(base);
    CHECK(applied.rotation.stiffness == Approx(500.0));
    CHECK(applied.rotation.maxAngle == Approx(15.0));
    CHECK(applied.style.weight == Approx(0.25));
}

TEST_CASE("overlap solver: stretch, rotation vectors and colliders")
{
    CHECK(overlap::saturateStretch(0.0, 0.6, 1.8) == 1.0);
    CHECK(overlap::saturateStretch(100.0, 0.6, 1.8) == Approx(1.8));
    CHECK(overlap::saturateStretch(-100.0, 0.6, 1.8) == Approx(0.6));

    const glm::dvec3 r(0.3, -0.2, 0.9);
    const glm::dvec3 back = overlap::toRotationVector(overlap::fromRotationVector(r));
    CHECK(glm::length(back - r) < 1e-12);
    // The negative hemisphere reads as the short way round.
    CHECK(glm::length(overlap::toRotationVector(-overlap::fromRotationVector(r)) - r) < 1e-12);

    overlap::Collider sphere{overlap::Shape::Sphere, glm::dvec3(0), glm::dvec3(0), 5.0};
    CHECK(overlap::pushOut(glm::dvec3(3, 0, 0), sphere, 1.0).x == Approx(3.0));
    CHECK(overlap::pushOut(glm::dvec3(7, 0, 0), sphere, 1.0) == glm::dvec3(0));
    overlap::Collider capsule{overlap::Shape::Capsule, glm::dvec3(-10, 0, 0), glm::dvec3(10, 0, 0), 2.0};
    glm::dvec3 push = overlap::pushOut(glm::dvec3(4, 1, 0), capsule, 0.0);
    CHECK(push.x == Approx(0.0));
    CHECK(push.y == Approx(1.0));
}

TEST_CASE("overlap extension: chains simulate deterministically and record to a layer")
{
    loadOverlap();
    Graph& g = Graph::current();
    tailScene(false);
    std::string o = run("createOverlap", {{"args", {"tail_01"}}});
    CHECK(o == "root_overlap");
    REQUIRE(g.find("tail_01_chain"));
    const NodeId on = g.find(o)->id();

    json info = run("simulate", {{"args", {o}}, {"frame", 30}, {"positions", true}});
    CHECK(info[0]["error"] == "");
    CHECK(info[0]["positions"].size() == 4);
    // The tail lags the hips: the simulated tip is off its animated place.
    run("currentTime", {{"args", {0.3}}});
    g.evaluate();
    const glm::dvec3 animatedTip(g.find("tail_04")->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
    const glm::dvec3 simTip(sim::simulate(g, on, 9)->world.back()[3]);
    CHECK(glm::length(simTip - animatedTip) > 0.1);

    // The same scene simulates to the same bits, on any number of threads, and undoing a recording restores it.
    sim::clearCache(g);
    const std::vector<glm::dmat4> reference = sim::simulate(g, on, 30)->world;
    auto sameAsReference = [&] {
        sim::clearCache(g);
        const std::vector<glm::dmat4> w = sim::simulate(g, on, 30)->world;
        return std::memcmp(w.data(), reference.data(), w.size() * sizeof(glm::dmat4)) == 0;
    };
    CHECK(sameAsReference());
    {
        tbb::global_control limit(tbb::global_control::max_allowed_parallelism, 1);
        CHECK(sameAsReference());
    }
    // Record twice from the same scene, the second time on one thread: the same bits.
    sim::clearCache(g);
    std::string layer = run("recordSimulation", {{"args", {o}}});
    const std::string first = layerKeys(layer);
    CHECK_FALSE(first.empty());
    run("undo");
    REQUIRE_FALSE(g.find(layer));
    {
        tbb::global_control limit(tbb::global_control::max_allowed_parallelism, 1);
        sim::clearCache(g);
        std::string again = run("recordSimulation", {{"args", {o}}});
        CHECK(again == layer);
        CHECK(layerKeys(again) == first);
    }
    run("undo");
    CHECK(sameAsReference());

    // Recorded, the joints evaluate to the simulation.
    sim::clearCache(g);
    std::vector<glm::dmat4> want = sim::simulate(g, on, 12)->world;
    run("recordSimulation", {{"args", {o}}});
    run("currentTime", {{"args", {12.0 / 30.0}}});
    g.evaluate();
    const sim::Info si = sim::info(g, on);
    for (size_t i = 0; i < si.outputs.size(); ++i)
    {
        const glm::dmat4 got = g.node(si.outputs[i])->as<glm::dmat4>(Transform::kWorldMatrix);
        for (int c = 0; c < 4; ++c)
            CHECK(glm::length(glm::dvec4(got[c] - want[i][c])) < 1e-6);
    }
}

TEST_CASE("overlap extension: scrubbing, pins, colliders, wind and stepped output")
{
    loadOverlap();
    Graph& g = Graph::current();
    tailScene(false);
    run("createOverlap", {{"args", {"tail_01"}}});
    run("setAttr", {{"args", {"root_overlap.windStrength", 300.0}}});
    run("setAttr", {{"args", {"tail_01_chain.delay", 2.0}}});
    run("createOverlapCollider", {{"args", {"root"}}, {"radius", 6.0}});
    run("setAttr", {{"args", {"overlapCollider1.translate", 15.0, 100.0, -30.0}}});
    const NodeId on = g.find("root_overlap")->id();

    sim::clearCache(g);
    std::vector<std::vector<glm::dmat4>> straight;
    for (int64_t f = 0; f <= 30; ++f)
        straight.push_back(sim::simulate(g, on, f)->world);
    sim::clearCache(g);
    for (int64_t f : {17, 3, 29, 11, 30, 0, 22, 8})
    {
        const std::vector<glm::dmat4> w = sim::simulate(g, on, f)->world;
        CHECK(std::memcmp(w.data(), straight[size_t(f)].data(), w.size() * sizeof(glm::dmat4)) == 0);
    }

    // Colliders hold the joints out: nothing ends inside the sphere.
    const glm::dvec3 centre(15.0, 100.0, -30.0);
    for (const auto& frame : straight)
        for (const glm::dmat4& m : frame)
            CHECK(glm::length(glm::dvec3(m[3]) - centre) >= 6.0 + 1.0 - 1e-6);

    // A fully pinned joint stays on its animation.
    run("addOverlapJoint", {{"args", {"tail_02"}}, {"pin", 1.0}});
    run("addOverlapJoint", {{"args", {"tail_01"}}, {"pin", 1.0}});
    run("currentTime", {{"args", {15.0 / 30.0}}});
    g.evaluate();
    const glm::dvec3 anim2(g.find("tail_02")->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
    const sim::Info info = sim::info(g, on);
    const std::vector<glm::dmat4> pinned = sim::simulate(g, on, 15)->world;
    for (size_t i = 0; i < info.outputs.size(); ++i)
        if (g.node(info.outputs[i])->name() == "tail_02")
            CHECK(glm::length(glm::dvec3(pinned[i][3]) - anim2) < 1e-9);

    // On twos: odd frames hold the even frame's pose.
    run("setAttr", {{"args", {"root_overlap.stepped", 2}}});
    const std::vector<glm::dmat4> f10 = sim::simulate(g, on, 10)->world, f11 = sim::simulate(g, on, 11)->world;
    CHECK(std::memcmp(f10.data(), f11.data(), f10.size() * sizeof(glm::dmat4)) == 0);
}

TEST_CASE("overlap extension: a looping take gives a seamless loop")
{
    loadOverlap();
    Graph& g = Graph::current();
    tailScene(true);
    run("createOverlap", {{"args", {"tail_01"}}});
    const NodeId on = g.find("root_overlap")->id();
    sim::clearCache(g);
    std::vector<std::vector<glm::dmat4>> f;
    for (int64_t k = 0; k <= 30; ++k)
        f.push_back(sim::simulate(g, on, k)->world);
    const sim::Info i = sim::info(g, on);
    CHECK(i.loop);
    CHECK(i.loopEnd == 30);
    CHECK(i.converged);
    CHECK(std::memcmp(f.front().data(), f.back().data(), f.front().size() * sizeof(glm::dmat4)) == 0);
    for (size_t j = 0; j < f.front().size(); ++j)
    {
        auto pos = [&](size_t k) { return glm::dvec3(f[k][j][3]); };
        double interior = 0.0;
        for (size_t k = 1; k + 1 < f.size(); ++k)
            interior = std::max(interior, glm::length((pos(k + 1) - pos(k)) - (pos(k) - pos(k - 1))));
        CHECK(glm::length((pos(1) - pos(0)) - (pos(30) - pos(29))) <= 2.0 * interior + 1e-9);
    }

    // The recording of a loop loops.
    std::string layer = run("recordSimulation", {{"args", {"root_overlap"}}});
    const NodeId l = g.find(layer)->id();
    bool anyLoop = false;
    for (const PlugRef& m : anim::layerMembers(g, l))
        for (int k = -1; k < 3; ++k)
            if (auto s = g.source({anim::blendFor(g, m, l), anim::BlendNode::kInputB, k}))
                anyLoop = anyLoop || anim::curveLoops(*g.node(s->node));
    CHECK(anyLoop);
}

TEST_CASE("overlap extension: unloading takes everything it registered")
{
    extensions::ExtensionManager& em = extensions::ExtensionManager::get();
    // An extension whose node types are in the scene can't unload.
    run("newScene");
    if (sim::solverTypes().find("overlap"))
        em.unload("Overlap");
    const json before = uiRegistries();
    REQUIRE(em.load(overlapPackage()) == "Overlap");
    const json loaded = uiRegistries();
    for (const char* k : {"actions", "drawOverrides", "items", "panels", "types", "commands", "solvers"})
        CHECK_MESSAGE(loaded[k] != before[k], k);
    CHECK(sim::solverTypes().find("overlap"));
    em.unload("Overlap");
    CHECK(uiRegistries() == before);
    CHECK_FALSE(cmds::find("createOverlap"));
}
