#include <doctest/doctest.h>

#include "ops/Command.h"
#include "gn/Exec.h"
#include "gn/Gn.h"
#include "gn/NodesCommon.h"
#include "gn/Stateful.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/Simulation.h"

#include <glm/gtc/matrix_transform.hpp>

#include <atomic>
#include <cstring>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }
std::string add(const std::string& g, const char* type) { return run("gnAddNode", args({g, type})).get<std::string>(); }
void link(const std::string& g, const std::string& from, const std::string& to) { run("gnConnect", args({g, from, to})); }

std::atomic<int> gUpstreamRuns{0};
std::atomic<int> gResets{0};

/** Steps add `rate` to a counter; the output lifts the geometry by it. */
class CounterSim final : public gn::NodeSim
{
public:
    Setup configure(const gn::SimInputs& in) override { return {in.i("start"), int(in.i("substeps")), 0}; }
    void reset(const gn::SimInputs&, std::vector<uint8_t>& state) override
    {
        ++gResets;
        state.assign(sizeof(int64_t), 0);
    }
    void step(const gn::SimInputs& in, std::vector<uint8_t>& state, const scene::sim::StepInfo&) override
    {
        int64_t v = 0;
        std::memcpy(&v, state.data(), sizeof v);
        v += in.i("rate");
        std::memcpy(state.data(), &v, sizeof v);
    }
};

void registerTestNodes()
{
    if (gn::findDecl("TestCounter"))
        return;
    using gn::nodes::in;
    using gn::nodes::geoIn;
    using gn::nodes::geoOut;
    using T = gn::SocketType;
    auto stateful = std::make_shared<gn::StatefulDecl>();
    stateful->create = [] { return std::make_unique<CounterSim>(); };
    stateful->output = [](gn::ExecContext& c, const std::vector<uint8_t>* state) {
        int64_t v = 0;
        if (state && state->size() >= sizeof v)
            std::memcpy(&v, state->data(), sizeof v);
        c.out("geometry", gn::makeGeometry(gn::transformed(*c.geometry("geometry"), glm::translate(glm::dmat4(1.0), glm::dvec3(0, double(v), 0)))));
    };
    gn::registerDecl({.type = "TestCounter",
                      .label = "Test Counter",
                      .category = "Test",
                      .sockets = {.inputs = {geoIn("geometry"), in("start", T::Int, 1), in("substeps", T::Int, 1), in("rate", T::Int, 1)},
                                  .outputs = {geoOut("geometry")}},
                      .stateful = stateful});
    gn::registerDecl({.type = "TestCountRuns",
                      .label = "Test Count Runs",
                      .category = "Test",
                      .sockets = {.inputs = {geoIn("geometry")}, .outputs = {geoOut("geometry")}},
                      .exec =
                          [](gn::ExecContext& c) {
                              ++gUpstreamRuns;
                              c.out("geometry", c.geometry("geometry"));
                          }});
}

double topY(const std::string& mesh) { return run("meshInfo", args({mesh}))["boundsMax"][1].get<double>(); }

/** Cube -> Test Count Runs -> Test Counter -> output, with rate exposed. Returns the modifier. */
std::string counterScene(const std::string& mesh)
{
    registerTestNodes();
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 10.0}, {"fps", 24.0}});
    run("gnCreateGroup", args({"Count"}));
    const std::string runs = add("Count", "TestCountRuns"), counter = add("Count", "TestCounter");
    run("gnDisconnect", args({"Count", "GroupOutput.geometry"}));
    link("Count", "GroupInput.geometry", runs + ".geometry");
    link("Count", runs + ".geometry", counter + ".geometry");
    link("Count", counter + ".geometry", "GroupOutput.geometry");
    run("gnExposeInput", args({"Count", counter + ".rate", "rate"}));
    return run("gnApply", args({mesh, "Count"})).get<std::string>();
}

double frameTime(int frame) { return double(frame) / 24.0; }

/** A key holding its value until the next one. */
void keyStep(const std::string& plug, int frame, double value)
{
    run("setKeyframe",
        {{"args", {plug}}, {"time", frameTime(frame)}, {"value", value}, {"inTangentType", "step"}, {"outTangentType", "step"}});
}
} // namespace

TEST_CASE("a simulation node steps once per frame from its start and scrubs back from the cache")
{
    run("newScene");
    const std::string mesh = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Box"}})[0];
    const std::string mod = counterScene(mesh);
    run("currentTime", args({frameTime(1)}));
    CHECK(topY(mesh) == doctest::Approx(50.0));
    run("currentTime", args({frameTime(11)}));
    CHECK(topY(mesh) == doctest::Approx(60.0));
    run("currentTime", args({frameTime(5)}));
    CHECK(topY(mesh) == doctest::Approx(54.0));
    // Before the start the first frame stands in.
    run("currentTime", args({frameTime(-3)}));
    CHECK(topY(mesh) == doctest::Approx(50.0));
}

