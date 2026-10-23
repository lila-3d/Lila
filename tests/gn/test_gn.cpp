#include <doctest/doctest.h>

#include "ops/Command.h"
#include "gn/Exec.h"
#include "gn/Gn.h"
#include "geo/Mesh.h"
#include "geo/ObjIO.h"
#include "scene/Graph.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }
int faces(const std::string& m) { return run("meshInfo", args({m}))["faces"].get<int>(); }
json bounds(const std::string& m) { return run("meshInfo", args({m})); }

std::string add(const std::string& g, const char* type) { return run("gnAddNode", args({g, type})).get<std::string>(); }
void link(const std::string& g, const std::string& from, const std::string& to) { run("gnConnect", args({g, from, to})); }
void set(const std::string& g, const std::string& plug, json v) { run("gnSetValue", args({g, plug, v})); }

/** Positions of a mesh's evaluated outMesh. */
std::vector<glm::dvec3> positions(const std::string& mesh)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(mesh);
    auto m = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
    return m ? std::vector<glm::dvec3>(m->positions().begin(), m->positions().end()) : std::vector<glm::dvec3>{};
}

/** Group input -> Set Position whose offset is up * height, with height exposed. */
std::string heightGroup(const char* name)
{
    run("gnCreateGroup", args({name}));
    std::string sp = add(name, "SetPosition"), xyz = add(name, "CombineXYZ");
    run("gnDisconnect", args({name, "GroupOutput.geometry"}));
    link(name, "GroupInput.geometry", sp + ".geometry");
    link(name, sp + ".geometry", "GroupOutput.geometry");
    link(name, xyz + ".vector", sp + ".offset");
    run("gnExposeInput", args({name, xyz + ".y", "height"}));
    return name;
}
} // namespace

TEST_CASE("distribute points on a grid and instance cubes")
{
    run("newScene");
    std::string mesh = run("polyPlane", {{"name", "Ground"}})[0];
    run("gnCreateGroup", args({"Scatter"}));
    std::string grid = add("Scatter", "MeshGrid"), dist = add("Scatter", "DistributePointsOnFaces"), cube = add("Scatter", "MeshCube"),
                iop = add("Scatter", "InstanceOnPoints"), real = add("Scatter", "RealizeInstances");
    set("Scatter", grid + ".sizeX", 400);
    set("Scatter", grid + ".sizeY", 400);
    set("Scatter", dist + ".density", 10);
    set("Scatter", dist + ".seed", 3);
    set("Scatter", cube + ".size", json::array({5, 5, 5}));
    link("Scatter", grid + ".mesh", dist + ".mesh");
    link("Scatter", dist + ".points", iop + ".points");
    link("Scatter", cube + ".mesh", iop + ".instance");
    link("Scatter", dist + ".rotation", iop + ".rotation");
    link("Scatter", iop + ".instances", real + ".geometry");
    link("Scatter", real + ".geometry", "GroupOutput.geometry");

    // Count the distributed points straight from the evaluator.
    scene::Graph& g = scene::Graph::current();
    gn::Group grp = gn::readGroup(*gn::findGroupNode(g, "Scatter"));
    gn::Group probe = grp;
    std::erase_if(probe.links, [](const gn::GLink& l) { return l.toNode == "GroupOutput"; });
    probe.links.push_back({dist, "points", "GroupOutput", "geometry"});
    gn::GeometryPtr pts = gn::execute(*gn::compile(probe), {});
    REQUIRE(pts->points);
    size_t n = pts->points->numPoints();
    // 16 m2 at 10 per m2.
    CHECK(n > 120);
    CHECK(n < 200);

    std::string mod = run("gnApply", args({mesh, "Scatter"}));
    CHECK(run("modifiers", args({mesh})) == json::array({mod}));
    CHECK(faces(mesh) == int(6 * n));

    // Same seed, same result.
    std::vector<glm::dvec3> a = positions(mesh);
    set("Scatter", dist + ".seed", 4);
    std::vector<glm::dvec3> b = positions(mesh);
    set("Scatter", dist + ".seed", 3);
    CHECK(positions(mesh) == a);
    CHECK(positions(mesh) != b);

    run("undo");
    CHECK(positions(mesh) == b);
}

