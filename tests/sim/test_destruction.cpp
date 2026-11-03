#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"
#include "scene/Graph.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"
#include "seq/Alembic.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace lila;
using namespace lila::scene;
using namespace simtest;
using json = nlohmann::json;

namespace
{
json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }
std::string add(const std::string& g, const char* type) { return run("gnAddNode", args({g, type})).get<std::string>(); }
void link(const std::string& g, const std::string& from, const std::string& to) { run("gnConnect", args({g, from, to})); }
void set(const std::string& g, const std::string& plug, json v) { run("gnSetValue", args({g, plug, v})); }

void loadDestruction()
{
    if (!gn::findDecl("RigidBodySolver"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_DESTRUCTION_PACKAGE) == "Destruction");
}

double frameTime(int frame) { return double(frame) / 24.0; }

/** A key holding its value until the next one. */
void keyStep(const std::string& plug, int frame, double value)
{
    run("setKeyframe",
        {{"args", {plug}}, {"time", frameTime(frame)}, {"value", value}, {"inTangentType", "step"}, {"outTangentType", "step"}});
}

struct Wall
{
    std::string mesh, modifier, solver;
};

/**
 * A 200 x 100 x 20 cm wall standing on the ground (y = 0), fractured into `pieces`, anchored, simulated; blast speed and
 * strength exposed on the modifier.
 */
Wall wallScene(int pieces)
{
    loadDestruction();
    run("newScene");
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 4.0}, {"fps", 24.0}});
    Wall w;
    w.mesh = run("polyCube", {{"name", "Wall"}, {"width", 200.0}, {"height", 100.0}, {"depth", 20.0}})[0];
    run("gnCreateGroup", args({"Collapse"}));
    const std::string lift = add("Collapse", "TransformGeometry"), fracture = add("Collapse", "Fracture"),
                      anchor = add("Collapse", "AnchorPieces"), solver = add("Collapse", "RigidBodySolver"),
                      realize = add("Collapse", "RealizeInstances");
    w.solver = solver;
    set("Collapse", lift + ".translation", json::array({0, 50, 0}));
    set("Collapse", fracture + ".pieces", pieces);
    set("Collapse", fracture + ".seed", 7);
    set("Collapse", solver + ".blastCenter", json::array({0, 50, 20}));
    set("Collapse", solver + ".blastRadius", 150.0);
    run("gnDisconnect", args({"Collapse", "GroupOutput.geometry"}));
    link("Collapse", "GroupInput.geometry", lift + ".geometry");
    link("Collapse", lift + ".geometry", fracture + ".mesh");
    link("Collapse", fracture + ".pieces", anchor + ".pieces");
    link("Collapse", fracture + ".links", anchor + ".links");
    link("Collapse", fracture + ".pieces", solver + ".pieces");
    link("Collapse", anchor + ".links", solver + ".links");
    link("Collapse", solver + ".pieces", realize + ".geometry");
    link("Collapse", realize + ".geometry", "GroupOutput.geometry");
    run("gnExposeInput", args({"Collapse", solver + ".blastSpeed", "blastSpeed"}));
    run("gnExposeInput", args({"Collapse", solver + ".strength", "strength"}));
    w.modifier = run("gnApply", args({w.mesh, "Collapse"})).get<std::string>();
    return w;
}

scene::MeshPtr evaluated(const std::string& mesh)
{
    Graph& g = Graph::current();
    g.evaluate();
    Node* n = g.find(mesh);
    return n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
}

geo::Bounds boundsAt(const std::string& mesh, int frame)
{
    run("currentTime", args({frameTime(frame)}));
    return evaluated(mesh)->bounds();
}

/** Bounds of the evaluated mesh with its world transform applied. */
geo::Bounds worldBoundsAt(const std::string& mesh, int frame)
{
    run("currentTime", args({frameTime(frame)}));
    scene::MeshPtr m = evaluated(mesh);
    const glm::dmat4 w = Graph::current().find(mesh)->as<glm::dmat4>(Transform::kWorldMatrix);
    geo::Bounds b;
    for (const glm::dvec3& p : m->positions())
        b.extend(glm::dvec3(w * glm::dvec4(p, 1.0)));
    return b;
}

