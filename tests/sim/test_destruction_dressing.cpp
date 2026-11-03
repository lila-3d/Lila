#include <doctest/doctest.h>

#include "ops/Command.h"
#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "gn/Gn.h"
#include "gn/NodeLib.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;

namespace dressing_test
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }

void load()
{
    if (!gn::findDecl("RigidBodySolver"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_DESTRUCTION_PACKAGE) == "Destruction");
}

double frameTime(int frame) { return double(frame) / 24.0; }

MeshPtr evaluated(const std::string& mesh)
{
    Graph& g = Graph::current();
    g.evaluate();
    Node* n = g.find(mesh);
    return n->as<MeshPtr>(MeshNode::kOutMesh);
}

/** A mesh's evaluated points in world space at a frame. */
std::vector<glm::dvec3> worldPoints(const std::string& mesh, int frame)
{
    run("currentTime", args({frameTime(frame)}));
    MeshPtr m = evaluated(mesh);
    const glm::dmat4 w = Graph::current().find(mesh)->as<glm::dmat4>(Transform::kWorldMatrix);
    std::vector<glm::dvec3> out;
    for (const glm::dvec3& p : m->positions())
        out.push_back(glm::dvec3(w * glm::dvec4(p, 1.0)));
    return out;
}

geo::Bounds boundsOf(const std::vector<glm::dvec3>& pts)
{
    geo::Bounds b;
    for (const glm::dvec3& p : pts)
        b.extend(p);
    return b;
}

