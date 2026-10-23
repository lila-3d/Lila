#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "scene/MeshData.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
} // namespace

TEST_CASE("primitives are procedural until history is deleted")
{
    run("newScene");
    json r = run("polyCube", {{"width", 200}, {"subdivisionsX", 2}});
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
    json baked = run("polySphere", {{"generator", false}});
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

TEST_CASE("meshes save and load in both formats")
{
    run("newScene");
    run("polyTorus", {{"name", "Ring"}});
    run("polyCube", {{"name", "Box"}, {"generator", false}});
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

TEST_CASE("obj export and import")
{
    run("newScene");
    run("polyCylinder", {{"name", "Can"}, {"position", {0, 50, 0}}});
    auto path = (std::filesystem::temp_directory_path() / "lila_tests" / "can.obj").string();
    run("exportObj", {{"args", {path, "Can"}}});
    run("newScene");
    json names = run("importObj", {{"args", {path}}});
    REQUIRE(names.size() == 1);
    json info = run("meshInfo", {{"args", {names[0]}}});
    CHECK(info["boundsMin"][1].get<double>() == doctest::Approx(0).epsilon(1e-6));
}
