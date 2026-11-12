#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <filesystem>
#include <fstream>

using namespace lila;
namespace fs = std::filesystem;

TEST_CASE("a photogrammetry FBX with an offset and .prj lands on its grid")
{
    scene::georef::setCurrent(std::nullopt);
    const fs::path dir = fs::temp_directory_path() / "lila_fbx_georef";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // A cube 1 m east, 2 m north and 3 m up of the export's origin, written as the scan tool would (east, north, up).
    const bool zUp = scene::georef::sceneZUp();
    const json cube = ops::execute("polyCube", {{"width", 20.0}});
    const std::string name = cube.is_array() ? cube[0].get<std::string>() : cube.get<std::string>();
    ops::execute("setAttr", {{"args", {name + ".translate", zUp ? json{100.0, 200.0, 300.0} : json{100.0, 300.0, -200.0}}}});
    ops::execute("exportFbx", {{"args", {(dir / "scan.fbx").string(), name}}});
    ops::execute("delete", {{"args", {name}}});
    std::ofstream(dir / "scan_offset.xyz") << "500000 4649776 100";
    std::ofstream(dir / "scan.prj") << scene::georef::wkt("EPSG:32633");

    const json r = ops::execute("importFbx", {{"args", {(dir / "scan.fbx").string()}}, {"report", true}});
    const auto geo = scene::georef::current();
    REQUIRE(geo);
    CHECK(geo->origin == glm::dvec3(500000.0, 4649776.0, 100.0));
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const scene::Node* n = g.find(r["roots"][0].get<std::string>());
    REQUIRE(n);
    const glm::dvec3 at(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
    const glm::dvec3 p = scene::georef::toProjected(*geo, at, zUp);
    CHECK(p.x == doctest::Approx(500001.0).epsilon(1e-9));
    CHECK(p.y == doctest::Approx(4649778.0).epsilon(1e-9));
    CHECK(p.z == doctest::Approx(103.0).epsilon(1e-9));
    scene::georef::setCurrent(std::nullopt);
}
