#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "core/PrefStore.h"
#include "extensions/ExtensionManager.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/DrawOverrides.h"
#include "ui/Menus.h"
#include "ui/Panels.h"
#include "ui/SelectionHandlers.h"
#include "ui/ViewportTool.h"

#include <tbb/global_control.h>

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstring>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

void loadPhysics()
{
    if (!sim::solverTypes().find("physicsSolver"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_PHYSICS_PACKAGE) == "PhysicsAnimation");
}

/** A horizontal chain along +X from root at height 100: root, a, b, c, 15 cm apart; root keyed side to side when animated. */
void chainScene(bool animated, bool loop = false)
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 1.0}, {"fps", 30.0}});
    run("joint", {{"position", v3(0, 100, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(15, 100, 0)}, {"name", "a"}});
    run("joint", {{"position", v3(30, 100, 0)}, {"name", "b"}});
    run("joint", {{"position", v3(45, 100, 0)}, {"name", "c"}});
    if (animated)
    {
        const double zs[] = {0, 30, 0, -30, 0};
        for (int k = 0; k < 5; ++k)
            run("setKeyframe", {{"args", {"root.tz"}}, {"time", k * 7.5 / 30.0}, {"value", zs[k]}});
    }
    if (loop)
        run("setLoopKey", {{"args", {"root"}}, {"time", 1.0}});
}

/** Markers on the chain: root kinematic, the rest with the given behaviour. Returns the solver. */
NodeId rig(const char* behaviour)
{
    json markers = run("assignPhysicsMarker", {{"args", {"root", "a", "b", "c"}}, {"behaviour", behaviour}, {"fitLimits", false}});
    REQUIRE(markers.size() == 4);
    run("setAttr", {{"args", {"root_marker.behaviour", 0}}});
    Graph& g = Graph::current();
    const NodeId s = g.find("root_physics")->id();
    return s;
}

glm::dvec3 worldPos(const char* name)
{
    Graph& g = Graph::current();
    g.evaluate();
    return glm::dvec3(g.find(name)->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
}

std::string layerKeys(const std::string& layer)
{
    Graph& g = Graph::current();
    const NodeId l = g.find(layer)->id();
    std::string out;
    for (const PlugRef& m : anim::layerMembers(g, l))
    {
        const NodeId b = anim::blendFor(g, m, l);
        for (int k = -1; k < 3; ++k)
            if (auto s = g.source({b, anim::BlendNode::kInputB, k}))
            {
                const Node& c = *g.node(s->node);
                out += g.plugName(m) + ":" + c.as<std::string>(c.type().attrIndex("keys")) + "\n";
            }
    }
    return out;
}

double angleDeg(glm::dvec3 a, glm::dvec3 b)
{
    return std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
}
} // namespace

TEST_CASE("physics: a ragdoll records the same keys every time, on any number of threads")
{
    loadPhysics();
    Graph& g = Graph::current();
    chainScene(true);
    const NodeId s = rig("simulated");
    sim::clearCache(g);
    const std::string layer = run("recordPhysics", {{"args", {"root_physics"}}});
    const std::string first = layerKeys(layer);
    CHECK_FALSE(first.empty());
    run("undo");
    REQUIRE_FALSE(g.find(layer));
    {
        tbb::global_control limit(tbb::global_control::max_allowed_parallelism, 1);
        sim::clearCache(g);
        const std::string again = run("recordPhysics", {{"args", {"root_physics"}}});
        CHECK(layerKeys(again) == first);
        run("undo");
    }
    {
        tbb::global_control limit(tbb::global_control::max_allowed_parallelism, 3);
        sim::clearCache(g);
        const std::string again = run("recordPhysics", {{"args", {"root_physics"}}});
        CHECK(layerKeys(again) == first);
    }
    // The chain swung and sagged: the tip is far from where it is animated.
    sim::clearCache(g);
    const std::vector<glm::dmat4> w = sim::simulate(g, s, 30)->world;
    CHECK(glm::length(glm::dvec3(w.back()[3]) - glm::dvec3(45, 100, 0)) > 10.0);
    CHECK(w.back()[3].y < 99.0);
}