TEST_CASE("set position with a noise field is deterministic")
{
    run("newScene");
    std::string mesh = run("polyPlane", {{"name", "Terrain"}, {"subdivisionsX", 20}, {"subdivisionsZ", 20}})[0];
    run("gnCreateGroup", args({"Bumps"}));
    std::string sp = add("Bumps", "SetPosition"), noise = add("Bumps", "NoiseTexture"), vm = add("Bumps", "VectorMath");
    set("Bumps", vm + ".operation", "scale");
    run("gnDisconnect", args({"Bumps", "GroupOutput.geometry"}));
    link("Bumps", "GroupInput.geometry", sp + ".geometry");
    link("Bumps", sp + ".geometry", "GroupOutput.geometry");
    std::string normal = add("Bumps", "Normal");
    link("Bumps", normal + ".value", vm + ".a");
    link("Bumps", noise + ".fac", vm + ".scale");
    link("Bumps", vm + ".vector", sp + ".offset");

    std::vector<glm::dvec3> before = positions(mesh);
    std::string mod = run("gnApply", args({mesh, "Bumps"}));
    std::vector<glm::dvec3> after = positions(mesh);
    REQUIRE(after.size() == before.size());
    int moved = 0;
    for (size_t k = 0; k < after.size(); ++k)
    {
        CHECK(after[k].x == doctest::Approx(before[k].x));
        CHECK(after[k].y >= before[k].y);
        moved += after[k].y > before[k].y + 1e-6 ? 1 : 0;
    }
    CHECK(moved > int(after.size()) / 2);
    // Noise varies across the surface.
    double lo = 1e9, hi = -1e9;
    for (const glm::dvec3& p : after)
    {
        lo = std::min(lo, p.y);
        hi = std::max(hi, p.y);
    }
    CHECK(hi - lo > 0.05);

    // Re-evaluating from scratch gives the same positions.
    run("setAttr", args({mod + ".enabled", false}));
    run("setAttr", args({mod + ".enabled", true}));
    CHECK(positions(mesh) == after);
}

