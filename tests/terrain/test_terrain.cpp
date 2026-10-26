#include "Terrain.h"

#include "extensions/ExtensionManager.h"
#include "geo/Heightfield.h"
#include "geo/Mesh.h"
#include "gn/Geometry.h"
#include "core/ImageIO.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

void loadTerrain()
{
    if (!gn::findDecl("HeightfieldGrid"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}

void newScene() { run("newScene"); }

/** A bumpy test heightfield. */
geo::Heightfield bumpy(int nx = 65, int nz = 49, double cell = 100.0, glm::dvec2 origin = {-3200.0, -2400.0})
{
    geo::Heightfield h = geo::makeHeightfield(nx, nz, cell, origin);
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
        {
            glm::dvec2 p = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(300.0 * std::sin(p.x / 700.0) * std::cos(p.y / 900.0) + 0.05 * p.x);
        }
    return h;
}
} // namespace

TEST_CASE("terrain: a heightfield covering an area has square cells, the asked resolution and its centre where asked")
{
    geo::Heightfield h = geo::makeHeightfieldCovering({1000.0, -500.0}, {20000.0, 10000.0}, 201, 5.0f);
    CHECK(h.nx == 201);
    CHECK(h.nz == 101);
    CHECK(h.cell == doctest::Approx(100.0));
    CHECK(h.origin.x + h.extent().x * 0.5 == doctest::Approx(1000.0));
    CHECK(h.origin.y + h.extent().y * 0.5 == doctest::Approx(-500.0));
    CHECK(h.range().x == doctest::Approx(5.0));
}

TEST_CASE("terrain: bilinear height matches samples and interpolates between them")
{
    geo::Heightfield h = bumpy();
    CHECK(h.heightAt(h.samplePos(10, 7).x, h.samplePos(10, 7).y) == doctest::Approx(h.at(10, 7)));
    glm::dvec2 mid = (h.samplePos(10, 7) + h.samplePos(11, 7)) * 0.5;
    CHECK(h.heightAt(mid.x, mid.y) == doctest::Approx((h.at(10, 7) + h.at(11, 7)) * 0.5).epsilon(1e-5));
    // Outside reads the edge.
    CHECK(h.heightAt(-1e9, h.samplePos(0, 3).y) == doctest::Approx(h.at(0, 3)));
    CHECK(h.normalAt(0.0, 0.0).y > 0.0);
}

TEST_CASE("terrain: heightfield to mesh is quads facing up with uvs and masks, and bakes back within half a cell")
{
    geo::Heightfield h = bumpy();
    std::vector<float>& rock = h.mask("rock");
    for (size_t k = 0; k < rock.size(); ++k)
        rock[k] = float(k % 7) / 7.0f;
    geo::Mesh m = geo::heightfieldToMesh(h);
    CHECK(m.numPoints() == h.size());
    CHECK(m.numFaces() == size_t(h.nx - 1) * size_t(h.nz - 1));
    CHECK(m.validate().empty());
    for (geo::Index f = 0; f < 50; ++f)
        CHECK(m.faceNormal(f).y > 0.0);
    CHECK(m.pointAttrs().has("rock"));
    CHECK(m.cornerAttrs().has(geo::attr::uvName(0)));

    geo::Heightfield back = geo::meshToHeightfield(m, h.nx, h.nz, h.cell, h.origin, -1e6f);
    double worst = 0.0;
    for (size_t k = 0; k < h.size(); ++k)
        worst = std::max(worst, double(std::abs(back.heights[k] - h.heights[k])));
    CHECK(worst < 1e-2);

    geo::HeightfieldMeshOptions coarse;
    coarse.step = 4;
    geo::Mesh c = geo::heightfieldToMesh(h, coarse);
    geo::Bounds bb;
    for (geo::Index p = 0; p < c.pointCapacity(); ++p)
        bb.extend(c.position(p));
    CHECK(bb.min.x == doctest::Approx(h.origin.x));
    CHECK(bb.max.x == doctest::Approx(h.origin.x + h.extent().x));
    CHECK(bb.max.z == doctest::Approx(h.origin.y + h.extent().y));
}

TEST_CASE("terrain: noise is seamless across neighbouring heightfields")
{
    terrain::NoiseParams p;
    p.seed = 7;
    geo::Heightfield a = geo::makeHeightfield(33, 33, 250.0, {0.0, 0.0});
    geo::Heightfield b = geo::makeHeightfield(33, 33, 250.0, {a.extent().x, 0.0});
    for (int z = 0; z < 33; ++z)
    {
        glm::dvec2 pa = a.samplePos(32, z), pb = b.samplePos(0, z);
        CHECK(pa.x == doctest::Approx(pb.x));
        CHECK(terrain::noiseHeight(p, pa.x, pa.y) == terrain::noiseHeight(p, pb.x, pb.y));
    }
    double lo = 1e30, hi = -1e30;
    for (int k = 0; k < 2000; ++k)
    {
        double v = terrain::noiseHeight(p, k * 137.0, k * -91.0);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    CHECK(lo >= 0.0);
    CHECK(hi <= p.amplitude);
    CHECK(hi - lo > p.amplitude * 0.2);
}

TEST_CASE("terrain: geometry keeps a heightfield under move and scale, and turns it into a mesh when rotated or joined twice")
{
    gn::GeometryPtr g = gn::heightfieldGeometry(bumpy());
    glm::dmat4 xf = glm::scale(glm::translate(glm::dmat4(1.0), {100.0, 50.0, -20.0}), {2.0, 3.0, 2.0});
    gn::Geometry moved = gn::transformed(*g, xf);
    REQUIRE(moved.heightfield);
    CHECK(moved.heightfield->cell == doctest::Approx(200.0));
    CHECK(moved.heightfield->at(5, 5) == doctest::Approx(g->heightfield->at(5, 5) * 3.0 + 50.0));
    CHECK(moved.heightfield->origin.x == doctest::Approx(g->heightfield->origin.x * 2.0 + 100.0));

    gn::Geometry turned = gn::transformed(*g, glm::rotate(glm::dmat4(1.0), 0.5, {0.0, 1.0, 0.0}));
    CHECK_FALSE(turned.heightfield);
    REQUIRE(turned.mesh);
    CHECK(turned.mesh->numPoints() == g->heightfield->size());

    const gn::GeometryPtr parts[] = {g, g};
    gn::Geometry both = gn::join(parts);
    CHECK(both.heightfield);
    REQUIRE(both.mesh);
    CHECK(both.mesh->numPoints() == g->heightfield->size());
    CHECK_FALSE(both.empty());

    geo::Mesh out = gn::toOutputMesh(*g);
    CHECK(out.numFaces() == size_t(g->heightfield->nx - 1) * size_t(g->heightfield->nz - 1));
}

TEST_CASE("terrain: .lilahf files round trip exactly, masks and placement included")
{
    geo::Heightfield h = bumpy();
    h.mask("snow")[3] = 0.75f;
    const auto file = std::filesystem::temp_directory_path() / "lila_terrain_roundtrip.lilahf";
    terrain::writeHeightfield(file, h);
    geo::Heightfield back = terrain::readHeightfield(file);
    CHECK(back.nx == h.nx);
    CHECK(back.nz == h.nz);
    CHECK(back.cell == h.cell);
    CHECK(back.origin == h.origin);
    CHECK(back.heights == h.heights);
    REQUIRE(back.findMask("snow"));
    CHECK((*back.findMask("snow"))[3] == 0.75f);
    std::filesystem::remove(file);
}

TEST_CASE("terrain: terrainCreate makes a noise terrain that its inputs reshape, and terrainInfo and terrainSample read it")
{
    loadTerrain();
    newScene();
    json t = run("terrainCreate", {{"size", {40000.0, 20000.0}}, {"resolution", 129}, {"amplitude", 5000.0}, {"seed", 3}});
    CHECK(t["resolution"] == json::array({129, 65}));
    CHECK(t["size"][0].get<double>() == doctest::Approx(40000.0));
    CHECK(t["heightRange"][0].get<double>() >= 0.0);
    CHECK(t["heightRange"][1].get<double>() <= 5000.0);
    CHECK(t["heightRange"][1].get<double>() - t["heightRange"][0].get<double>() > 500.0);

    const std::string mod = t["modifier"].get<std::string>();
    run("setAttr", {{"args", {mod + ".resolution", 257}}});
    json info = run("terrainInfo", {{"args", {t["mesh"]}}});
    CHECK(info["resolution"] == json::array({257, 129}));

    json pts = run("terrainSample", {{"args", {t["mesh"]}}, {"points", {{0.0, 0.0}, {1000.0, 0.0, -2000.0}, {1e7, 0.0}}}});
    REQUIRE(pts.size() == 3);
    CHECK(pts[0]["inside"] == true);
    CHECK(pts[2]["inside"] == false);
    CHECK(pts[1]["height"].get<double>() >= info["heightRange"][0].get<double>() - 1e-3);

    json profile = run("terrainSample", {{"args", {t["mesh"]}}, {"path", {{-10000.0, 0.0}, {10000.0, 0.0}, {10000.0, 5000.0}}}, {"step", 1000.0}});
    CHECK(profile.size() == 26);
    CHECK(profile.back()["distance"].get<double>() == doctest::Approx(25000.0));
    CHECK(profile.back()["z"].get<double>() == doctest::Approx(5000.0));

    run("setAttr", {{"args", {t["mesh"].get<std::string>() + ".translate", {0.0, 100.0, 0.0}}}});
    json lifted = run("terrainSample", {{"args", {t["mesh"]}}, {"points", {{1000.0, -2000.0}}}});
    CHECK(lifted[0]["height"].get<double>() == doctest::Approx(pts[1]["height"].get<double>() + 100.0));
}

TEST_CASE("terrain: Set Height reads fields per sample")
{
    loadTerrain();
    newScene();
    json t = run("terrainCreate", {{"size", {10000.0, 10000.0}}, {"resolution", 33}, {"flat", true}, {"name", "Field"}});
    json spec = {{"name", t["group"]},
                 {"nodes", json::array({{{"name", "Grid"}, {"type", "HeightfieldGrid"}, {"values", {{"sizeX", 10000.0}, {"sizeZ", 10000.0}, {"resolution", 33}}}},
                                        {{"name", "P"}, {"type", "Position"}},
                                        {{"name", "Sep"}, {"type", "SeparateXYZ"}},
                                        {{"name", "Set"}, {"type", "SetHeight"}}})},
                 {"links", json::array({"Grid.heightfield -> Set.heightfield", "P.value -> Sep.vector", "Sep.x -> Set.height",
                                        "Set.heightfield -> GroupOutput.geometry"})}};
    run("gnBuildGroup", {{"args", {spec}}});
    json s = run("terrainSample", {{"args", {t["mesh"]}}, {"points", {{1250.0, 0.0}, {-5000.0, 300.0}}}});
    CHECK(s[0]["height"].get<double>() == doctest::Approx(1250.0));
    CHECK(s[1]["height"].get<double>() == doctest::Approx(-5000.0));
}

TEST_CASE("terrain: terrainToMesh copies it and terrainBake swaps the graph for a file with the same heights")
{
    loadTerrain();
    newScene();
    json t = run("terrainCreate", {{"size", {20000.0, 20000.0}}, {"resolution", 65}, {"seed", 11}, {"name", "Baked"}});
    json m = run("terrainToMesh", {{"args", {t["mesh"]}}, {"step", 2}});
    CHECK(m["points"] == 33 * 33);
    CHECK(m["faces"] == 32 * 32);

    const auto file = std::filesystem::temp_directory_path() / "lila_terrain_bake_test.lilahf";
    json before = run("terrainSample", {{"args", {t["mesh"]}}, {"points", {{1234.0, -4321.0}}}});
    json b = run("terrainBake", {{"args", {t["mesh"]}}, {"file", file.generic_string()}});
    CHECK(std::filesystem::exists(file));
    CHECK(b["resolution"] == json::array({65, 65}));
    json after = run("terrainSample", {{"args", {t["mesh"]}}, {"points", {{1234.0, -4321.0}}}});
    CHECK(after[0]["height"].get<double>() == doctest::Approx(before[0]["height"].get<double>()));
    std::filesystem::remove(file);
}

TEST_CASE("terrain: images load as heights with the top row at -Z, and Image Texture samples them in a graph")
{
    // 3 x 2, red rising left to right on the top row; the bottom row is black.
    const std::vector<float> rgba = {0.0f, 0, 0, 1, 0.5f, 0, 0, 1, 1.0f, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    const auto file = std::filesystem::temp_directory_path() / "lila_terrain_image_test.png";
    image::writePng(file, 3, 2, rgba, 16);

    geo::Heightfield h = terrain::loadHeightfield(file, {0.0, 0.0}, 2000.0, 100.0, 1100.0, false);
    CHECK(h.nx == 3);
    CHECK(h.nz == 2);
    CHECK(h.cell == doctest::Approx(1000.0));
    CHECK(h.at(2, 0) == doctest::Approx(1100.0).epsilon(1e-4));
    CHECK(h.at(1, 0) == doctest::Approx(600.0).epsilon(1e-3));
    CHECK(h.at(2, 1) == doctest::Approx(100.0));
    CHECK(h.samplePos(0, 0).y < h.samplePos(0, 1).y);

    loadTerrain();
    newScene();
    json t = run("terrainCreate", {{"size", {1000.0, 1000.0}}, {"resolution", 5}, {"flat", true}, {"name", "Img"}});
    json spec = {{"name", t["group"]},
                 {"nodes", json::array({{{"name", "Grid"}, {"type", "HeightfieldGrid"}, {"values", {{"sizeX", 1000.0}, {"sizeZ", 1000.0}, {"resolution", 5}}}},
                                        {{"name", "Image"}, {"type", "ImageTexture"}, {"values", {{"file", file.generic_string()}, {"vector", {5.0 / 6.0, 0.75, 0.0}}}}},
                                        {{"name", "Set"}, {"type", "SetHeight"}}})},
                 {"links", json::array({"Grid.heightfield -> Set.heightfield", "Image.value -> Set.height", "Set.heightfield -> GroupOutput.geometry"})}};
    run("gnBuildGroup", {{"args", {spec}}});
    json info = run("terrainInfo", {{"args", {t["mesh"]}}});
    CHECK(info["heightRange"][0].get<double>() == doctest::Approx(1.0).epsilon(1e-4));
    std::filesystem::remove(file);
}