TEST_CASE("physics: scrubbing gives the same frames as playing straight through")
{
    loadPhysics();
    Graph& g = Graph::current();
    chainScene(true);
    const NodeId s = rig("poseMatch");
    run("setAttr", {{"args", {"b_marker.behaviour", 2}}});
    sim::clearCache(g);
    std::vector<std::vector<glm::dmat4>> straight;
    for (int64_t f = 0; f <= 30; ++f)
        straight.push_back(sim::simulate(g, s, f)->world);
    sim::clearCache(g);
    for (int64_t f : {17, 3, 29, 11, 30, 0, 22, 8, 9, 10})
    {
        const std::vector<glm::dmat4> w = sim::simulate(g, s, f)->world;
        CHECK_MESSAGE(std::memcmp(w.data(), straight[size_t(f)].data(), w.size() * sizeof(glm::dmat4)) == 0, f);
    }
    // A cache emptied mid-way and stepped on from a cached frame agrees too.
    sim::clearCache(g);
    sim::simulate(g, s, 12);
    const std::vector<glm::dmat4> w = sim::simulate(g, s, 25)->world;
    CHECK(std::memcmp(w.data(), straight[25].data(), w.size() * sizeof(glm::dmat4)) == 0);
}

TEST_CASE("physics: a looping take loops, or says it did not settle")
{
    loadPhysics();
    Graph& g = Graph::current();
    chainScene(true, true);
    const NodeId s = rig("poseMatch");
    sim::clearCache(g);
    std::vector<std::vector<glm::dmat4>> f;
    for (int64_t k = 0; k <= 30; ++k)
        f.push_back(sim::simulate(g, s, k)->world);
    const sim::Info i = sim::info(g, s);
    CHECK(i.loop);
    CHECK(i.loopEnd == 30);
    CHECK(std::memcmp(f.front().data(), f.back().data(), f.front().size() * sizeof(glm::dmat4)) == 0);
    if (!i.converged)
        CHECK(i.residual > 1.0);
    MESSAGE("loop converged: " << i.converged << " after " << i.passes << " passes, residual " << i.residual);
}

TEST_CASE("physics: pose match at high stiffness tracks a kinematic animation")
{
    loadPhysics();
    Graph& g = Graph::current();
    chainScene(true);
    const NodeId s = rig("poseMatch");
    for (const char* m : {"a_marker", "b_marker", "c_marker"})
    {
        run("setAttr", {{"args", {std::string(m) + ".poseStiffness", 25.0}}});
        run("setAttr", {{"args", {std::string(m) + ".poseSpace", 2}}});
    }
    sim::clearCache(g);
    double worst = 0.0;
    for (int64_t f = 0; f <= 30; ++f)
    {
        const std::vector<glm::dmat4> w = sim::simulate(g, s, f)->world;
        const sim::Info info = sim::info(g, s);
        run("currentTime", {{"args", {double(f) / 30.0}}});
        for (size_t k = 0; k < info.outputs.size(); ++k)
            worst = std::max(worst, glm::length(glm::dvec3(w[k][3]) - worldPos(g.node(info.outputs[k])->name().c_str())));
    }
    MESSAGE("worst pose match error " << worst << " cm");
    CHECK(worst < 2.0);
}

TEST_CASE("physics: joint limits hold a falling chain")
{
    loadPhysics();
    Graph& g = Graph::current();
    chainScene(false);
    const NodeId s = rig("simulated");
    for (const char* m : {"a_marker", "b_marker", "c_marker"})
        for (const char* l : {".limitTwist", ".limitSwing1", ".limitSwing2"})
            run("setAttr", {{"args", {std::string(m) + l, 15.0}}});
    sim::clearCache(g);
    double worst = 0.0;
    for (int64_t f = 0; f <= 30; ++f)
    {
        const std::vector<glm::dmat4> w = sim::simulate(g, s, f)->world;
        // Each bone (joint to its child) against its parent's bone, which at rest is straight along X.
        for (size_t k = 1; k + 1 < w.size(); ++k)
        {
            const glm::dvec3 parentBone = glm::dvec3(w[k][3]) - glm::dvec3(w[k - 1][3]);
            const glm::dvec3 bone = glm::dvec3(w[k + 1][3]) - glm::dvec3(w[k][3]);
            worst = std::max(worst, angleDeg(parentBone, bone));
        }
    }
    MESSAGE("worst bend " << worst << " degrees");
    CHECK(worst < 15.0 + 4.0);
    // Unlimited, the same chain folds far past that.
    for (const char* m : {"a_marker", "b_marker", "c_marker"})
        run("setAttr", {{"args", {std::string(m) + ".limitEnabled", false}}});
    sim::clearCache(g);
    double bent = 0.0;
    for (int64_t f = 0; f <= 30; ++f)
    {
        const std::vector<glm::dmat4> w = sim::simulate(g, s, f)->world;
        bent = std::max(bent, angleDeg(glm::dvec3(w[1][3]) - glm::dvec3(w[0][3]), glm::dvec3(w[2][3]) - glm::dvec3(w[1][3])));
    }
    CHECK(bent > 30.0);
}

