#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/Simulation.h"
#include "scene/SplineChain.h"
#include "scene/Transform.h"

#include <tbb/global_control.h>

#include <cmath>
#include <cstring>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;

namespace
{
json sdRun(const char* name, json call = json::object()) { return ops::execute(name, call); }
json sdV3(double x, double y, double z) { return json::array({x, y, z}); }

void sdLoad()
{
    curve::registerTypes();
    if (!ops::find("curve"))
        curve::registerCommands();
    extensions::ExtensionManager& em = extensions::ExtensionManager::get();
    if (!sim::solverTypes().find("overlap"))
        REQUIRE(em.load(LILA_TEST_OVERLAP_PACKAGE) == "Overlap");
    if (!sim::solverTypes().find("physicsSolver"))
        REQUIRE(em.load(LILA_TEST_PHYSICS_PACKAGE) == "PhysicsAnimation");
}

enum class Motion
{
    Still,
    Swing, // hips turn 60 degrees about Z over frames 0..10 and hold
    Loop,  // hips swing side to side, looping over 0..30
    Carry, // a carrier under the hips accelerates along X
};

/**
 * Hips at (0, 100, 0), a five-joint tail up +Y on a spline chain, a cylinder skinned to it. Returns the chain. The tail
 * tip is tail4, 40 cm up.
 */
std::string sdTail(Motion motion)
{
    sdRun("newScene");
    sdRun("autoKeyframe", {{"state", false}});
    sdRun("playbackOptions", {{"minTime", 0.0}, {"maxTime", 1.0}, {"fps", 30.0}});
    if (motion == Motion::Carry)
    {
        sdRun("joint", {{"position", sdV3(0, 0, 0)}, {"name", "carrier"}});
        const double xs[] = {0, 0, 60, 60};
        const double ts[] = {0, 5, 15, 30};
        for (int k = 0; k < 4; ++k)
            sdRun("setKeyframe", {{"args", {"carrier.tx"}}, {"time", ts[k] / 30.0}, {"value", xs[k]}});
    }
    else
        sdRun("select", {{"args", json::array()}});
    sdRun("joint", {{"position", sdV3(0, 100, 0)}, {"name", "hips"}});
    for (int i = 0; i < 5; ++i)
        sdRun("joint", {{"position", sdV3(0, 110 + 10.0 * i, 0)}, {"name", "tail" + std::to_string(i)}});
    if (motion == Motion::Swing)
    {
        sdRun("setKeyframe", {{"args", {"hips.rz"}}, {"time", 0.0}, {"value", 0.0}});
        sdRun("setKeyframe", {{"args", {"hips.rz"}}, {"time", 10.0 / 30.0}, {"value", 60.0}});
        sdRun("setKeyframe", {{"args", {"hips.rz"}}, {"time", 1.0}, {"value", 60.0}});
    }
    if (motion == Motion::Loop)
    {
        const double zs[] = {0, 30, 0, -30, 0};
        for (int k = 0; k < 5; ++k)
            sdRun("setKeyframe", {{"args", {"hips.rz"}}, {"time", k * 7.5 / 30.0}, {"value", zs[k]}});
        sdRun("setLoopKey", {{"args", {"hips"}}, {"time", 1.0}});
    }
    const std::string mesh = sdRun("polyCylinder", {{"name", "TailSkin"}, {"radius", 3}, {"height", 40}, {"subdivisionsHeight", 8}, {"caps", false}})[0];
    sdRun("move", {{"args", {mesh}}, {"by", sdV3(0, 130, 0)}});
    sdRun("skinCluster", {{"args", {"hips", mesh}}});
    const std::string chain = sdRun("splineChain", {{"args", {"tail0", "tail4"}}, {"lengthMode", "keepLength"}, {"controls", 3}}).get<std::string>();
    // The curve's controls ride the hips, as a rig's would.
    Graph& g = Graph::current();
    const Node& c = *g.find(chain);
    sdRun("parent", {{"args", {g.find(c.as<std::string>(c.type().attrIndex("curve")))->name() + "_controls", "hips"}}});
    return chain;
}

glm::dvec3 sdPos(const std::string& name)
{
    Graph& g = Graph::current();
    g.evaluate();
    return glm::dvec3(g.find(name)->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
}

void sdAt(int64_t frame) { sdRun("currentTime", {{"args", {double(frame) / 30.0}}}); }

/** Every simulated frame 0..30. */
std::vector<std::vector<glm::dmat4>> sdFrames(NodeId s)
{
    Graph& g = Graph::current();
    std::vector<std::vector<glm::dmat4>> out;
    for (int64_t f = 0; f <= 30; ++f)
        out.push_back(sim::simulate(g, s, f)->world);
    return out;
}

bool sdSame(const std::vector<std::vector<glm::dmat4>>& a, const std::vector<std::vector<glm::dmat4>>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t f = 0; f < a.size(); ++f)
        if (a[f].size() != b[f].size() || std::memcmp(a[f].data(), b[f].data(), a[f].size() * sizeof(glm::dmat4)) != 0)
            return false;
    return true;
}

size_t sdOutput(NodeId s, const char* joint)
{
    Graph& g = Graph::current();
    const sim::Info i = sim::info(g, s);
    for (size_t k = 0; k < i.outputs.size(); ++k)
        if (g.node(i.outputs[k])->name() == joint)
            return k;
    FAIL("no output " << joint);
    return 0;
}

NodeId sdDynamics(const std::string& chain, const char* mode, json flags = json::object())
{
    flags["args"] = {chain};
    flags["mode"] = mode;
    const json made = sdRun("splineDynamics", flags);
    REQUIRE(made.size() == 1);
    Graph& g = Graph::current();
    CHECK(spline::dynamicsOf(g, g.find(chain)->id()) == g.find(made[0].get<std::string>())->id());
    return g.find(made[0].get<std::string>())->id();
}

/** Same frames on 1, 3 and every thread, from a cold cache each time. */
void sdCheckDeterministic(NodeId s)
{
    Graph& g = Graph::current();
    sim::clearCache(g);
    const auto first = sdFrames(s);
    for (int threads : {1, 3})
    {
        tbb::global_control limit(tbb::global_control::max_allowed_parallelism, size_t(threads));
        sim::clearCache(g);
        CHECK_MESSAGE(sdSame(sdFrames(s), first), threads << " threads");
    }
}

/** Follow-through on a swing toward -X: the tip trails while the hips speed up and runs on past them once they stop. */
void sdCheckFollowThrough(NodeId s)
{
    Graph& g = Graph::current();
    sim::clearCache(g);
    const auto frames = sdFrames(s);
    const size_t tip = sdOutput(s, "tail4");
    double trail = 0.0, overshoot = 0.0;
    for (int64_t f = 0; f <= 30; ++f)
    {
        sdAt(f);
        const double d = frames[size_t(f)][tip][3].x - sdPos("tail4").x;
        if (f <= 6)
            trail = std::max(trail, d);
        if (f >= 10 && f <= 18)
            overshoot = std::min(overshoot, d);
    }
    MESSAGE("trail " << trail << " cm, overshoot " << overshoot << " cm");
    CHECK(trail > 0.3);
    CHECK(overshoot < -0.3);
}

/** With nothing moving the simulated chain sits on the animated one. */
void sdCheckStill(NodeId s, double tolerance)
{
    Graph& g = Graph::current();
    sim::clearCache(g);
    const auto frames = sdFrames(s);
    const sim::Info info = sim::info(g, s);
    double worst = 0.0;
    for (size_t k = 0; k < info.outputs.size(); ++k)
    {
        const glm::dvec3 anim = sdPos(g.node(info.outputs[k])->name());
        for (const auto& f : frames)
            worst = std::max(worst, glm::length(glm::dvec3(f[k][3]) - anim));
    }
    MESSAGE("still: worst " << worst << " cm");
    CHECK(worst < tolerance);
}

/** Records onto the chain's offsets: the joints land where the simulation put them, and the bake passes unrealCheck. */
void sdCheckRecordAndBake(NodeId s, double tolerance)
{
    Graph& g = Graph::current();
    sim::clearCache(g);
    const auto frames = sdFrames(s);
    const std::string layer = sdRun("recordSimulation", {{"args", {g.node(s)->path()}}}).get<std::string>();
    bool offsets = false;
    for (const PlugRef& m : anim::layerMembers(g, g.find(layer)->id()))
        offsets = offsets || (g.node(m.node)->type().name == spline::kJointType && g.attr(m).name == "offset");
    CHECK(offsets);
    const sim::Info info = sim::info(g, s);
    double worst = 0.0;
    for (int64_t f : {3, 9, 14, 22, 30})
    {
        sdAt(f);
        for (size_t k = 0; k < info.outputs.size(); ++k)
            worst = std::max(worst, glm::length(glm::dvec3(frames[size_t(f)][k][3]) - sdPos(g.node(info.outputs[k])->name())));
    }
    MESSAGE("recorded: worst " << worst << " cm");
    CHECK(worst < tolerance);

    const json baked = sdRun("bakeToDeformSkeleton", {{"args", {"hips"}}, {"start", 0.0}, {"end", 1.0}, {"fps", 30.0}});
    REQUIRE(baked.size() == 1);
    sdAt(14);
    CHECK(glm::length(sdPos("tail4") - sdPos("tail4_bake")) < 1e-4);
    const json check = sdRun("unrealCheck", {{"args", {baked[0]}}, {"start", 0.0}, {"end", 1.0}});
    CHECK(check["skins"] == 1);
    CHECK(check["mismatches"].empty());
}

void sdCheckLoop(NodeId s)
{
    Graph& g = Graph::current();
    sim::clearCache(g);
    const auto frames = sdFrames(s);
    const sim::Info i = sim::info(g, s);
    CHECK(i.loop);
    CHECK(i.loopEnd == 30);
    CHECK(std::memcmp(frames.front().data(), frames.back().data(), frames.front().size() * sizeof(glm::dmat4)) == 0);
    MESSAGE("loop converged: " << i.converged << " after " << i.passes << " passes, residual " << i.residual);
    // The recording loops too: the last frame poses the joints as the first does.
    sdRun("recordSimulation", {{"args", {g.node(s)->path()}}});
    sdAt(0);
    const glm::dvec3 a = sdPos("tail4");
    sdAt(30);
    CHECK(glm::length(sdPos("tail4") - a) < 1e-6);
}
} // namespace

