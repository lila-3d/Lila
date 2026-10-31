#include <doctest/doctest.h>

#include "ops/Command.h"
#include "gn/Exec.h"
#include "gn/GeometryIO.h"
#include "gn/Gn.h"
#include "gn/Volume.h"
#include "gn/Zones.h"
#include "geo/Mesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>

using namespace lila;
using json = nlohmann::json;

namespace canvaszones
{
using N = gn::GNode;
using L = gn::GLink;
using T = gn::SocketType;

json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }

gn::PlanPtr compiled(const gn::Group& g)
{
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    return plan;
}

json items(std::initializer_list<std::pair<const char*, const char*>> list)
{
    json out = json::array();
    for (auto& [name, type] : list)
        out.push_back({{"name", name}, {"type", type}});
    return out;
}

/** A cube through a zone; the body moves it 1 along X and adds 1 to "count". */
gn::Group repeatZone(int iterations)
{
    gn::Group g = gn::Group::makeDefault("Outer");
    g.links.clear();
    g.outputs.push_back({.name = "count", .type = T::Float});
    g.nodes.push_back(N{"C", "MeshCube", {}, {}});
    g.nodes.push_back(N{"In", "RepeatInput", {}, {{"output", "Out"}, {"iterations", iterations}}});
    g.nodes.push_back(N{"Out", "RepeatOutput", {}, {{"items", items({{"count", "float"}})}}});
    g.nodes.push_back(N{"T", "TransformGeometry", {}, {{"translation", json::array({1, 0, 0})}}});
    g.nodes.push_back(N{"Add", "Math", {}, {{"b", 1.0}}});
    g.links = {L{"C", "mesh", "In", "geometry"},    L{"In", "geometry", "T", "geometry"}, L{"T", "geometry", "Out", "geometry"},
               L{"In", "count", "Add", "a"},        L{"Add", "value", "Out", "count"},    L{"Out", "geometry", "GroupOutput", "geometry"},
               L{"Out", "count", "GroupOutput", "count"}};
    return g;
}
} // namespace canvaszones

TEST_CASE("a repeat zone on the canvas runs its body each pass")
{
    using namespace canvaszones;
    gn::Group g = repeatZone(3);
    std::vector<gn::ZoneRegion> regions = gn::zoneRegions(g);
    REQUIRE(regions.size() == 1);
    CHECK(regions[0].body == std::vector<std::string>{"T", "Add"});

    auto out = gn::executeOutputs(*compiled(g), {});
    REQUIRE(out[0].geometry->mesh);
    CHECK(out[0].geometry->mesh->bounds().min.x == doctest::Approx(-47.0));
    CHECK(gn::evaluateSingle<double>(out[1].field) == doctest::Approx(3.0));
}

TEST_CASE("a zone body reads values from outside the zone")
{
    using namespace canvaszones;
    gn::Group g = repeatZone(4);
    g.nodes.push_back(N{"Step", "Value", {}, {{"value", 2.5}}});
    std::erase_if(g.links, [](const L& l) { return l.toNode == "Add" && l.toSocket == "b"; });
    g.links.push_back(L{"Step", "value", "Add", "b"});
    auto out = gn::executeOutputs(*compiled(g), {});
    CHECK(gn::evaluateSingle<double>(out[1].field) == doctest::Approx(10.0));
}

TEST_CASE("zones nest, and the iteration reaches an inner zone")
{
    using namespace canvaszones;
    gn::Group g = gn::Group::makeDefault("Nest");
    g.links.clear();
    g.outputs.push_back({.name = "total", .type = T::Float});
    g.nodes.push_back(N{"A", "RepeatInput", {}, {{"output", "AOut"}, {"iterations", 3}}});
    g.nodes.push_back(N{"AOut", "RepeatOutput", {}, {{"items", items({{"total", "float"}})}}});
    g.nodes.push_back(N{"B", "RepeatInput", {}, {{"output", "BOut"}, {"iterations", 2}}});
    g.nodes.push_back(N{"BOut", "RepeatOutput", {}, {{"items", items({{"total", "float"}})}}});
    g.nodes.push_back(N{"Add", "Math", {}, {}});
    g.nodes.push_back(N{"One", "Math", {}, {{"b", 1.0}}});
    // Outer passes 0, 1, 2; each runs the inner zone twice adding outer iteration + 1.
    g.links = {L{"A", "total", "B", "total"},    L{"B", "total", "Add", "a"},       L{"A", "iteration", "One", "a"},
               L{"One", "value", "Add", "b"},    L{"Add", "value", "BOut", "total"}, L{"BOut", "total", "AOut", "total"},
               L{"AOut", "total", "GroupOutput", "total"}};
    auto out = gn::executeOutputs(*compiled(g), {});
    CHECK(gn::evaluateSingle<double>(out[1].field) == doctest::Approx(2.0 * (1 + 2 + 3)));
}

