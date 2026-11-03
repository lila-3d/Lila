#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/PrefStore.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"

#include <tbb/global_control.h>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <random>

using namespace lila;
using namespace lila::scene;
using namespace simtest;
using json = nlohmann::json;

namespace
{
/** A point on a damped spring chasing a locator, plus seeded noise: small enough to reason about, real enough to test. */
class SpringSolver final : public sim::Solver
{
public:
    explicit SpringSolver(NodeId node) : mNode(node) {}

    sim::Setup configure(Graph& g) override
    {
        const Node& n = *g.node(mNode);
        auto num = [&](const char* a) { return n.as<double>(n.type().attrIndex(a)); };
        Node* target = g.find(n.as<std::string>(n.type().attrIndex("target")));
        Node* follower = g.find(n.as<std::string>(n.type().attrIndex("follower")));
        if (!target || !follower)
            throw std::invalid_argument("target and follower must exist");
        mTarget = target->id();
        mStiffness = num("stiffness");
        mDamping = num("damping");
        mNoise = num("noise");
        sim::Setup s;
        s.startFrame = n.as<int64_t>(n.type().attrIndex("startFrame"));
        s.substeps = int(n.as<int64_t>(n.type().attrIndex("substeps")));
        s.stepped = int(n.as<int64_t>(n.type().attrIndex("stepped")));
        s.layout = {{sim::Channel::Position, 3}, {sim::Channel::Velocity, 3}};
        s.outputs = {follower->id()};
        s.sources = {mTarget};
        double params[] = {mStiffness, mDamping, mNoise};
        s.paramsHash = sim::hashBytes(params, sizeof params);
        return s;
    }

    void sample(Graph& g, std::vector<double>& in) override
    {
        const glm::dmat4 w = g.node(mTarget)->as<glm::dmat4>(Transform::kWorldMatrix);
        in.insert(in.end(), {w[3].x, w[3].y, w[3].z});
    }

    void reset(const std::vector<double>& in, std::vector<double>& state) override
    {
        std::copy(in.begin(), in.begin() + 3, state.begin());
        std::fill(state.begin() + 3, state.end(), 0.0);
    }

    void step(const std::vector<double>& in, std::vector<double>& s, const sim::StepInfo& at) override
    {
        const double h = at.dt;
        for (int i = 0; i < 3; ++i)
        {
            const double force = mNoise * sim::noise(7 + uint64_t(i), at.seconds * 3.0);
            const double x = s[size_t(i)] - in[size_t(i)];
            s[size_t(3 + i)] = (s[size_t(3 + i)] + (force - mStiffness * x) * h) / (1.0 + h * mDamping + h * h * mStiffness);
            s[size_t(i)] += s[size_t(3 + i)] * h;
        }
    }

    void pose(const std::vector<double>&, const std::vector<double>& s, std::vector<glm::dmat4>& world) override
    {
        glm::dmat4 m(1.0);
        m[3] = glm::dvec4(s[0], s[1], s[2], 1.0);
        world = {m};
    }

private:
    NodeId mNode;
    NodeId mTarget = kNoNode;
    double mStiffness = 0.0, mDamping = 0.0, mNoise = 0.0;
};

void registerTestSolver()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    const NodeType* xf = types::find("transform");
    NodeType t{.name = "testSpring", .base = xf, .dag = true, .create = xf->create};
    const uint32_t s = AttrInput | AttrStorable;
    t.ownAttrs = {
        {.name = "target", .type = PlugType::String, .defaultValue = std::string("target"), .flags = s},
        {.name = "follower", .type = PlugType::String, .defaultValue = std::string("follower"), .flags = s},
        {.name = "stiffness", .type = PlugType::Float, .defaultValue = 120.0, .flags = s},
        {.name = "damping", .type = PlugType::Float, .defaultValue = 6.0, .flags = s},
        {.name = "noise", .type = PlugType::Float, .defaultValue = 40.0, .flags = s},
        {.name = "startFrame", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = s},
        {.name = "substeps", .type = PlugType::Int, .defaultValue = int64_t(3), .flags = s},
        {.name = "stepped", .type = PlugType::Int, .defaultValue = int64_t(1), .flags = s},
        {.name = sim::kRecordingsAttr, .type = PlugType::String, .defaultValue = std::string(), .flags = s | AttrHidden},
    };
    types::registerType(std::move(t));
    sim::registerSolverType({.nodeType = "testSpring", .create = [](Graph&, NodeId n) -> std::unique_ptr<sim::Solver> {
                                 return std::make_unique<SpringSolver>(n);
                             }});
}

