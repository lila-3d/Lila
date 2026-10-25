#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "scene/MeshNodes.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json ui(const char* name, json call = json::object()) { return ops::execute(name, call, CallSource::UI); }

const geo::Mesh& meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    return *m;
}

int64_t upAxisOf(const std::string& generator)
{
    scene::Node* n = scene::Graph::current().find(generator);
    REQUIRE(n);
    return n->as<int64_t>(n->type().attrIndex("upAxis"));
}

glm::dvec3 faceNormal(const geo::Mesh& m)
{
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            return m.faceNormal(f);
    return glm::dvec3(0.0);
}
} // namespace

TEST_CASE("primitive upAxis turns the geometry to each axis")
{
    run("newScene");
    struct Case
    {
        const char* axis;
        glm::dvec3 dir;
    };
    for (const Case& c : {Case{"x", {1, 0, 0}}, Case{"y", {0, 1, 0}}, Case{"z", {0, 0, 1}}, Case{"-x", {-1, 0, 0}}, Case{"-y", {0, -1, 0}},
                          Case{"-z", {0, 0, -1}}})
    {
        CAPTURE(c.axis);
        // Base at the origin, so the side it rises toward shows the sign.
        json r = run("polyCylinder", {{"radius", 10.0}, {"height", 100.0}, {"baseAtOrigin", true}, {"upAxis", c.axis}});
        geo::Bounds b = meshOf(r[0]).bounds();
        const glm::dvec3 size = b.size();
        CHECK(glm::dot(size, glm::abs(c.dir)) == doctest::Approx(100.0));
        CHECK(size.x + size.y + size.z == doctest::Approx(140.0).epsilon(0.01));
        const glm::dvec3 centre = (b.min + b.max) * 0.5;
        CHECK(glm::dot(centre, c.dir) == doctest::Approx(50.0));

        // A plane faces along its up, so the turn keeps the winding.
        json plane = run("polyPlane", {{"upAxis", c.axis}});
        CHECK(glm::dot(faceNormal(meshOf(plane[0])), c.dir) == doctest::Approx(1.0));
    }
}

TEST_CASE("every primitive has upAxis, defaulting to the scene up axis")
{
    run("newScene");
    for (const char* type : {"polyCube", "polySphere", "polyCylinder", "polyCapsule", "polyCone", "polyPlane", "polyTorus", "polyDisc", "polyPipe",
                             "polyQuadSphere", "polyIcoSphere"})
    {
        CAPTURE(type);
        json r = run(type);
        CHECK(upAxisOf(r[1]) == 1);
        json z = run(type, {{"upAxis", "z"}});
        CHECK(upAxisOf(z[1]) == 2);
        const geo::Bounds a = meshOf(r[0]).bounds(), b = meshOf(z[0]).bounds();
        CHECK(a.size().y == doctest::Approx(b.size().z));
    }
    CHECK_THROWS_AS(run("polyCube", {{"upAxis", "w"}}), CommandError);

    math::setUpAxis(math::UpAxis::Z);
    json r = run("polyCone");
    math::setUpAxis(math::UpAxis::Y);
    CHECK(upAxisOf(r[1]) == 2);
}

TEST_CASE("primitive upAxis survives saving, and files without it load as y")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "lila_tests";
    fs::create_directories(dir);
    for (const char* ext : {".lila", ".lilaa"})
    {
        CAPTURE(ext);
        run("newScene");
        json r = run("polyTorus", {{"upAxis", "-x"}});
        const glm::dvec3 before = meshOf(r[0]).bounds().size();
        const std::string path = (dir / (std::string("prim_axis") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(upAxisOf(r[1]) == 3);
        const glm::dvec3 after = meshOf(r[0]).bounds().size();
        CHECK(after.x == doctest::Approx(before.x));
        CHECK(after.y == doctest::Approx(before.y));
    }

    // An older file has no upAxis line on the generator.
    const std::string path = (dir / "prim_axis.lilaa").string();
    std::stringstream text;
    text << std::ifstream(path).rdbuf();
    const std::string stripped = std::regex_replace(text.str(), std::regex("\n[ \t]+upAxis [^\n]*"), "");
    REQUIRE(stripped != text.str());
    const std::string old = (dir / "prim_axis_old.lilaa").string();
    std::ofstream(old) << stripped;
    run("newScene");
    run("openScene", {{"args", {old}}});
    CHECK(upAxisOf("PolyTorus") == 1);
    CHECK(meshOf("Torus").bounds().size().y == doctest::Approx(40.0));
}

TEST_CASE("a primitive created from the UI is adjustable, upAxis included")
{
    scene::lastOp::install();
    run("newScene");
    json r = ui("polyTorus", {{"position", {10, 0, 0}}});
    REQUIRE(scene::lastOp::live());
    REQUIRE(scene::lastOp::current()->command == "polyTorus");
    CHECK(scene::lastOp::current()->call["upAxis"] == "y");
    for (const FlagDef* f : scene::lastOp::editableFlags("polyTorus"))
        if (f->name == "radius")
            CHECK(scene::lastOp::flagValue(*scene::lastOp::current(), *f) == 50.0);

    scene::lastOp::rerun({{"upAxis", "z"}, {"radius", 80.0}});
    json gens = run("ls", {{"type", "polyTorus"}});
    REQUIRE(gens.size() == 1);
    CHECK(upAxisOf(gens[0]) == 2);
    const geo::Bounds b = meshOf(r[0]).bounds();
    CHECK(b.size().x == doctest::Approx(200.0));
    CHECK(b.size().z == doctest::Approx(40.0));
    CHECK(run("getAttr", {{"args", {std::string(r[0]) + ".translateX"}}}).get<double>() == doctest::Approx(10.0));

    run("undo");
    gens = run("ls", {{"type", "polyTorus"}});
    REQUIRE(gens.size() == 1);
    CHECK(upAxisOf(gens[0]) == 1);
    run("undo");
    CHECK(run("ls", {{"type", "polyTorus"}}).empty());
}

TEST_CASE("placing a primitive turns the scene up to the surface normal")
{
    run("newScene");
    json r = run("polyCylinder", {{"height", 100.0}, {"radius", 10.0}, {"upAxis", "x"}, {"position", {0, 0, 0}}, {"normal", {0, 0, 1}}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    // The cylinder's axis is x in object space; standing its y on +z leaves the axis along world x.
    const glm::dmat4 w = g.find(r[0])->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dvec3 axis = glm::dvec3(w * glm::dvec4(1, 0, 0, 0));
    CHECK(std::abs(axis.x) == doctest::Approx(1.0));
}