TEST_CASE("spline dynamics: overlap follows through, deterministically, and bakes for Unreal")
{
    sdLoad();
    const std::string chain = sdTail(Motion::Swing);
    const NodeId s = sdDynamics(chain, "overlap");
    sdCheckDeterministic(s);
    sdCheckFollowThrough(s);
    sdCheckRecordAndBake(s, 1e-3);
}

TEST_CASE("spline dynamics: jolt follows through, deterministically, and bakes for Unreal")
{
    sdLoad();
    const std::string chain = sdTail(Motion::Swing);
    const NodeId s = sdDynamics(chain, "jolt");
    sdCheckDeterministic(s);
    sdCheckFollowThrough(s);
    // Rotation is what records: the bodies' small give at the joints is not kept.
    sdCheckRecordAndBake(s, 1.0);
}

TEST_CASE("spline dynamics: additive dynamics add nothing to a still chain")
{
    sdLoad();
    sdCheckStill(sdDynamics(sdTail(Motion::Still), "overlap"), 1e-6);
    sdCheckStill(sdDynamics(sdTail(Motion::Still), "jolt"), 0.05);
}

TEST_CASE("spline dynamics: a looping swing loops without a seam")
{
    sdLoad();
    sdCheckLoop(sdDynamics(sdTail(Motion::Loop), "overlap"));
    sdCheckLoop(sdDynamics(sdTail(Motion::Loop), "jolt"));
}