TEST_CASE("a for-each zone runs per face")
{
    using namespace canvaszones;
    gn::Group g = gn::Group::makeDefault("Each");
    g.links.clear();
    g.nodes.push_back(N{"C", "MeshCube", {}, {}});
    g.nodes.push_back(N{"In", "ForEachInput", {}, {{"output", "Out"}, {"element", "face"}}});
    g.nodes.push_back(N{"Out", "ForEachOutput", {}, json::object()});
    g.nodes.push_back(N{"S", "ScaleElements", {}, {{"scale", 0.5}}});
    g.links = {L{"C", "mesh", "In", "geometry"}, L{"In", "geometry", "S", "mesh"}, L{"S", "mesh", "Out", "geometry"},
               L{"Out", "geometry", "GroupOutput", "geometry"}};
    auto out = gn::executeOutputs(*compiled(g), {});
    REQUIRE(out[0].geometry->mesh);
    CHECK(out[0].geometry->mesh->numFaces() == 6);
    CHECK(out[0].geometry->mesh->numPoints() == 24);
}

TEST_CASE("a simulation zone steps its state frame by frame")
{
    using namespace canvaszones;
    run("newScene");
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 10.0}, {"fps", 24.0}});
    const std::string mesh = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Box"}})[0];
    run("gnCreateGroup", args({"Sim"}));
    std::string in = run("gnAddNode", args({"Sim", "SimulationInput"})).get<std::string>();
    std::string out = run("gnGroupInfo", args({"Sim"}))["zones"][0]["output"].get<std::string>();
    std::string move = run("gnAddNode", args({"Sim", "TransformGeometry"})).get<std::string>();
    run("gnSetValue", args({"Sim", move + ".translation", json::array({0, 1, 0})}));
    run("gnDisconnect", args({"Sim", "GroupOutput.geometry"}));
    run("gnDisconnect", args({"Sim", out + ".geometry"}));
    run("gnConnect", args({"Sim", "GroupInput.geometry", in + ".geometry"}));
    run("gnConnect", args({"Sim", in + ".geometry", move + ".geometry"}));
    run("gnConnect", args({"Sim", move + ".geometry", out + ".geometry"}));
    run("gnConnect", args({"Sim", out + ".geometry", "GroupOutput.geometry"}));
    run("gnApply", args({mesh, "Sim"}));
    auto topY = [&] { return run("meshInfo", args({mesh}))["boundsMax"][1].get<double>(); };
    run("currentTime", args({1.0 / 24.0}));
    CHECK(topY() == doctest::Approx(50.0));
    run("currentTime", args({11.0 / 24.0}));
    CHECK(topY() == doctest::Approx(60.0));
    run("currentTime", args({5.0 / 24.0}));
    CHECK(topY() == doctest::Approx(54.0));
}

TEST_CASE("a Bake node saves what reaches it and reads it back")
{
    using namespace canvaszones;
    run("newScene");
    const std::string mesh = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Box"}})[0];
    run("gnCreateGroup", args({"B"}));
    std::string move = run("gnAddNode", args({"B", "TransformGeometry"})).get<std::string>();
    std::string bake = run("gnAddNode", args({"B", "Bake"})).get<std::string>();
    run("gnSetItems", args({"B", bake, "items", items({{"height", "float"}})}));
    run("gnSetValue", args({"B", bake + ".height", 7.0}));
    run("gnSetValue", args({"B", move + ".translation", json::array({0, 10, 0})}));
    run("gnDisconnect", args({"B", "GroupOutput.geometry"}));
    run("gnConnect", args({"B", "GroupInput.geometry", move + ".geometry"}));
    run("gnConnect", args({"B", move + ".geometry", bake + ".geometry"}));
    run("gnConnect", args({"B", bake + ".geometry", "GroupOutput.geometry"}));
    run("gnApply", args({mesh, "B"}));
    auto topY = [&] { return run("meshInfo", args({mesh}))["boundsMax"][1].get<double>(); };
    CHECK(topY() == doctest::Approx(60.0));
    std::string dir = run("gnBake", args({"B", bake})).get<std::string>();
    CHECK(std::filesystem::exists(std::filesystem::path(dir) / "still.lnb"));
    // Upstream changes no longer reach the output while baked.
    run("gnSetValue", args({"B", move + ".translation", json::array({0, 30, 0})}));
    CHECK(topY() == doctest::Approx(60.0));
    run("gnDeleteBake", args({"B", bake}));
    CHECK(topY() == doctest::Approx(80.0));
    CHECK_FALSE(std::filesystem::exists(dir));
}

