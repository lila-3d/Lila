#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "geo/Mesh.h"
#include "math/MathCore.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/MeshNodes.h"

#include <ufbx.h>

#include <cmath>
#include <filesystem>

using namespace lila;
using namespace lila::scene;
using namespace fbxtest;
namespace fs = std::filesystem;

namespace
{
/** A source with a modifier, two instances placed apart, and a plain copy for contrast. */
void buildKit()
{
    math::setUpAxis(math::UpAxis::Y);
    run("newScene");
    run("polyCube", {{"name", "Bolt"}});
    run("addModifier", {{"args", {"Bolt"}}, {"kind", "bevel"}, {"params", {{"segments", 1}}}});
    run("instance", {{"args", {"Bolt"}}, {"name", "BoltA"}});
    run("instance", {{"args", {"Bolt"}}, {"name", "BoltB"}});
    run("move", {{"args", {"BoltA"}}, {"by", {100, 0, 0}}});
    run("rotate", {{"args", {"BoltB"}}, {"by", {0, 90, 0}}});
    run("duplicate", {{"args", {"Bolt"}}, {"name", "Copy"}});
}

std::vector<NodeId> ids(const std::vector<const char*>& names)
{
    std::vector<NodeId> out;
    for (const char* n : names)
        out.push_back(Graph::current().find(n)->id());
    return out;
}
} // namespace

TEST_CASE("fbx export writes one Geometry per instance group, connected to every member")
{
    buildKit();
    Graph& g = Graph::current();
    fs::path path = tempFile("instances.fbx");
    fbx::ExportOptions opts;
    std::string err;
    REQUIRE(fbx::writeFbx(fbx::gatherExport(g, ids({"Bolt", "BoltA", "BoltB", "Copy"}), opts), path, opts, &err));

    UfbxScene s(path);
    REQUIRE(s.scene);
    // Bolt, BoltA and BoltB share one; the plain copy has its own.
    CHECK(s.scene->meshes.count == 2);
    const ufbx_node* bolt = ufbx_find_node(s.scene, "Bolt");
    const ufbx_node* a = ufbx_find_node(s.scene, "BoltA");
    const ufbx_node* b = ufbx_find_node(s.scene, "BoltB");
    const ufbx_node* copy = ufbx_find_node(s.scene, "Copy");
    REQUIRE((bolt && a && b && copy));
    REQUIRE(bolt->mesh);
    CHECK(a->mesh == bolt->mesh);
    CHECK(b->mesh == bolt->mesh);
    CHECK(copy->mesh != bolt->mesh);
    CHECK(bolt->mesh->instances.count == 3);
    // Each member keeps its own placement (in the file's axes).
    const ufbx_vec3 t = a->local_transform.translation;
    CHECK(std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z) == doctest::Approx(100.0));

    // Baked to world space the points differ, so each gets its own again.
    fs::path baked = tempFile("instances_world.fbx");
    opts.worldSpace = true;
    REQUIRE(fbx::writeFbx(fbx::gatherExport(g, ids({"Bolt", "BoltA", "BoltB"}), opts), baked, opts, &err));
    UfbxScene w(baked);
    REQUIRE(w.scene);
    CHECK(w.scene->meshes.count == 3);
}

TEST_CASE("fbx import turns shared geometry into Lila instances")
{
    buildKit();
    Graph& g = Graph::current();
    fs::path path = tempFile("instances_roundtrip.fbx");
    fbx::ExportOptions opts;
    std::string err;
    REQUIRE(fbx::writeFbx(fbx::gatherExport(g, ids({"Bolt", "BoltA", "BoltB", "Copy"}), opts), path, opts, &err));
    const int faces = run("meshInfo", {{"args", {"Bolt"}}})["faces"].get<int>();

    run("newScene");
    run("importFbx", {{"args", {path.string()}}});
    CHECK(instancing::kind(g, g.find("Bolt")->id()) == instancing::Kind::Source);
    CHECK(instancing::kind(g, g.find("BoltA")->id()) == instancing::Kind::Instance);
    CHECK(instancing::kind(g, g.find("BoltB")->id()) == instancing::Kind::Instance);
    CHECK(instancing::kind(g, g.find("Copy")->id()) == instancing::Kind::None);
    CHECK(run("meshInfo", {{"args", {"BoltB"}}})["faces"].get<int>() == run("meshInfo", {{"args", {"Bolt"}}})["faces"].get<int>());
    CHECK(faces > 6);
    CHECK(run("getAttr", {{"args", {"BoltA.tx"}}}).get<double>() == doctest::Approx(100.0));

    // instances=False keeps the old behaviour: separate nodes holding one shared snapshot.
    run("newScene");
    run("importFbx", {{"args", {path.string()}}, {"instances", false}});
    CHECK(instancing::kind(g, g.find("BoltA")->id()) == instancing::Kind::None);
    g.evaluate();
    CHECK(g.find("BoltA")->as<MeshPtr>(MeshNode::kInMesh) == g.find("Bolt")->as<MeshPtr>(MeshNode::kInMesh));
}

TEST_CASE("fbx export writes a live array's copies as models sharing the source's Geometry")
{
    math::setUpAxis(math::UpAxis::Y);
    run("newScene");
    run("polyCube", {{"name", "Spoke"}});
    run("move", {{"args", {"Spoke"}}, {"by", {100, 0, 0}}});
    run("array", {{"args", {"Spoke"}}, {"mode", "radial"}, {"count", 4}, {"center", {0, 0, 0}}, {"axis", "y"}, {"name", "Wheel"}});
    Graph& g = Graph::current();

    fbx::ExportOptions opts;
    fbx::ExportScene scene = fbx::gatherExport(g, ids({"Wheel"}), opts);
    // The array, its source and three generated copies under it, all on one mesh snapshot.
    REQUIRE(scene.nodes.size() == 5);
    int meshes = 0;
    for (const fbx::ExportNode& e : scene.nodes)
        if (e.mesh)
        {
            ++meshes;
            CHECK(e.mesh == scene.nodes[1].mesh);
            CHECK(e.parent == 0);
        }
    CHECK(meshes == 4);

    fs::path path = tempFile("array.fbx");
    std::string err;
    REQUIRE(fbx::writeFbx(scene, path, opts, &err));
    UfbxScene s(path);
    REQUIRE(s.scene);
    CHECK(s.scene->meshes.count == 1);
    const ufbx_node* spoke = ufbx_find_node(s.scene, "Spoke");
    const ufbx_node* half = ufbx_find_node(s.scene, "Spoke_2");
    REQUIRE((spoke && half));
    CHECK(half->mesh == spoke->mesh);
    CHECK(spoke->mesh->instances.count == 4);
    // Copy 2 of 4 is half way round the ring: the same distance from the centre, on the other side (in the file's axes).
    const ufbx_vec3 a = spoke->local_transform.translation, b = half->local_transform.translation;
    CHECK(std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z) == doctest::Approx(100.0));
    CHECK(b.x == doctest::Approx(-a.x).scale(1.0));
    CHECK(b.y == doctest::Approx(-a.y).scale(1.0));
    CHECK(b.z == doctest::Approx(-a.z).scale(1.0));
}