TEST_CASE("exposed inputs are keyable and re-evaluate")
{
    run("newScene");
    std::string mesh = run("polyCube", {{"name", "Box"}})[0];
    heightGroup("Lift");
    std::string mod = run("gnApply", args({mesh, "Lift"}));
    CHECK(bounds(mesh)["boundsMax"][1].get<double>() == doctest::Approx(50.0));
    run("setAttr", args({mod + ".height", 30.0}));
    CHECK(bounds(mesh)["boundsMax"][1].get<double>() == doctest::Approx(80.0));

    run("setKeyframe", {{"args", {mod + ".height"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {mod + ".height"}}, {"time", 1.0}, {"value", 100.0}});
    run("currentTime", args({0.0}));
    CHECK(bounds(mesh)["boundsMax"][1].get<double>() == doctest::Approx(50.0));
    run("currentTime", args({1.0}));
    CHECK(bounds(mesh)["boundsMax"][1].get<double>() == doctest::Approx(150.0));

    // Growing the interface re-registers the type and migrates the modifier, keeping its name, key and stack slot.
    json info = run("gnGroupInfo", args({"Lift"}));
    std::string xyz;
    for (const json& n : info["nodes"])
        if (n["type"] == "CombineXYZ")
            xyz = n["name"];
    std::string typeBefore = scene::Graph::current().find(mod)->type().name;
    run("gnExposeInput", args({"Lift", xyz + ".x", "slide"}));
    std::string typeAfter = scene::Graph::current().find(mod)->type().name;
    CHECK(typeBefore != typeAfter);
    CHECK(run("modifiers", args({mesh})) == json::array({mod}));
    CHECK(bounds(mesh)["boundsMax"][1].get<double>() == doctest::Approx(150.0));
    run("setAttr", args({mod + ".slide", 10.0}));
    CHECK(bounds(mesh)["boundsMax"][0].get<double>() == doctest::Approx(60.0));

    run("undo");
    run("undo");
    CHECK(scene::Graph::current().find(mod)->type().name == typeBefore);
    CHECK(bounds(mesh)["boundsMax"][1].get<double>() == doctest::Approx(150.0));
}

TEST_CASE("groups and their modifiers survive save and load")
{
    for (const char* ext : {".lila", ".lilaa"})
    {
        run("newScene");
        std::string mesh = run("polyCube", {{"name", "Crate"}})[0];
        heightGroup("Raise");
        std::string mod = run("gnApply", args({mesh, "Raise"}));
        run("gnExposeInput", args({"Raise", run("gnGroupInfo", args({"Raise"}))["nodes"][3]["name"].get<std::string>() + ".z", "push"}));
        run("setAttr", args({mod + ".height", 25.0}));
        double top = bounds(mesh)["boundsMax"][1].get<double>();
        CHECK(top == doctest::Approx(75.0));
        int f = faces(mesh);

        fs::path path = fs::temp_directory_path() / (std::string("lila_gn_scene") + ext);
        run("saveScene", args({path.string()}));
        run("newScene");
        CHECK(run("gnGroups").empty());
        run("openScene", args({path.string()}));
        CHECK(run("gnGroups") == json::array({"Raise"}));
        CHECK(run("modifiers", args({mesh})) == json::array({mod}));
        CHECK(run("getAttr", args({mod + ".height"})).get<double>() == doctest::Approx(25.0));
        CHECK(bounds(mesh)["boundsMax"][1].get<double>() == doctest::Approx(top));
        CHECK(faces(mesh) == f);
        // Still live after loading.
        run("setAttr", args({mod + ".height", 5.0}));
        CHECK(bounds(mesh)["boundsMax"][1].get<double>() == doctest::Approx(55.0));
    }
}

TEST_CASE("node library evaluates")
{
    run("newScene");
    run("gnCreateGroup", args({"Lib"}));
    scene::Graph& g = scene::Graph::current();
    // Every generator-like node feeds the output on its own without throwing.
    for (const char* type : {"MeshCube", "MeshGrid", "MeshUVSphere", "MeshIcoSphere", "MeshQuadSphere", "MeshCylinder", "MeshCone", "MeshTorus", "MeshCircle"})
    {
        gn::Group grp = gn::readGroup(*gn::findGroupNode(g, "Lib"));
        grp.nodes.push_back({"P", type, {0, 0}, json::object()});
        std::erase_if(grp.links, [](const gn::GLink& l) { return l.toNode == "GroupOutput"; });
        grp.links.push_back({"P", "mesh", "GroupOutput", "geometry"});
        gn::GeometryPtr r = gn::execute(*gn::compile(grp), {});
        CHECK_MESSAGE(r->mesh, type);
        if (r->mesh)
            CHECK_MESSAGE(r->mesh->numPoints() > 0, type);
    }

    // Curve circle swept along a spiral, filled ends.
    gn::Group grp = gn::Group::makeDefault("Tube");
    grp.links.clear();
    grp.nodes.push_back({"Spiral", "CurveSpiral", {0, 0}, json::object()});
    grp.nodes.push_back({"Circle", "CurveCircle", {0, 0}, {{"resolution", 8}, {"radius", 5.0}}});
    grp.nodes.push_back({"Sweep", "CurveToMesh", {0, 0}, {{"fillCaps", true}}});
    grp.links.push_back({"Spiral", "curve", "Sweep", "curve"});
    grp.links.push_back({"Circle", "curve", "Sweep", "profileCurve"});
    grp.links.push_back({"Sweep", "mesh", "GroupOutput", "geometry"});
    gn::GeometryPtr tube = gn::execute(*gn::compile(grp), {});
    REQUIRE(tube->mesh);
    CHECK(tube->mesh->numFaces() == (65 - 1) * 8 + 2);
    CHECK(tube->mesh->validate(geo::kValidateStructure).empty());

    // Boolean: a cube minus a smaller cube is still closed.
    gn::Group b = gn::Group::makeDefault("Cut");
    b.links.clear();
    b.nodes.push_back({"A", "MeshCube", {0, 0}, json::object()});
    b.nodes.push_back({"B", "MeshCube", {0, 0}, {{"size", {50, 200, 50}}}});
    b.nodes.push_back({"Bool", "MeshBoolean", {0, 0}, json::object()});
    b.links.push_back({"A", "mesh", "Bool", "meshA"});
    b.links.push_back({"B", "mesh", "Bool", "meshB"});
    b.links.push_back({"Bool", "mesh", "GroupOutput", "geometry"});
    gn::GeometryPtr cut = gn::execute(*gn::compile(b), {});
    REQUIRE(cut->mesh);
    CHECK(cut->mesh->numFaces() > 6);

    // Capture + delete: removing faces above y=0 by a compare on position.
    gn::Group d = gn::Group::makeDefault("Half");
    d.links.clear();
    d.nodes.push_back({"S", "MeshUVSphere", {0, 0}, json::object()});
    d.nodes.push_back({"P", "Position", {0, 0}, json::object()});
    d.nodes.push_back({"Sep", "SeparateXYZ", {0, 0}, json::object()});
    d.nodes.push_back({"Cmp", "Compare", {0, 0}, {{"operation", "greaterThan"}}});
    d.nodes.push_back({"Del", "DeleteGeometry", {0, 0}, {{"domain", "face"}}});
    d.links.push_back({"S", "mesh", "Del", "geometry"});
    d.links.push_back({"P", "value", "Sep", "vector"});
    d.links.push_back({"Sep", "y", "Cmp", "a"});
    d.links.push_back({"Cmp", "result", "Del", "selection"});
    d.links.push_back({"Del", "geometry", "GroupOutput", "geometry"});
    gn::GeometryPtr half = gn::execute(*gn::compile(d), {});
    REQUIRE(half->mesh);
    CHECK(half->mesh->numFaces() == 32 * 8);
    CHECK(half->mesh->bounds().max.y < 1e-6);
}