TEST_CASE("spline dynamics: a carried chain feels the carrier's acceleration by its inertia share")
{
    sdLoad();
    for (const char* mode : {"overlap", "jolt"})
    {
        double worst[2] = {0.0, 0.0};
        for (int k = 0; k < 2; ++k)
        {
            const std::string chain = sdTail(Motion::Carry);
            const NodeId s = sdDynamics(chain, mode, {{"carrier", "carrier"}, {"carrierInertia", k == 0 ? 0.0 : 1.0}});
            Graph& g = Graph::current();
            sim::clearCache(g);
            const auto frames = sdFrames(s);
            const size_t tip = sdOutput(s, "tail4");
            for (int64_t f = 0; f <= 30; ++f)
            {
                sdAt(f);
                worst[k] = std::max(worst[k], glm::length(glm::dvec3(frames[size_t(f)][tip][3]) - sdPos("tail4")));
            }
        }
        MESSAGE(std::string(mode) << ": carried rigidly " << worst[0] << " cm, full inertia " << worst[1] << " cm");
        CHECK(worst[1] > 1.0);
        CHECK(worst[0] < worst[1] * 0.25);
    }
}

TEST_CASE("spline dynamics: one simulation per chain, and a hose limb takes them too")
{
    sdLoad();
    const std::string chain = sdTail(Motion::Still);
    sdDynamics(chain, "overlap");
    CHECK_THROWS_AS(sdRun("splineDynamics", {{"args", {chain}}, {"mode", "jolt"}}), CommandError);
    CHECK_THROWS_AS(sdRun("splineDynamics", {{"args", {"tail2"}}, {"mode", "nope"}}), CommandError);

    sdRun("newScene");
    sdRun("select", {{"args", json::array()}});
    sdRun("joint", {{"position", sdV3(0, 150, 0)}, {"name", "shoulder"}});
    sdRun("joint", {{"position", sdV3(25, 150, 0)}, {"name", "wrist"}});
    const std::string hose = sdRun("hoseLimb", {{"args", {"shoulder", "wrist"}}, {"joints", 8}}).get<std::string>();
    const json made = sdRun("splineDynamics", {{"args", {hose}}, {"mode", "overlap"}});
    REQUIRE(made.size() == 1);
    Graph& g = Graph::current();
    CHECK(spline::dynamicsOf(g, spline::hoseChain(g, g.find(hose)->id())) == g.find(made[0].get<std::string>())->id());
}

TEST_CASE("spline dynamics: a fully simulated chain falls from its root under gravity")
{
    sdLoad();
    for (const char* mode : {"overlap", "jolt"})
    {
        const std::string chain = sdTail(Motion::Still);
        // Leaning, so gravity has a side to pull it to: an upright chain balances.
        sdRun("setAttr", {{"args", {"hips.rotateZ", 45.0}}});
        const NodeId s = sdDynamics(chain, mode, {{"drive", "full"}});
        Graph& g = Graph::current();
        sim::clearCache(g);
        const std::vector<glm::dmat4> w = sim::simulate(g, s, 30)->world;
        const double drop = sdPos("tail4").y - w[sdOutput(s, "tail4")][3].y;
        MESSAGE(std::string(mode) << ": the tip fell " << drop << " cm");
        CHECK(drop > 5.0);
        CHECK(glm::length(glm::dvec3(w[sdOutput(s, "tail0")][3]) - sdPos("tail0")) < 0.5);
    }
}