TEST_CASE("upstream steps that don't depend on time run once across the frames sampled")
{
    run("newScene");
    const std::string mesh = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Box"}})[0];
    const std::string mod = counterScene(mesh);
    run("currentTime", args({frameTime(1)}));
    gUpstreamRuns = 0;
    run("currentTime", args({frameTime(30)}));
    CHECK(topY(mesh) == doctest::Approx(79.0));
    CHECK(gUpstreamRuns.load() <= 2);
}

TEST_CASE("changing a simulation's input re-simulates; scene edits elsewhere keep the cache")
{
    run("newScene");
    const std::string mesh = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Box"}})[0];
    const std::string mod = counterScene(mesh);
    run("currentTime", args({frameTime(11)}));
    CHECK(topY(mesh) == doctest::Approx(60.0));
    gResets = 0;
    run("polySphere", {{"radius", 50.0}, {"name", "Other"}});
    run("currentTime", args({frameTime(12)}));
    CHECK(topY(mesh) == doctest::Approx(61.0));
    CHECK(gResets.load() == 0);
    run("setAttr", args({mod + ".rate", 2}));
    CHECK(topY(mesh) == doctest::Approx(72.0));
    CHECK(gResets.load() == 1);
}

TEST_CASE("a keyed simulation input is sampled every frame")
{
    run("newScene");
    const std::string mesh = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Box"}})[0];
    const std::string mod = counterScene(mesh);
    keyStep(mod + ".rate", 1, 1);
    keyStep(mod + ".rate", 5, 1);
    keyStep(mod + ".rate", 6, 3);
    run("currentTime", args({frameTime(8)}));
    // Frames 2..5 add 1 each, 6..8 add 3 each.
    CHECK(topY(mesh) == doctest::Approx(50.0 + 4 + 9));
}

TEST_CASE("destruction nodes: mesh to pieces, connect and anchor")
{
    run("newScene");
    registerTestNodes();
    const std::string a = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "A"}})[0];
    run("gnCreateGroup", args({"Split"}));
    const std::string grid = add("Split", "MeshGrid"), toPieces = add("Split", "MeshToPieces"), connect = add("Split", "ConnectPieces"),
                      anchor = add("Split", "AnchorPieces");
    (void)grid;
    link("Split", "GroupInput.geometry", toPieces + ".mesh");
    link("Split", toPieces + ".pieces", connect + ".pieces");
    link("Split", toPieces + ".pieces", anchor + ".pieces");
    link("Split", connect + ".links", anchor + ".links");

    scene::Graph& g = scene::Graph::current();
    gn::Group grp = gn::readGroup(*gn::findGroupNode(g, "Split"));
    // Two cubes side by side, touching along x = 50.
    geo::Mesh two;
    {
        scene::Node* n = g.find(a);
        g.evaluate();
        auto m = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
        two = *m;
        geo::Mesh moved = *m;
        gn::transformMesh(moved, glm::translate(glm::dmat4(1.0), glm::dvec3(100, 0, 0)));
        two.append(moved);
        two.compact();
        gn::transformMesh(two, glm::translate(glm::dmat4(1.0), glm::dvec3(0, 50, 0)));
    }
    gn::ExecEnv env;
    env.input = gn::meshGeometry(two);

    gn::Group probe = grp;
    std::erase_if(probe.links, [](const gn::GLink& l) { return l.toNode == "GroupOutput"; });
    probe.links.push_back({toPieces, "pieces", "GroupOutput", "geometry"});
    gn::GeometryPtr pieces = gn::execute(*gn::compile(probe), env);
    REQUIRE(pieces->instances);
    CHECK(pieces->instances->size() == 2);
    CHECK(glm::dvec3(pieces->instances->transforms[1][3]).x == doctest::Approx(100.0));

    probe.links.back() = {connect, "links", "GroupOutput", "geometry"};
    gn::GeometryPtr links = gn::execute(*gn::compile(probe), env);
    REQUIRE(links->curves);
    CHECK(links->curves->numCurves() == 1);
    CHECK(links->curves->curveAttrs.get<float>("strength")[0] == doctest::Approx(20.0 * 100.0 * 100.0).epsilon(0.05));

    probe.links.back() = {anchor, "links", "GroupOutput", "geometry"};
    links = gn::execute(*gn::compile(probe), env);
    REQUIRE(links->curves);
    CHECK(links->curves->numCurves() == 3);
    CHECK(links->curves->curveAttrs.get<int32_t>("pieceB")[2] == -1);
}
