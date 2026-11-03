#include <doctest/doctest.h>

#include "TestUtil.h"

#include "io/FileTranslators.h"
#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "scene/MeshData.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

TEST_CASE("primitives are procedural until history is deleted")
{
    run("newScene");
    json r = run("polyCube", {{"height", 100.0}, {"depth", 100.0}, {"width", 200}, {"subdivisionsX", 2}});
    std::string mesh = r[0], gen = r[1];
    CHECK(mesh == "Cube");
    json info = run("meshInfo", {{"args", {mesh}}});
    CHECK(info["faces"] == 10); // 2 columns on four sides, 1 on the two ends
    CHECK(info["boundsMax"][0].get<double>() == doctest::Approx(100));
    run("setAttr", {{"args", {gen + ".subdivisionsY", 3}}});
    CHECK(run("meshInfo", {{"args", {mesh}}})["faces"] == 22);
    run("undo");
    CHECK(run("meshInfo", {{"args", {mesh}}})["faces"] == 10);
    run("bakeModifierStack", {{"args", {mesh}}});
    CHECK(!run("objExists", {{"args", {gen}}}).get<bool>());
    CHECK(run("meshInfo", {{"args", {mesh}}})["faces"] == 10);
    json baked = run("polySphere", {{"radius", 50.0}, {"generator", false}});
    CHECK(baked[1].is_null());
}

TEST_CASE("mesh json round trip keeps attributes")
{
    geo::Mesh cube = geo::makeCube();
    json j = scene::meshToJson(cube);
    scene::MeshPtr back = scene::meshFromJson(j);
    CHECK(back->numFaces() == cube.numFaces());
    CHECK(back->numPoints() == cube.numPoints());
    CHECK(scene::meshToJson(*back) == j);
}

TEST_CASE("mesh json round trip keeps loose edges")
{
    geo::Mesh m = geo::makeCube();
    const geo::Index a = m.addPoint({0, 300, 0}), b = m.addPoint({200, 300, 0}), c = m.addPoint({200, 300, 200});
    m.addEdge(a, b);
    m.addEdge(b, c);
    for (const json& j : {scene::meshToJson(m), scene::meshToJsonPacked(m)})
    {
        scene::MeshPtr back = scene::meshFromJson(j);
        CHECK(back->numEdges() == m.numEdges());
        CHECK(back->findEdge(a, b) != geo::kInvalid);
        CHECK(back->findEdge(b, c) != geo::kInvalid);
    }
}

TEST_CASE("meshes save and load in both formats")
{
    run("newScene");
    run("polyTorus", {{"radius", 50.0}, {"sectionRadius", 20.0}, {"name", "Ring"}});
    run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Box"}, {"generator", false}});
    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    for (const char* ext : {".lila", ".lilaa"})
    {
        auto path = (dir / (std::string("meshes") + ext)).string();
        json before = run("meshInfo", {{"args", {"Ring"}}});
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(run("meshInfo", {{"args", {"Ring"}}}) == before);
        CHECK(run("meshInfo", {{"args", {"Box"}}})["faces"] == 6);
    }
}

TEST_CASE("setMeshPoints moves every point in one undo step")
{
    run("newScene");
    std::string mesh = run("polyCube", {{"width", 2}, {"height", 2}, {"depth", 2}})[0];
    json before = run("meshData", {{"args", {mesh}}})["points"];
    json moved = json::array();
    for (const json& p : before)
        moved.push_back({p[0].get<double>() * 2.0, p[1].get<double>() + 5.0, p[2]});
    CHECK(run("setMeshPoints", {{"args", {mesh}}, {"points", moved}}) == int64_t(before.size()));
    CHECK(run("meshData", {{"args", {mesh}}})["points"] == moved);
    CHECK(run("meshInfo", {{"args", {mesh}}})["faces"] == 6);
    run("undo");
    CHECK(run("meshData", {{"args", {mesh}}})["points"] == before);
    CHECK_THROWS(run("setMeshPoints", {{"args", {mesh}}, {"points", json::array({{0, 0, 0}})}}));
}

TEST_CASE("obj export and import")
{
    run("newScene");
    run("polyCylinder", {{"radius", 50.0}, {"height", 100.0}, {"name", "Can"}, {"position", {0, 50, 0}}});
    auto path = (std::filesystem::temp_directory_path() / "lila_tests" / "can.obj").string();
    run("exportObj", {{"args", {path, "Can"}}});
    run("newScene");
    json names = run("importObj", {{"args", {path}}});
    REQUIRE(names.size() == 1);
    json info = run("meshInfo", {{"args", {names[0]}}});
    CHECK(info["boundsMin"][1].get<double>() == doctest::Approx(0).epsilon(1e-6));
}

TEST_CASE("stl and ply export and import through the translators")
{
    namespace tr = io::translators;
    tr::registerBuiltins();
    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    std::filesystem::create_directories(dir);
    for (const char* ext : {".stl", ".ply"})
    {
        CAPTURE(ext);
        run("newScene");
        run("polyCylinder", {{"radius", 50.0}, {"height", 100.0}, {"name", "Can"}, {"position", {0, 50, 0}}});
        const std::string path = (dir / (std::string("can") + ext)).string();
        CHECK(tr::forPath(tr::Op::Import, path)->id == std::string(ext + 1));
        tr::run(tr::Op::Export, path);
        run("newScene");
        json names = tr::run(tr::Op::Import, path);
        REQUIRE(names.size() == 1);
        json info = run("meshInfo", {{"args", {names[0]}}});
        CHECK(info["boundsMin"][1].get<double>() == doctest::Approx(0).epsilon(1e-4));
        CHECK(info["boundsMax"][1].get<double>() == doctest::Approx(100).epsilon(1e-4));
    }
}

TEST_CASE("stl units and up axis")
{
    auto path = (std::filesystem::temp_directory_path() / "lila_tests" / "box.stl").string();
    run("newScene");
    run("polyCube", {{"name", "Box"}, {"width", 10}, {"height", 20}, {"depth", 30}, {"position", {0, 10, 0}}});
    run("exportStl", {{"args", {path, "Box"}}});
    run("newScene");
    // Read as centimetres and Y up: the file's millimetres come in ten times larger, its Z up lying along Y.
    json names = run("importStl", {{"args", {path}}, {"unit", "cm"}, {"upAxis", "y"}});
    json info = run("meshInfo", {{"args", {names[0]}}});
    CHECK(info["boundsMax"][2].get<double>() - info["boundsMin"][2].get<double>() == doctest::Approx(200.0));
    CHECK(info["boundsMax"][1].get<double>() - info["boundsMin"][1].get<double>() == doctest::Approx(300.0));
}