/** The farthest any point moved between two frames. */
double largestMove(const std::string& mesh, int from, int to)
{
    run("currentTime", args({frameTime(from)}));
    const std::vector<glm::dvec3> a(evaluated(mesh)->positions().begin(), evaluated(mesh)->positions().end());
    run("currentTime", args({frameTime(to)}));
    std::span<const glm::dvec3> b = evaluated(mesh)->positions();
    double most = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        most = std::max(most, glm::length(a[i] - b[i]));
    return most;
}

uint64_t hashAt(const std::string& mesh, int frame)
{
    run("currentTime", args({frameTime(frame)}));
    std::span<const glm::dvec3> p = evaluated(mesh)->positions();
    return sim::hashBytes(p.data(), p.size_bytes());
}
} // namespace

TEST_CASE("an anchored wall stands; a blast knocks it down")
{
    Wall w = wallScene(24);
    const geo::Bounds start = boundsAt(w.mesh, 1);
    CHECK(start.min.y == doctest::Approx(0.0).epsilon(0.01));
    CHECK(start.max.y == doctest::Approx(100.0).epsilon(0.01));
    const geo::Bounds still = boundsAt(w.mesh, 48);
    CHECK(still.max.y == doctest::Approx(100.0).epsilon(0.02));

    run("setAttr", args({w.modifier + ".blastSpeed", 1500.0}));
    const auto t0 = std::chrono::steady_clock::now();
    const geo::Bounds hit = boundsAt(w.mesh, 48);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("24-piece wall, 48 frames x 4 substeps: %.0f ms\n", ms);
    CHECK(hit.max.y < 90.0);
    CHECK(hit.min.y > -2.0);
    CHECK(glm::length(hit.size()) > glm::length(start.size()) * 1.05);
}

TEST_CASE("destruction is deterministic: a restored run matches a straight one")
{
    Wall w = wallScene(20);
    run("setAttr", args({w.modifier + ".blastSpeed", 1200.0}));
    keyStep(w.modifier + ".strength", 1, 1.0);
    keyStep(w.modifier + ".strength", 25, 1.0);
    hashAt(w.mesh, 40);
    hashAt(w.mesh, 10);
    // Weakening the links from frame 25 keeps the cache to 24; the solver restores Jolt's state from there.
    keyStep(w.modifier + ".strength", 25, 0.3);
    const uint64_t resumed = hashAt(w.mesh, 40);
    const sim::Info info = sim::info(Graph::current(), Graph::current().find(w.modifier)->id());
    CHECK(info.lastInvalidated >= 20);

    sim::clearCache(Graph::current());
    hashAt(w.mesh, 1);
    const uint64_t straight = hashAt(w.mesh, 40);
    CHECK(resumed == straight);

    sim::clearCache(Graph::current());
    hashAt(w.mesh, 1);
    CHECK(hashAt(w.mesh, 40) == straight);

    // Jolt on one thread instead of the TBB pool: the same bytes.
#ifdef _WIN32
    _putenv_s("LILA_PHYSICS_THREADS", "1");
#else
    setenv("LILA_PHYSICS_THREADS", "1", 1);
#endif
    sim::clearCache(Graph::current());
    hashAt(w.mesh, 1);
    const uint64_t serial = hashAt(w.mesh, 40);
#ifdef _WIN32
    _putenv_s("LILA_PHYSICS_THREADS", "");
#else
    unsetenv("LILA_PHYSICS_THREADS");
#endif
    CHECK(serial == straight);
}