/** target keyed back and forth over frames 0..40 (30 fps), a follower, and the simulation node. */
NodeId springScene(bool loop = false)
{
    registerTestSolver();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 40.0 / 30.0}, {"fps", 30.0}});
    run("spaceLocator", {{"name", "target"}});
    run("spaceLocator", {{"name", "follower"}});
    const double xs[] = {0, 30, -10, 25, 0, 30};
    for (int k = 0; k < 6; ++k)
        run("setKeyframe", {{"args", {"target.tx"}}, {"time", k * 8.0 / 30.0}, {"value", xs[k]}});
    if (loop)
        run("setLoopKey", {{"args", {"target"}}, {"time", 40.0 / 30.0}});
    Graph& g = Graph::current();
    return g.createNode(*types::find("testSpring"), "spring")->id();
}

std::vector<glm::dmat4> worldsAt(Graph& g, NodeId n, int64_t f) { return sim::simulate(g, n, f)->world; }

bool same(const std::vector<glm::dmat4>& a, const std::vector<glm::dmat4>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(glm::dmat4)) == 0;
}

glm::dvec3 posAt(Graph& g, NodeId n, int64_t f) { return glm::dvec3(worldsAt(g, n, f)[0][3]); }
} // namespace

TEST_CASE("simulation: a frame has one answer however it is reached")
{
    Graph& g = Graph::current();
    NodeId n = springScene();
    sim::clearCache(g);
    std::vector<std::vector<glm::dmat4>> straight;
    for (int64_t f = 0; f <= 40; ++f)
        straight.push_back(worldsAt(g, n, f));
    // The simulation really moves off the target.
    CHECK(std::abs(straight[12][0][3].x - std::get<glm::dvec3>(g.value({g.find("target")->id(), Transform::kTranslate}))[0]) > 1e-3);
    CHECK(sim::info(g, n).steps == 40 * 3);

    // Random scrubbing from an empty cache, then play: identical bits.
    sim::clearCache(g);
    std::vector<int64_t> order(41);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937(1234));
    for (int64_t f : order)
        CHECK(same(worldsAt(g, n, f), straight[size_t(f)]));
    for (int64_t f = 0; f <= 40; ++f)
        CHECK(same(worldsAt(g, n, f), straight[size_t(f)]));

    // Different TBB thread counts evaluate the same graph to the same simulation.
    for (int threads : {1, 2, 8})
    {
        tbb::global_control limit(tbb::global_control::max_allowed_parallelism, size_t(threads));
        sim::clearCache(g);
        CHECK(same(worldsAt(g, n, 40), straight[40]));
    }

    // Before the start frame the first frame stands in; the time is put back after simulating.
    run("currentTime", {{"args", {0.5}}});
    CHECK(same(worldsAt(g, n, -5), straight[0]));
    CHECK(g.time() == 0.5);
}

TEST_CASE("simulation: an edit invalidates from the first frame it touches")
{
    Graph& g = Graph::current();
    NodeId n = springScene();
    sim::clearCache(g);
    worldsAt(g, n, 40);
    const int64_t before = sim::info(g, n).steps;

    // Moving the last key only changes the curve after the key before it (frame 32).
    run("setKeyframe", {{"args", {"target.tx"}}, {"time", 40.0 / 30.0}, {"value", -20.0}});
    std::vector<glm::dmat4> edited = worldsAt(g, n, 40);
    sim::Info i = sim::info(g, n);
    CHECK(i.lastInvalidated > 16);
    CHECK(i.lastInvalidated <= 33);
    CHECK(i.steps - before == (40 - i.lastInvalidated + 1) * 3);

    sim::clearCache(g);
    CHECK(same(worldsAt(g, n, 40), edited));

    // A parameter change throws the whole cache away.
    run("setAttr", {{"args", {"spring.damping", 9.0}}});
    worldsAt(g, n, 10);
    CHECK(sim::info(g, n).steps == 10 * 3);
}