TEST_CASE("an unpaired zone node is a readable error")
{
    using namespace canvaszones;
    gn::Group g = repeatZone(1);
    g.node("In")->values.erase("output");
    CHECK(gn::compile(g)->error.find("no zone output") != std::string::npos);
}

TEST_CASE("a closure captures outside values and runs where it is evaluated")
{
    using namespace canvaszones;
    gn::Group g = gn::Group::makeDefault("Clo");
    g.links.clear();
    g.outputs.push_back({.name = "r", .type = T::Float});
    g.nodes.push_back(N{"In", "ClosureInput", {}, {{"output", "Out"}}});
    g.nodes.push_back(N{"Out", "ClosureOutput", {}, {{"inputs", items({{"x", "float"}})}, {"outputs", items({{"y", "float"}})}}});
    g.nodes.push_back(N{"K", "Value", {}, {{"value", 10.0}}});
    g.nodes.push_back(N{"Mul", "Math", {}, {{"operation", "multiply"}}});
    g.nodes.push_back(N{"Eval", "EvaluateClosure", {}, {{"inputs", items({{"x", "float"}})}, {"outputs", items({{"y", "float"}})}, {"x", 4.0}}});
    g.links = {L{"In", "x", "Mul", "a"}, L{"K", "value", "Mul", "b"}, L{"Mul", "value", "Out", "y"}, L{"Out", "closure", "Eval", "closure"},
               L{"Eval", "y", "GroupOutput", "r"}};
    auto out = gn::executeOutputs(*compiled(g), {});
    CHECK(gn::evaluateSingle<double>(out[1].field) == doctest::Approx(40.0));
}

TEST_CASE("bundles carry several values along one link")
{
    using namespace canvaszones;
    gn::Group g = gn::Group::makeDefault("Bun");
    g.links.clear();
    g.outputs.push_back({.name = "a", .type = T::Float});
    g.outputs.push_back({.name = "v", .type = T::Vector});
    g.outputs.push_back({.name = "missing", .type = T::Int});
    g.nodes.push_back(N{"Cb", "CombineBundle", {}, {{"items", items({{"a", "float"}, {"v", "vector"}})}, {"a", 3.0}, {"v", json::array({1, 2, 3})}}});
    g.nodes.push_back(N{"Cb2", "CombineBundle", {}, {{"items", items({{"a", "float"}})}, {"a", 7.0}}});
    g.nodes.push_back(N{"J", "JoinBundle", {}, json::object()});
    g.nodes.push_back(N{"Sb", "SeparateBundle", {}, {{"items", items({{"a", "float"}, {"v", "vector"}, {"missing", "int"}})}}});
    g.links = {L{"Cb", "bundle", "J", "bundle"}, L{"Cb2", "bundle", "J", "bundle"}, L{"J", "bundle", "Sb", "bundle"},
               L{"Sb", "a", "GroupOutput", "a"},  L{"Sb", "v", "GroupOutput", "v"},     L{"Sb", "missing", "GroupOutput", "missing"}};
    auto out = gn::executeOutputs(*compiled(g), {});
    CHECK(gn::evaluateSingle<double>(out[1].field) == doctest::Approx(7.0));
    CHECK(gn::evaluateSingle<glm::dvec3>(out[2].field).z == doctest::Approx(3.0));
    CHECK(gn::evaluateSingle<int64_t>(out[3].field) == 0);
}

TEST_CASE("matrix nodes compose, invert and transform")
{
    using namespace canvaszones;
    gn::Group g = gn::Group::makeDefault("Mat");
    g.links.clear();
    g.outputs.push_back({.name = "p", .type = T::Vector});
    g.outputs.push_back({.name = "back", .type = T::Vector});
    g.outputs.push_back({.name = "rot", .type = T::Rotation});
    g.outputs.push_back({.name = "det", .type = T::Float});
    g.nodes.push_back(N{"Ct", "CombineTransform", {}, {{"translation", json::array({10, 0, 0})}, {"rotation", json::array({0, 90, 0})}, {"scale", json::array({2, 2, 2})}}});
    g.nodes.push_back(N{"Tp", "TransformPoint", {}, {{"mode", "matrix"}, {"vector", json::array({1, 0, 0})}}});
    g.nodes.push_back(N{"Inv", "InvertMatrix", {}, json::object()});
    g.nodes.push_back(N{"Tb", "TransformPoint", {}, {{"mode", "matrix"}}});
    g.nodes.push_back(N{"St", "SeparateTransform", {}, json::object()});
    g.nodes.push_back(N{"Det", "MatrixDeterminant", {}, json::object()});
    g.links = {L{"Ct", "transform", "Tp", "transform"}, L{"Ct", "transform", "Inv", "matrix"},  L{"Inv", "matrix", "Tb", "transform"},
               L{"Tp", "vector", "Tb", "vector"},       L{"Ct", "transform", "St", "transform"}, L{"Ct", "transform", "Det", "matrix"},
               L{"Tp", "vector", "GroupOutput", "p"},   L{"Tb", "vector", "GroupOutput", "back"}, L{"St", "rotation", "GroupOutput", "rot"},
               L{"Det", "determinant", "GroupOutput", "det"}};
    auto out = gn::executeOutputs(*compiled(g), {});
    glm::dvec3 p = gn::evaluateSingle<glm::dvec3>(out[1].field);
    CHECK(p.x == doctest::Approx(10.0));
    CHECK(p.z == doctest::Approx(-2.0));
    glm::dvec3 back = gn::evaluateSingle<glm::dvec3>(out[2].field);
    CHECK(back.x == doctest::Approx(1.0));
    CHECK(std::abs(back.z) < 1e-9);
    CHECK(gn::evaluateSingle<glm::dvec3>(out[3].field).y == doctest::Approx(glm::radians(90.0)));
    CHECK(gn::evaluateSingle<double>(out[4].field) == doctest::Approx(8.0));
    CHECK(gn::linkable(T::Rotation, T::Matrix));
    CHECK_FALSE(gn::linkable(T::Matrix, T::Vector));
    CHECK_FALSE(gn::linkable(T::Bundle, T::Float));
}