TEST_CASE("breakApart sets up a standing wall that a click knocks down, and it bakes to rigid vertex animation")
{
    loadDestruction();
    run("newScene");
    run("playbackOptions", {{"minTime", 1.0 / 24.0}, {"maxTime", 1.0}, {"fps", 24.0}});
    const std::string wall = run("polyCube", {{"name", "Wall"}, {"width", 200.0}, {"height", 100.0}, {"depth", 20.0}})[0];
    run("move", {{"args", {wall}}, {"to", json::array({0, 50, 0})}});
    json mods = run("breakApart", {{"args", {wall}}, {"material", "concrete"}, {"pieces", 16}, {"impact", json::array({0, 60, 10})}});
    REQUIRE(mods.size() == 1);
    const std::string mod = mods[0].get<std::string>();

    // The wall stands on the ground until the blast at its start frame (1) throws pieces out.
    const geo::Bounds start = worldBoundsAt(wall, 1);
    CHECK(start.min.y == doctest::Approx(0.0).epsilon(0.01));
    CHECK(start.max.y == doctest::Approx(100.0).epsilon(0.01));
    CHECK(largestMove(wall, 1, 24) > 20.0);
    CHECK(worldBoundsAt(wall, 24).min.y > -2.0);

    run("setAttr", args({mod + ".blast", 0.0}));
    CHECK(largestMove(wall, 1, 24) < 1.0);
    run("setAttr", args({mod + ".blast", 1500.0}));

    const auto folder = std::filesystem::temp_directory_path() / "lila_destruction_vat";
    std::filesystem::remove_all(folder);
    json r = run("exportVertexAnimation", {{"args", {wall}}, {"start", 1.0}, {"end", 24.0}, {"folder", folder.string()}, {"name", "WallBreak"}});
    CHECK(r["meta"]["mode"] == "rigid");
    CHECK(r["meta"]["pieceCount"].get<int>() >= 8);
    CHECK(r["meta"]["rigid"]["maxResidual"].get<double>() < 0.01);

    const auto csv = folder / "breaks.csv";
    json breaks = run("exportBreaks", {{"args", {wall}}, {"path", csv.string()}, {"end", 24}});
    CHECK(breaks["breaks"].get<int>() > 0);
    {
        std::ifstream in(csv);
        std::string line;
        int lines = 0;
        while (std::getline(in, line))
            ++lines;
        CHECK(lines == breaks["breaks"].get<int>() + 1);
    }

    const int64_t seed = Graph::current().find(mod)->as<int64_t>(Graph::current().find(mod)->type().attrIndex("seed"));
    json wedges = run("wedgeDestruction", {{"args", {wall}}, {"count", 2}, {"folder", (folder / "wedges").string()}, {"start", 1.0}, {"end", 12.0}});
    CHECK(wedges.size() == 2);
    CHECK(Graph::current().find(mod)->as<int64_t>(Graph::current().find(mod)->type().attrIndex("seed")) == seed);
    CHECK(std::filesystem::exists(folder / "wedges" / ("Wall_seed" + std::to_string(seed + 1) + "_pos.exr")));

    json baked = run("bakePiecesToJoints", {{"args", {wall}}, {"start", 1.0}, {"end", 24.0}});
    CHECK(baked["joints"].size() == r["meta"]["pieceCount"].get<size_t>());
    CHECK(baked["residual"].get<double>() < 0.01);
    const std::string skinned = baked["mesh"].get<std::string>();
    for (int frame : {1, 12, 24})
    {
        const geo::Bounds sim = worldBoundsAt(wall, frame);
        const geo::Bounds keyed = worldBoundsAt(skinned, frame);
        CHECK(glm::length(sim.min - keyed.min) < 0.05);
        CHECK(glm::length(sim.max - keyed.max) < 0.05);
    }
}