TEST_CASE("physics: snapping keys the joints to the simulated pose")
{
    loadPhysics();
    Graph& g = Graph::current();
    chainScene(false);
    const NodeId s = rig("simulated");
    sim::clearCache(g);
    const std::vector<glm::dmat4> want = sim::simulate(g, s, 15)->world;
    run("currentTime", {{"args", {0.5}}});
    json keyed = run("snapToSimulation", {{"args", {"root_physics"}}, {"frame", 15}});
    CHECK(keyed.size() == 4);
    const sim::Info info = sim::info(g, s);
    g.evaluate();
    for (size_t k = 0; k < info.outputs.size(); ++k)
        CHECK(glm::length(glm::dvec3(g.node(info.outputs[k])->as<glm::dmat4>(Transform::kWorldMatrix)[3]) - glm::dvec3(want[k][3])) < 1e-6);
}

TEST_CASE("physics: clicking a body selects its joint")
{
    loadPhysics();
    Graph& g = Graph::current();
    chainScene(false);
    rig("poseMatch");
    ui::selectionHandlers::Hit hit;
    hit.node = g.find("b_marker")->id();
    hit.name = "b_marker";
    CHECK(ui::selectionHandlers::anyRedirect());
    CHECK(ui::selectionHandlers::redirect({hit}) == std::vector<std::string>{"b"});
    // Off, the marker itself is selected; other hits are never touched.
    PrefStore::get().set("general", "/physics/proxySelectsJoint", false);
    CHECK(ui::selectionHandlers::redirect({hit}) == std::vector<std::string>{"b_marker"});
    PrefStore::get().set("general", "/physics/proxySelectsJoint", true);
    ui::selectionHandlers::Hit other;
    other.node = g.find("a")->id();
    other.name = "a";
    CHECK(ui::selectionHandlers::redirect({other}) == std::vector<std::string>{"a"});
}

TEST_CASE("physics: a labelled skeleton gets a ragdoll in one go, and a marker can be retargeted")
{
    loadPhysics();
    Graph& g = Graph::current();
    run("newScene");
    run("joint", {{"position", v3(0, 100, 0)}, {"name", "pelvis"}});
    run("joint", {{"position", v3(0, 115, 0)}, {"name", "spine_01"}});
    run("joint", {{"position", v3(0, 130, 0)}, {"name", "neck_01"}});
    run("joint", {{"position", v3(0, 145, 0)}, {"name", "head"}});
    run("joint", {{"position", v3(0, 160, 0)}, {"name", "head_end"}});
    json made = run("physicsMarkersFromTemplate", {{"args", {"spine_01"}}});
    CHECK(made.size() == 4);
    json info = run("physicsMarkerInfo", {{"args", {"head"}}});
    CHECK(info[0]["label"] == "head");
    CHECK(info[0]["shape"] == 2);
    CHECK(info[0]["parent"] == "neck_01_marker");
    run("retargetPhysicsMarker", {{"args", {"head_marker", "head_end"}}});
    CHECK(run("getAttr", {{"args", {"head_marker.joint"}}}) == "head_end");
    run("undo");
    CHECK(run("getAttr", {{"args", {"head_marker.joint"}}}) == "head");
    CHECK(g.find("pelvis_physics"));
}

TEST_CASE("physics: unloading takes everything it registered")
{
    extensions::ExtensionManager& em = extensions::ExtensionManager::get();
    run("newScene");
    if (sim::solverTypes().find("physicsSolver"))
        em.unload("PhysicsAnimation");
    auto registries = [] {
        return json{{"actions", ui::actions::registry().ids()},         {"drawOverrides", ui::drawOverrides::registry().ids()},
                    {"items", ui::menus::itemRegistry().ids()},         {"panels", ui::panels::registry().ids()},
                    {"types", types::registry().ids()},                 {"commands", cmds::registry().ids()},
                    {"solvers", sim::solverTypes().ids()},              {"handlers", ui::selectionHandlers::registry().ids()},
                    {"tools", ui::viewportTools::registry().ids()}};
    };
    const json before = registries();
    REQUIRE(em.load(LILA_TEST_PHYSICS_PACKAGE) == "PhysicsAnimation");
    const json loaded = registries();
    for (const char* k : {"actions", "drawOverrides", "items", "panels", "types", "commands", "solvers", "handlers", "tools"})
        CHECK_MESSAGE(loaded[k] != before[k], k);
    em.unload("PhysicsAnimation");
    CHECK(registries() == before);
    CHECK_FALSE(cmds::find("assignPhysicsMarker"));
    CHECK_FALSE(ui::selectionHandlers::anyRedirect());
}