double largestMove(const std::string& mesh, int from, int to)
{
    const std::vector<glm::dvec3> a = worldPoints(mesh, from), b = worldPoints(mesh, to);
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

/** The frozen pieces under a group: world bounds of the pieces (not their collision), and how many have a UCX_ child. */
struct Frozen
{
    size_t pieces = 0, withCollision = 0;
    geo::Bounds bounds;
};

Frozen frozen(const std::string& group)
{
    Graph& g = Graph::current();
    g.evaluate();
    Frozen f;
    Node* grp = g.find(group);
    REQUIRE(grp);
    for (NodeId c : g.children(grp->id()))
    {
        Node* n = g.node(c);
        ++f.pieces;
        for (NodeId k : g.children(c))
            if (g.node(k)->name().starts_with("UCX_"))
            {
                ++f.withCollision;
                CHECK(g.node(k)->as<MeshPtr>(MeshNode::kOutMesh)->numFaces() >= 4);
            }
        const MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
        REQUIRE(m);
        const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
        for (const glm::dvec3& p : m->positions())
            f.bounds.extend(glm::dvec3(w * glm::dvec4(p, 1.0)));
    }
    return f;
}

std::string tempScene(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

std::string wall(const char* name, double y = 50.0)
{
    const std::string w = run("polyCube", {{"name", name}, {"width", 200.0}, {"height", 100.0}, {"depth", 20.0}})[0];
    run("move", {{"args", {w}}, {"to", json::array({0, y, 0})}});
    return w;
}
} // namespace dressing_test

using namespace dressing_test;

TEST_CASE("freezeDestruction: the pieces at rest become static meshes with collision; undo, objects and save + reopen")
{
    load();
    run("newScene");
    run("playbackOptions", {{"minTime", 1.0 / 24.0}, {"maxTime", 2.0}, {"fps", 24.0}});
    const std::string w = wall("Wall");
    run("breakApart", {{"args", {w}}, {"pieces", 16}, {"impact", json::array({0, 60, 10})}});

    json r = run("freezeDestruction", {{"args", {w}}});
    CHECK(r["settled"].get<bool>());
    CHECK(r["frame"].get<int64_t>() > 1);
    const size_t pieces = r["pieces"].get<size_t>();
    CHECK(pieces >= 8);
    CHECK(r["meshes"].get<size_t>() == pieces);
    const std::string group = r["group"].get<std::string>();
    Frozen f = frozen(group);
    CHECK(f.pieces == pieces);
    CHECK(f.withCollision == pieces);
    CHECK(f.bounds.min.y > -3.0);
    CHECK(f.bounds.max.y < 95.0);
    CHECK(run("getAttr", args({w + ".visibility"})) == false);

    // Static: time doesn't move them.
    run("currentTime", args({frameTime(1)}));
    const Frozen still = frozen(group);
    CHECK(glm::length(still.bounds.min - f.bounds.min) < 1e-9);

    run("undo");
    CHECK(Graph::current().find(group) == nullptr);
    CHECK(run("getAttr", args({w + ".visibility"})) == true);

    // A chosen frame, every piece its own object, no collision: the wall still standing at frame 1.
    json standing = run("freezeDestruction", {{"args", {w}}, {"frame", 1}, {"meshes", "objects"}, {"collision", false}, {"name", "Standing"}});
    const Frozen s = frozen(standing["group"].get<std::string>());
    CHECK(s.withCollision == 0);
    CHECK(s.bounds.max.y == doctest::Approx(100.0).epsilon(0.01));
    CHECK(s.bounds.min.y == doctest::Approx(0.0).epsilon(0.01));

    r = run("freezeDestruction", {{"args", {w}}, {"hide", false}});
    f = frozen(r["group"].get<std::string>());
    const std::string path = tempScene("lila_frozen_destruction.lila");
    run("saveScene", args({path}));
    run("newScene");
    run("openScene", args({path}));
    const Frozen back = frozen(r["group"].get<std::string>());
    CHECK(back.pieces == f.pieces);
    CHECK(back.withCollision == f.withCollision);
    CHECK(glm::length(back.bounds.min - f.bounds.min) < 1e-6);
    CHECK(glm::length(back.bounds.max - f.bounds.max) < 1e-6);
    std::filesystem::remove(path);
}

TEST_CASE("damaged props without a simulation: cracked, missing, chipped and collapsed")
{
    load();
    // A wall damaged one way, and its modifier: amount (and gap) at 0 show it undamaged.
    auto setup = [](const char* name, const char* damage) {
        const std::string w = wall(name);
        json call = {{"args", {w}}, {"pieces", 24}, {"impact", json::array({0, 90, 10})}, {"damage", damage}, {"amount", 0.3}};
        const std::string mod = run("breakApart", call)[0].get<std::string>();
        return std::make_pair(w, mod);
    };
    auto undamaged = [](const std::string& w, const std::string& mod, bool gap) {
        run("setAttr", args({mod + ".amount", 0.0}));
        if (gap)
            run("setAttr", args({mod + ".gap", 0.0}));
        std::vector<glm::dvec3> pts = worldPoints(w, 1);
        run("setAttr", args({mod + ".amount", 0.3}));
        if (gap)
            run("setAttr", args({mod + ".gap", 0.3}));
        return pts;
    };

    SUBCASE("cracked stands where it was, cracks open")
    {
        run("newScene");
        auto [w, mod] = setup("Wall", "cracked");
        const std::vector<glm::dvec3> before = undamaged(w, mod, true), after = worldPoints(w, 1);
        REQUIRE(before.size() == after.size());
        const geo::Bounds b = boundsOf(after);
        CHECK(b.min.y > -3.0);
        CHECK(b.max.y < 103.0);
        double moved = 0.0;
        for (size_t i = 0; i < before.size(); ++i)
            moved = std::max(moved, glm::length(before[i] - after[i]));
        CHECK(moved > 0.05);
        CHECK(moved < 5.0);
    }
    SUBCASE("missing and chipped take pieces out")
    {
        for (const char* damage : {"missing", "chipped"})
        {
            run("newScene");
            auto [w, mod] = setup("Wall", damage);
            const size_t whole = undamaged(w, mod, false).size();
            const std::vector<glm::dvec3> after = worldPoints(w, 1);
            CHECK(after.size() < whole);
            CHECK(boundsOf(after).max.y <= 100.5);
        }
    }
    SUBCASE("collapsed lets what lost its support fall and settle; deterministic; survives save + reopen")
    {
        run("newScene");
        auto [w, mod] = setup("Wall", "collapsed");
        const std::vector<glm::dvec3> before = undamaged(w, mod, false), after = worldPoints(w, 1);
        REQUIRE(before.size() == after.size());
        double fell = 0.0;
        for (size_t i = 0; i < before.size(); ++i)
            fell = std::max(fell, before[i].y - after[i].y);
        CHECK(fell > 10.0);
        CHECK(boundsOf(after).min.y > -2.0);
        const uint64_t h = hashAt(w, 1);

        auto [again, mod2] = setup("Again", "collapsed");
        CHECK(hashAt(again, 1) == h);

        const std::string path = tempScene("lila_damaged_wall.lila");
        run("saveScene", args({path}));
        run("newScene");
        run("openScene", args({path}));
        CHECK(hashAt(w, 1) == h);
        std::filesystem::remove(path);
    }
}

TEST_CASE("addDebris: chunks thrown off at the breaks settle as instances of a few shapes, and freeze with the pieces")
{
    load();
    run("newScene");
    run("playbackOptions", {{"minTime", 1.0 / 24.0}, {"maxTime", 2.0}, {"fps", 24.0}});
    const std::string w = wall("Wall");
    json mods = run("breakApart", {{"args", {w}}, {"pieces", 12}, {"impact", json::array({0, 60, 10})}});
    CHECK(run("addDebris", {{"args", {w}}, {"amount", 3.0}, {"size", 6.0}, {"dust", 4}}) == "Debris");
    run("addDebris", {{"args", {w}}, {"amount", 3.0}, {"size", 6.0}, {"dust", 4}});

    run("currentTime", args({frameTime(40)}));
    Graph& g = Graph::current();
    g.evaluate();
    gn::GeometryPtr out = gn::modifierGeometry(g.find(mods[0].get<std::string>())->id());
    REQUIRE(out);
    REQUIRE(out->instances);
    const size_t total = out->instances->size();
    CHECK(total > 12);
    // The modifier's space has the wall's base at -50.
    for (size_t k = 12; k < total; ++k)
        CHECK(out->instances->transforms[k][3].y > -53.0);

    json r = run("freezeDestruction", {{"args", {w}}, {"frame", 40}});
    CHECK(r["pieces"].get<size_t>() == total);
    CHECK(r["meshes"].get<size_t>() < total);
}

TEST_CASE("kilometre scale: walls metres across, kilometres out, stand, fall and stay deterministic")
{
    load();
    run("newScene");
    run("playbackOptions", {{"minTime", 1.0 / 24.0}, {"maxTime", 2.0}, {"fps", 24.0}});
    const double x = 500000.0, z = 400000.0; // 5 km and 4 km from the origin
    run("polyCube", {{"name", "Ground"}, {"position", json::array({x, -100, z})}, {"width", 6000.0}, {"height", 100.0}, {"depth", 3000.0},
                     {"baseAtOrigin", true}, {"generator", false}});
    run("polyCube", {{"name", "Wall"}, {"position", json::array({x, 0, z})}, {"width", 2000.0}, {"height", 1000.0}, {"depth", 200.0},
                     {"baseAtOrigin", true}, {"generator", false}});
    json mods = run("breakApart", {{"args", {"Wall"}}, {"colliders", {"Ground"}}, {"pieces", 12}});
    REQUIRE(mods.size() == 1);
    const std::string mod = mods[0].get<std::string>();
    // Concrete as strong as the real thing (200 N/cm2 in tension), which a wall this big needs to hold itself up.
    run("setAttr", args({mod + ".strength", 200.0}));

    CHECK(largestMove("Destruction", 1, 12) < 0.01);
    run("addExplosion", {{"args", {"Destruction"}}, {"position", json::array({x, 500, z + 150})}, {"frame", 6}, {"blast", 3000.0}, {"radius", 1500.0}});
    CHECK(largestMove("Destruction", 1, 30) > 50.0);
    const geo::Bounds after = boundsOf(worldPoints("Destruction", 40));
    CHECK(after.min.y > -10.0);
    CHECK(after.min.x > x - 5000.0);

    const uint64_t straight = hashAt("Destruction", 40);
    sim::clearCache(Graph::current());
    hashAt("Destruction", 1);
    CHECK(hashAt("Destruction", 40) == straight);
#ifdef _WIN32
    _putenv_s("LILA_PHYSICS_THREADS", "1");
#else
    setenv("LILA_PHYSICS_THREADS", "1", 1);
#endif
    sim::clearCache(Graph::current());
    hashAt("Destruction", 1);
    const uint64_t serial = hashAt("Destruction", 40);
#ifdef _WIN32
    _putenv_s("LILA_PHYSICS_THREADS", "");
#else
    unsetenv("LILA_PHYSICS_THREADS");
#endif
    CHECK(serial == straight);

    const std::string path = tempScene("lila_km_destruction.lila");
    run("saveScene", args({path}));
    run("newScene");
    run("openScene", args({path}));
    hashAt("Destruction", 1);
    CHECK(hashAt("Destruction", 40) == straight);
    std::filesystem::remove(path);

    json r = run("freezeDestruction", {{"args", {"Destruction"}}, {"frame", 40}});
    const Frozen f = frozen(r["group"].get<std::string>());
    CHECK(glm::length(f.bounds.min - after.min) < 0.01);
    CHECK(glm::length(f.bounds.max - after.max) < 0.01);
}

TEST_CASE("exportChaos writes the pieces and their motion for a Geometry Collection and Chaos cache, in Unreal's space")
{
    load();
    run("newScene");
    run("playbackOptions", {{"minTime", 1.0 / 24.0}, {"maxTime", 1.0}, {"fps", 24.0}});
    const std::string w = wall("Wall");
    run("breakApart", {{"args", {w}}, {"pieces", 10}, {"impact", json::array({0, 60, 10})}});
    const auto folder = std::filesystem::temp_directory_path() / "lila_chaos_export";
    std::filesystem::remove_all(folder);
    json r = run("exportChaos", {{"args", {w}}, {"folder", folder.string()}, {"start", 1}, {"end", 24}});
    CHECK(r["frames"].get<int>() == 24);
    const size_t pieces = r["pieces"].get<size_t>();
    CHECK(pieces >= 6);

    std::ifstream js(r["json"].get<std::string>());
    const json m = json::parse(js);
    CHECK(m["format"] == "lila-geometry-collection");
    REQUIRE(m["pieces"].size() == pieces);
    std::ifstream bf(r["bin"].get<std::string>(), std::ios::binary);
    const std::vector<char> bin((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
    auto buffer = [&](const char* name, size_t stride) {
        const json& b = m["buffers"][name];
        CHECK(b["offset"].get<size_t>() + b["count"].get<size_t>() * stride <= bin.size());
        return std::make_pair(bin.data() + b["offset"].get<size_t>(), b["count"].get<size_t>());
    };
    auto [pos, vertices] = buffer("positions", 12);
    auto [nrm, normals] = buffer("normals", 12);
    auto [idx, corners] = buffer("indices", 4);
    auto [xf, transforms] = buffer("transforms", 28);
    CHECK(normals == vertices);
    CHECK(transforms == pieces * 24);
    CHECK(corners % 3 == 0);

    // Outward winding: each triangle's face normal agrees with its corners' normals.
    const auto* p = reinterpret_cast<const glm::vec3*>(pos);
    const auto* n = reinterpret_cast<const glm::vec3*>(nrm);
    const auto* ix = reinterpret_cast<const int32_t*>(idx);
    size_t agree = 0, total = 0;
    for (const json& piece : m["pieces"])
    {
        const size_t v0 = piece["vertexStart"], t0 = piece["triangleStart"], tn = piece["triangleCount"];
        for (size_t t = t0; t < t0 + tn; ++t)
        {
            const glm::vec3 a = p[v0 + ix[t * 3]], b = p[v0 + ix[t * 3 + 1]], c = p[v0 + ix[t * 3 + 2]];
            const glm::vec3 face = glm::cross(b - a, c - a);
            if (glm::length(face) < 1e-6f)
                continue;
            ++total;
            agree += glm::dot(face, n[v0 + ix[t * 3]]) > 0.0f ? 1 : 0;
        }
    }
    CHECK(total > 0);
    CHECK(double(agree) / double(total) > 0.95);

    // Unreal's Z is up: the wall's pieces sit within its 100 cm at rest, and the ones the blast threw are marked released.
    double top = -1e9;
    int released = 0;
    for (const json& piece : m["pieces"])
    {
        top = std::max(top, piece["position"][2].get<double>());
        released += piece["releaseFrame"].get<int>() >= 1 ? 1 : 0;
    }
    CHECK(top > 50.0);
    CHECK(top < 100.0);
    CHECK(released > 0);
    js.close();
    bf.close();
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}