TEST_CASE("zz destruction timing" * doctest::skip())
{
    for (int pieces : {100, 300, 600})
    {
        Wall w = wallScene(pieces);
        run("setAttr", args({w.modifier + ".blastSpeed", 1500.0}));
        auto t0 = std::chrono::steady_clock::now();
        boundsAt(w.mesh, 1);
        const double fracture = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        boundsAt(w.mesh, 72);
        const double sim = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        boundsAt(w.mesh, 36);
        const double scrub = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (int f : {37, 38, 36})
        {
            t0 = std::chrono::steady_clock::now();
            boundsAt(w.mesh, f);
            std::printf("  scrub to %d: %.1f ms\n", f,std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::printf("  output: %u faces, %u points\n", evaluated(w.mesh)->numFaces(), evaluated(w.mesh)->numPoints());
        std::printf("%d pieces: fracture + first frame %.0f ms, 72 frames x 4 substeps %.0f ms, scrub back %.1f ms\n", pieces, fracture, sim, scrub);
    }
}

TEST_CASE("guided pieces are pulled back to their own transform")
{
    Wall w = wallScene(20);
    run("setAttr", args({w.modifier + ".blastSpeed", 1500.0}));
    const double free = largestMove(w.mesh, 1, 72);
    CHECK(free > 30.0);

    const std::string store = add("Collapse", "StoreNamedAttribute");
    set("Collapse", store + ".domain", "instance");
    set("Collapse", store + ".name", "guide");
    set("Collapse", store + ".value", 1.0);
    run("gnDisconnect", args({"Collapse", w.solver + ".pieces"}));
    json info = run("gnGroupInfo", args({"Collapse"}));
    std::string fracture;
    for (const json& n : info["nodes"])
        if (n["type"] == "Fracture")
            fracture = n["name"];
    link("Collapse", fracture + ".pieces", store + ".geometry");
    link("Collapse", store + ".geometry", w.solver + ".pieces");
    set("Collapse", w.solver + ".guideStiffness", 4.0);
    const double guided = largestMove(w.mesh, 1, 72);
    CHECK(guided < free * 0.25);
}

TEST_CASE("a destruction exports to Alembic as one transform per piece")
{
    Wall w = wallScene(12);
    run("setAttr", args({w.modifier + ".blastSpeed", 1500.0}));
    const auto folder = std::filesystem::temp_directory_path() / "lila_destruction_abc";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const std::string path = (folder / "wall.abc").string();
    run("exportAlembic", {{"args", {w.mesh}}, {"path", path}, {"start", 1.0}, {"end", 24.0}, {"rigidPieces", true}});
    std::string error;
    const std::vector<seq::abc::ObjectInfo> objects = seq::abc::read(path, &error);
    CHECK(error.empty());
    CHECK(objects.size() >= 8);
    for (const seq::abc::ObjectInfo& o : objects)
        CHECK(o.samples == 1);
    const std::string meshes = (folder / "wall_mesh.abc").string();
    run("exportAlembic", {{"args", {w.mesh}}, {"path", meshes}, {"start", 1.0}, {"end", 24.0}});
    CHECK(std::filesystem::file_size(path) * 3 < std::filesystem::file_size(meshes));
}

TEST_CASE("meshes broken together hit each other; explosions go off on their frames; collapse drops the ground in a sweep")
{
    loadDestruction();
    run("newScene");
    run("playbackOptions", {{"minTime", 1.0 / 24.0}, {"maxTime", 3.0}, {"fps", 24.0}});
    run("polyCube", {{"name", "Ground"}, {"position", json::array({0, -30, 0})}, {"width", 800.0}, {"height", 30.0}, {"depth", 300.0},
                     {"baseAtOrigin", true}, {"generator", false}});
    run("polyCube", {{"name", "Wall"}, {"position", json::array({0, 0, 0})}, {"width", 200.0}, {"height", 100.0}, {"depth", 20.0},
                     {"baseAtOrigin", true}, {"generator", false}});
    json mods = run("breakApart", {{"args", {"Ground", "Wall"}}, {"pieces", 20}});
    REQUIRE(mods.size() == 1);
    const std::string mod = mods[0].get<std::string>();
    CHECK(Graph::current().find("Destruction") != nullptr);

    // Nothing moves before the explosions; the wall stands on the slab.
    CHECK(largestMove("Destruction", 1, 12) < 1.0);
    run("addExplosion", {{"args", {"Destruction"}}, {"position", json::array({-60, 50, 40})}, {"frame", 12}, {"blast", 2500.0}, {"radius", 150.0}});
    run("addExplosion", {{"args", {"Destruction"}}, {"position", json::array({60, 50, 40})}, {"frame", 30}, {"blast", 2500.0}, {"radius", 150.0}});
    CHECK(largestMove("Destruction", 1, 11) < 1.0);
    CHECK(largestMove("Destruction", 11, 20) > 20.0);

    // The ground fails from -X to +X between frames 40 and 60 and falls into the pit.
    run("collapse", {{"args", {"Destruction"}}, {"part", "Ground"}, {"start", 40.0}, {"duration", 20.0}, {"direction", json::array({1, 0, 0})},
                     {"rumble", 2.0}, {"lead", 10.0}, {"raggedness", 0.0}, {"depth", 1000.0}});
    run("currentTime", args({frameTime(1)}));
    const std::vector<glm::dvec3> rest(evaluated("Destruction")->positions().begin(), evaluated("Destruction")->positions().end());
    // The slab's own points (below its top at frame 1), by where they started.
    auto groundBelow = [&](int frame, double xMin, double xMax) {
        run("currentTime", args({frameTime(frame)}));
        std::span<const glm::dvec3> now = evaluated("Destruction")->positions();
        int below = 0, total = 0;
        for (size_t i = 0; i < rest.size() && i < now.size(); ++i)
            if (rest[i].y < -1.0 && rest[i].x >= xMin && rest[i].x <= xMax)
            {
                ++total;
                below += now[i].y < -80.0 ? 1 : 0;
            }
        return total ? double(below) / total : 0.0;
    };
    CHECK(groundBelow(38, -400, 400) == 0.0);
    CHECK(groundBelow(55, -400, -250) > 0.5);
    CHECK(groundBelow(55, 250, 400) < 0.1);
    CHECK(groundBelow(72, -400, 400) > 0.9);
    CHECK(worldBoundsAt("Destruction", 72).min.y > -1040.0);
}

TEST_CASE("sound cues hear a destruction's breaks and hits from its cached frames")
{
    loadDestruction();
    run("newScene");
    run("playbackOptions", {{"minTime", 1.0 / 24.0}, {"maxTime", 1.0}, {"fps", 24.0}});
    const std::string wall = run("polyCube", {{"name", "Wall"}, {"width", 200.0}, {"height", 100.0}, {"depth", 20.0}})[0];
    run("move", {{"args", {wall}}, {"to", json::array({0, 50, 0})}});
    run("breakApart", {{"args", {wall}}, {"material", "concrete"}, {"pieces", 16}, {"impact", json::array({0, 60, 10})}});
    run("soundCue", {{"name", "cracks"}, {"eventType", "break"}, {"sounds", {"crack.wav"}}, {"targets", wall}, {"minInterval", 0.0}});
    run("soundCue", {{"name", "thuds"}, {"eventType", "contact"}, {"minStrength", 50.0}, {"sounds", {"thud1.wav", "thud2.wav"}}});

    // Nothing is simulated yet, so nothing fires: cues never step a simulation.
    CHECK(run("soundCueTriggers").empty());
    for (int f = 1; f <= 24; ++f)
    {
        Graph::current().setTime(f / 24.0);
        Graph::current().evaluate();
    }
    int breaks = 0, hits = 0;
    for (const json& t : run("soundCueTriggers"))
    {
        (t["event"] == "break" ? breaks : hits) += 1;
        CHECK(t["object"].get<std::string>().find("Wall") != std::string::npos);
    }
    const auto csv = std::filesystem::temp_directory_path() / "lila_cue_breaks.csv";
    const json exported = run("exportBreaks", {{"args", {wall}}, {"path", csv.string()}, {"end", 24}});
    CHECK(breaks == exported["breaks"].get<int>());
    CHECK(hits > 0);
}