TEST_CASE("geometry survives a round trip through bytes")
{
    std::vector<glm::dvec3> quad = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    geo::Mesh m = geo::Mesh::fromPolygons(quad, std::vector<geo::Index>{4}, std::vector<geo::Index>{0, 1, 2, 3});
    m.pointAttrs().getOrAdd<float>("heat")[2] = 5.0f;
    gn::Geometry g;
    g.mesh = std::make_shared<const geo::Mesh>(m);
    gn::CurveSet cs;
    std::vector<glm::dvec3> pts = {{0, 0, 0}, {0, 2, 0}};
    cs.addCurve(pts, false);
    g.curves = std::make_shared<const gn::CurveSet>(cs);
    auto inst = std::make_shared<gn::Instances>();
    inst->references.push_back(gn::meshGeometry(m));
    inst->add(glm::translate(glm::dmat4(1.0), glm::dvec3(5, 0, 0)), 0);
    g.instances = inst;
    gn::Volume v = gn::Volume::grid({{0, 0, 0}, {2, 2, 2}}, 1.0, gn::Volume::Kind::Density, 0.0f);
    v.values[1] = 0.75f;
    g.volume = std::make_shared<const gn::Volume>(v);

    gn::GeometryPtr back = gn::geometryFromJson(gn::fromBytes(gn::toBytes(gn::geometryToJson(g))));
    REQUIRE(back->mesh);
    CHECK(back->mesh->numFaces() == 1);
    CHECK(back->mesh->pointAttrs().get<float>("heat")[2] == doctest::Approx(5.0f));
    REQUIRE(back->curves);
    CHECK(back->curves->numPoints() == 2);
    REQUIRE(back->instances);
    CHECK(back->instances->transforms[0][3].x == doctest::Approx(5.0));
    REQUIRE(back->volume);
    CHECK(back->volume->values[1] == doctest::Approx(0.75f));
}

TEST_CASE("zone commands add, pair, copy and edit items")
{
    using namespace canvaszones;
    run("newScene");
    run("gnCreateGroup", args({"Z"}));
    std::string in = run("gnAddNode", args({"Z", "RepeatInput"})).get<std::string>();
    json info = run("gnGroupInfo", args({"Z"}));
    REQUIRE(info["zones"].size() == 1);
    std::string out = info["zones"][0]["output"].get<std::string>();
    run("gnSetItems", args({"Z", in, "items", items({{"count", "float"}})}));
    run("gnConnect", args({"Z", in + ".count", out + ".count"}));
    json call = args({"Z", out, "items", items({{"total", "float"}})});
    call["renames"] = {{"count", "total"}};
    run("gnSetItems", call);
    info = run("gnGroupInfo", args({"Z"}));
    bool relinked = false;
    for (const json& l : info["definition"]["links"])
        relinked = relinked || (l[0] == in && l[1] == "total" && l[2] == out && l[3] == "total");
    CHECK(relinked);
    CHECK_THROWS(run("gnSetItems", args({"Z", out, "items", items({{"geometry", "float"}})})));

    json copies = run("gnDuplicateNodes", args({"Z", in}));
    CHECK(copies.size() == 2);
    CHECK(run("gnGroupInfo", args({"Z"}))["zones"].size() == 2);
    run("gnDeleteNode", args({"Z", copies[1].get<std::string>()}));
    info = run("gnGroupInfo", args({"Z"}));
    CHECK(info["zones"].size() == 1);
    CHECK_FALSE(info.contains("error"));
}