TEST_CASE("simulation: loops repeat across the seam, or say they did not settle")
{
    Graph& g = Graph::current();
    NodeId n = springScene(true);
    sim::clearCache(g);
    std::vector<glm::dvec3> p;
    for (int64_t f = 0; f <= 40; ++f)
        p.push_back(posAt(g, n, f));
    sim::Info i = sim::info(g, n);
    CHECK(i.loop);
    CHECK(i.loopStart == 0);
    CHECK(i.loopEnd == 40);
    CHECK(i.converged);
    CHECK(i.passes >= 2);
    CHECK(std::memcmp(&p.front(), &p.back(), sizeof(glm::dvec3)) == 0);
    // Velocity is continuous across the seam: no jump bigger than the motion's own frame-to-frame change.
    double interior = 0.0;
    for (size_t f = 1; f + 1 < p.size(); ++f)
        interior = std::max(interior, glm::length((p[f + 1] - p[f]) - (p[f] - p[f - 1])));
    CHECK(glm::length((p[1] - p[0]) - (p[40] - p[39])) <= 2.0 * interior);
    // Frames past the end wrap into the period.
    CHECK(posAt(g, n, 45) == p[5]);

    // Barely damped and allowed two passes, it cannot settle: reported, and the seam still closes.
    PrefStore::get().set("general", sim::kMaxPassesPref, 2);
    run("setAttr", {{"args", {"spring.damping", 0.05}}});
    run("setAttr", {{"args", {"spring.noise", 0.0}}});
    sim::clearCache(g);
    glm::dvec3 first = posAt(g, n, 0), last = posAt(g, n, 40);
    i = sim::info(g, n);
    CHECK_FALSE(i.converged);
    CHECK(i.residual > 1.0);
    CHECK(std::memcmp(&first, &last, sizeof first) == 0);
    json cmd = run("simulationInfo", {{"args", {"spring"}}});
    CHECK(cmd[0]["converged"] == false);
    PrefStore::get().remove("general", sim::kMaxPassesPref);
}

TEST_CASE("simulation: recording bakes to a new additive layer that reproduces it")
{
    Graph& g = Graph::current();
    NodeId n = springScene();
    sim::clearCache(g);
    std::vector<glm::dvec3> simulated, animated;
    for (int64_t f = 0; f <= 40; ++f)
        simulated.push_back(posAt(g, n, f));
    const NodeId follower = g.find("follower")->id();

    std::string layer = run("recordSimulation", {{"args", {"spring"}}});
    CHECK(layer == "spring_sim");
    Node* l = g.find(layer);
    REQUIRE(l);
    CHECK(anim::isLayer(*l));
    CHECK(l->as<int64_t>(anim::LayerNode::kMode) == int64_t(anim::LayerMode::Additive));
    for (int64_t f = 0; f <= 40; f += 5)
    {
        run("currentTime", {{"args", {double(f) / 30.0}}});
        g.evaluate();
        const glm::dvec3 got(g.node(follower)->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
        CHECK(glm::length(got - simulated[size_t(f)]) < 1e-6);
    }

    // Muted, the follower is back on its own (unanimated) place; the simulation doesn't read its own recording.
    run("editAnimLayer", {{"args", {layer}}, {"mute", true}});
    run("currentTime", {{"args", {20.0 / 30.0}}});
    g.evaluate();
    CHECK(glm::length(glm::dvec3(g.node(follower)->as<glm::dmat4>(Transform::kWorldMatrix)[3])) < 1e-9);
    run("editAnimLayer", {{"args", {layer}}, {"mute", false}});
    sim::clearCache(g);
    CHECK(glm::length(posAt(g, n, 20) - simulated[20]) < 1e-9);

    // A second recording is its own layer; undo takes it away.
    std::string second = run("recordSimulation", {{"args", {"spring"}}, {"tolerance", 0.01}});
    CHECK(second != layer);
    CHECK(g.find(second));
    run("undo");
    CHECK_FALSE(g.find(second));
    CHECK(g.find(layer));
}

TEST_CASE("simulation: stepped output holds poses")
{
    Graph& g = Graph::current();
    NodeId n = springScene();
    run("setAttr", {{"args", {"spring.stepped", 2}}});
    sim::clearCache(g);
    CHECK(posAt(g, n, 7) == posAt(g, n, 6));
    CHECK(posAt(g, n, 8) != posAt(g, n, 7));
}

TEST_CASE("simulation: seeded noise is deterministic, bounded and can repeat")
{
    double lo = 1.0, hi = -1.0;
    for (int i = 0; i < 2000; ++i)
    {
        const double x = i * 0.0137;
        const double v = sim::noise(42, x);
        CHECK(v == sim::noise(42, x));
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        CHECK(sim::noise(42, x, 5) == doctest::Approx(sim::noise(42, x + 5.0, 5)).epsilon(1e-12));
    }
    CHECK(lo >= -1.0);
    CHECK(hi <= 1.0);
    CHECK(hi - lo > 0.5);
    CHECK(sim::noise(1, 0.5) != sim::noise(2, 0.5));
    // Smooth: continuous across lattice cells.
    CHECK(std::abs(sim::noise(9, 3.0 - 1e-9) - sim::noise(9, 3.0)) < 1e-6);
}
