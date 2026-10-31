#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <doctest/doctest.h>

using namespace lila;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

scene::MeshPtr outMesh(const std::string& path)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(path);
    REQUIRE(n);
    return n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

double width(const geo::Mesh& m) { return m.bounds().max.x - m.bounds().min.x; }

void reset()
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
}
} // namespace

TEST_CASE("clay commands: create, add, cut, group, refresh, undo")
{
    reset();
    const json made = run("createClay", {{"name", "Blob"}});
    const std::string object = made[0], mesh = made[1];
    scene::MeshPtr start = outMesh(mesh);
    REQUIRE(start);
    CHECK(width(*start) == doctest::Approx(20.0).epsilon(0.02));

    const std::string arm = run("addClayPrimitive", {{"args", {object}}, {"shape", "capsule"}, {"radius", 3.0}, {"endRadius", 3.0},
                                                     {"length", 20.0}, {"rotation", {0, 0, 90}}, {"position", {15, 0, 0}}});
    CHECK(width(*outMesh(mesh)) == doctest::Approx(38.0).epsilon(0.02));

    run("addClayPrimitive", {{"args", {arm}}, {"operation", "subtract"}, {"radius", 4.0}, {"position", {25, 0, 0}}, {"name", "Cut"}});
    const double cut = width(*outMesh(mesh));
    CHECK(cut < 37.0);

    // Grouping the arm with its cut keeps the cut off the body; it still trims the arm.
    const std::string group = run("groupClay", {{"args", {arm, "Cut"}}});
    CHECK(width(*outMesh(mesh)) == doctest::Approx(cut).epsilon(0.02));
    scene::Graph& g = scene::Graph::current();
    CHECK(g.find(group)->type().name == "clayGroup");

    UndoStack::get().undo();
    CHECK(g.find(group) == nullptr);
    CHECK(width(*outMesh(mesh)) == doctest::Approx(cut).epsilon(0.02));

    CHECK(run("refreshClay", {{"args", {arm}}}).size() == 1);
}

TEST_CASE("clay commands: symmetry, a bad voxel size warns, convert bakes a plain mesh")
{
    reset();
    const json made = run("createClay", {{"name", "Pair"}, {"symmetry", "x"}, {"empty", true}});
    const std::string object = made[0], mesh = made[1], surface = made[2];
    CHECK(made[3].is_null());
    run("addClayPrimitive", {{"args", {object}}, {"radius", 3.0}, {"position", {10, 0, 0}}});
    scene::MeshPtr m = outMesh(mesh);
    CHECK(m->bounds().min.x == doctest::Approx(-13.0).epsilon(0.02));

    scene::Graph& g = scene::Graph::current();
    scene::Node* s = g.find(surface);
    g.setValue({s->id(), s->type().attrIndex("voxelSize"), -1}, 0.01);
    run("addClayPrimitive", {{"args", {object}}, {"radius", 900.0}});
    g.evaluate();
    CHECK_FALSE(s->as<std::string>(s->type().attrIndex("warning")).empty());
    UndoStack::get().undo();
    g.setValue({s->id(), s->type().attrIndex("voxelSize"), -1}, 0.5);
    g.evaluate();
    CHECK(s->as<std::string>(s->type().attrIndex("warning")).empty());

    const std::string baked = run("convertClayToMesh", {{"args", {object}}, {"voxelSize", 0.5}});
    CHECK(g.find(surface) == nullptr);
    scene::MeshPtr out = outMesh(baked);
    REQUIRE(out);
    CHECK(out->faceCapacity() > 0);
    CHECK(!g.source({g.find(baked)->id(), scene::MeshNode::kInMesh, -1}));
}
