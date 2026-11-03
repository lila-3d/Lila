#include "Erosion.h"
#include "Export.h"
#include "Inputs.h"
#include "Terrain.h"

#include "extensions/ExtensionManager.h"
#include "geo/Heightfield.h"
#include "geo/Mesh.h"
#include "gn/Geometry.h"
#include "core/ImageIO.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "tex/Graph.h"

#include <doctest/doctest.h>
#include <miniz.h>
#include <glm/gtc/matrix_transform.hpp>

#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

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

TEST_CASE("terrain: Evaluate Texture Graph writes a channel into the height or a mask")
{
    tex::Graph g = tex::Graph::makeMaterial("Flat");
    g.nodes.front().values = {{"height", 0.25}, {"roughness", 0.75}};
    const auto file = std::filesystem::temp_directory_path() / "lila_terrain_graph_test.lilamat";
    std::ofstream(file, std::ios::binary) << json{{"format", "lilamat"}, {"version", 1}, {"name", "Flat"}, {"graph", g.toJson()}}.dump();

    loadTerrain();
    newScene();
    json t = run("terrainCreate", {{"size", {1000.0, 1000.0}}, {"resolution", 9}, {"flat", true}, {"name", "Tex"}});
    json spec = {{"name", t["group"]},
                 {"nodes", json::array({{{"name", "Grid"}, {"type", "HeightfieldGrid"}, {"values", {{"sizeX", 1000.0}, {"sizeZ", 1000.0}, {"resolution", 9}}}},
                                        {{"name", "H"}, {"type", "TerrainTextureGraph"}, {"props", {{"backend", "cpu"}}},
                                         {"values", {{"file", file.generic_string()}, {"low", 100.0}, {"high", 500.0}}}},
                                        {{"name", "M"}, {"type", "TerrainTextureGraph"}, {"props", {{"backend", "cpu"}}},
                                         {"values", {{"file", file.generic_string()}, {"channel", "roughness"}, {"target", "rock"}, {"low", 0.0}, {"high", 1.0}}}}})},
                 {"links", json::array({"Grid.heightfield -> H.heightfield", "H.heightfield -> M.heightfield", "M.heightfield -> GroupOutput.geometry"})}};
    run("gnBuildGroup", {{"args", {spec}}});
    json info = run("terrainInfo", {{"args", {t["mesh"]}}});
    CHECK(info["heightRange"][0].get<double>() == doctest::Approx(200.0).epsilon(1e-3));
    CHECK(info["heightRange"][1].get<double>() == doctest::Approx(200.0).epsilon(1e-3));
    REQUIRE(info["masks"].contains("rock"));
    CHECK(info["masks"]["rock"]["mean"].get<double>() == doctest::Approx(0.75).epsilon(1e-3));
    std::filesystem::remove(file);
}

TEST_CASE("terrain: Unreal layouts fit the grid with the least waste, and the r16 decodes back to the heights in Unreal's axes")
{
    terrain::UnrealLayout l = terrain::chooseUnrealLayout(505, 505);
    CHECK(l.vertsX() == 505);
    CHECK(l.vertsY() == 505);
    l = terrain::chooseUnrealLayout(1000, 300);
    CHECK(l.vertsX() >= 1000);
    CHECK(l.vertsY() >= 300);
    CHECK(l.vertsX() - 1000 < l.quadsPerSection * l.sectionsPerComponent);

    // 127 x 64 quads: a size Unreal takes as is (one 63-quad section, two sections, one by two components... any fit).
    geo::Heightfield h = bumpy(127, 253, 100.0, {-6300.0, -12600.0});
    const auto dir = std::filesystem::temp_directory_path() / "lila_terrain_export_test";
    std::filesystem::remove_all(dir);
    json s = terrain::exportUnreal(h, glm::dmat4(1.0), dir, "T", {});
    const int w = s["resolution"][0].get<int>(), rows = s["resolution"][1].get<int>();
    CHECK(w >= h.nz);
    CHECK(rows >= h.nx);
    CHECK(s["clipped"] == 0);
    std::ifstream f(s["files"]["height"].get<std::string>(), std::ios::binary);
    std::vector<uint16_t> raw(size_t(w) * size_t(rows));
    f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size() * 2));
    REQUIRE(f);
    const double scaleZ = s["scale"][2].get<double>(), z = s["location"][2].get<double>(), cell = s["scale"][0].get<double>();
    // Unreal vertex (i, j) sits at location + (i, j) * cell; Unreal X is Lila Z and Unreal Y is Lila -X.
    for (auto [i, j] : {std::pair{0, 0}, std::pair{w / 3, rows / 2}, std::pair{w - 1, rows - 1}})
    {
        const double ux = s["location"][0].get<double>() + i * cell, uy = s["location"][1].get<double>() + j * cell;
        const double lilaX = -uy, lilaZ = ux;
        const double decoded = (double(raw[size_t(j) * size_t(w) + size_t(i)]) - 32768.0) * scaleZ / 128.0 + z;
        CHECK(decoded == doctest::Approx(h.heightAt(lilaX, lilaZ)).epsilon(0.002));
    }
    f.close();
    json files = terrain::exportFiles(h, dir, "F");
    CHECK(std::filesystem::exists(files["files"]["heightExr"].get<std::string>()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("terrain: drainage fills pits and every sample reaches the edge downhill")
{
    // A bowl: water collects in the middle until it spills over the rim.
    geo::Heightfield h = geo::makeHeightfield(41, 41, 100.0, {-2000.0, -2000.0});
    for (int z = 0; z < 41; ++z)
        for (int x = 0; x < 41; ++x)
        {
            const double r = glm::length(h.samplePos(x, z));
            h.heights[h.index(x, z)] = float(r < 1200.0 ? -300.0 + r * 0.2 : 500.0 - r * 0.1);
        }
    const terrain::Drainage d = terrain::drain(h);
    REQUIRE(d.order.size() == h.size());
    std::vector<size_t> rank(h.size());
    for (size_t k = 0; k < d.order.size(); ++k)
        rank[d.order[k]] = k;
    for (size_t i = 0; i < h.size(); ++i)
    {
        CHECK(d.filled[i] >= h.heights[i]);
        if (d.receiver[i] != i)
        {
            CHECK(d.filled[d.receiver[i]] < d.filled[i]);
            CHECK(rank[d.receiver[i]] < rank[i]);
        }
    }
    terrain::writeFlowMasks(h, d);
    CHECK(h.at(20, 20) == doctest::Approx(-300.0));
    CHECK((*h.findMask("lakes"))[h.index(20, 20)] > 0.5f);
    CHECK((*h.findMask("lakes"))[h.index(0, 0)] == 0.0f);
}

TEST_CASE("terrain: erosion carves drainage, keeps the edge, writes its masks, and repeats exactly")
{
    terrain::NoiseParams np;
    np.amplitude = 20000.0;
    np.featureSize = 20000.0;
    geo::Heightfield h = geo::makeHeightfield(97, 97, 500.0, {-24000.0, -24000.0});
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            glm::dvec2 p = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(terrain::noiseHeight(np, p.x, p.y));
        }
    terrain::ErosionParams ep;
    ep.iterations = 30;
    geo::Heightfield a = h, b = h;
    terrain::erode(a, ep);
    terrain::erode(b, ep);
    CHECK(a.heights == b.heights);
    CHECK(a.at(0, 40) == h.at(0, 40));
    for (const char* m : {"flow", "wear", "deposit", "lakes"})
        CHECK(a.findMask(m));
    double changed = 0.0;
    for (size_t k = 0; k < h.size(); ++k)
        changed = std::max(changed, double(std::abs(a.heights[k] - h.heights[k])));
    CHECK(changed > 100.0);
    float flowTop = 0.0f;
    for (float v : *a.findMask("flow"))
        flowTop = std::max(flowTop, v);
    CHECK(flowTop == doctest::Approx(1.0f));
}

TEST_CASE("terrain: thermal erosion settles a spike and keeps the material")
{
    geo::Heightfield h = geo::makeHeightfield(21, 21, 100.0, {0.0, 0.0});
    h.heights[h.index(10, 10)] = 5000.0f;
    double before = 0.0;
    for (float v : h.heights)
        before += v;
    terrain::thermal(h, 35.0, 200);
    double after = 0.0;
    for (float v : h.heights)
        after += v;
    CHECK(after == doctest::Approx(before).epsilon(1e-4));
    CHECK(h.at(10, 10) < 1000.0f);
    CHECK(h.at(11, 10) > 0.0f);
}

TEST_CASE("terrain: terrainErode turns on erosion and its masks appear in terrainInfo")
{
    loadTerrain();
    newScene();
    json t = run("terrainCreate", {{"size", {40000.0, 40000.0}}, {"resolution", 129}, {"amplitude", 8000.0}, {"name", "Eroded"}});
    CHECK_FALSE(t["masks"].contains("flow"));
    json e = run("terrainErode", {{"args", {t["mesh"]}}, {"iterations", 20}});
    REQUIRE(e["masks"].contains("flow"));
    CHECK(e["masks"]["flow"]["max"].get<double>() == doctest::Approx(1.0));
    CHECK(e["masks"].contains("wear"));
}

namespace
{
namespace fs = std::filesystem;

void put16(std::vector<uint8_t>& b, uint16_t v) { b.insert(b.end(), {uint8_t(v), uint8_t(v >> 8)}); }
void put32(std::vector<uint8_t>& b, uint32_t v) { b.insert(b.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}); }

/** LZW with literal codes only, cleared often enough that codes stay 9 bits. */
std::vector<uint8_t> lzwLiterals(const std::vector<uint8_t>& in)
{
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    auto code = [&](uint32_t c) {
        acc = (acc << 9) | c;
        bits += 9;
        while (bits >= 8)
        {
            out.push_back(uint8_t(acc >> (bits - 8)));
            bits -= 8;
        }
    };
    code(256);
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (i && i % 200 == 0)
            code(256);
        code(in[i]);
    }
    code(257);
    if (bits)
        out.push_back(uint8_t(acc << (8 - bits)));
    return out;
}

/**
 * A one-strip, one-band little-endian GeoTIFF in degrees: sample (0, 0)'s corner at (lon, lat), step degrees a sample.
 * compression 1, 5 or 8; predictor 3 shuffles float bytes the way libtiff does.
 */
void writeGeoTiff(const fs::path& path, int w, int h, const std::vector<uint8_t>& samples, int bits, int format, int compression,
                  int predictor, double lon, double lat, double step, const char* nodata)
{
    std::vector<uint8_t> data = samples;
    const size_t bps = size_t(bits / 8), rowBytes = size_t(w) * bps;
    if (predictor == 3)
        for (int y = 0; y < h; ++y)
        {
            std::vector<uint8_t> row(rowBytes);
            for (int x = 0; x < w; ++x)
                for (size_t b = 0; b < bps; ++b)
                    row[b * size_t(w) + size_t(x)] = data[y * rowBytes + size_t(x) * bps + (bps - 1 - b)];
            for (size_t k = rowBytes; k-- > 1;)
                row[k] = uint8_t(row[k] - row[k - 1]);
            std::copy(row.begin(), row.end(), data.begin() + ptrdiff_t(y * rowBytes));
        }
    if (compression == 8)
    {
        mz_ulong n = mz_compressBound(mz_ulong(data.size()));
        std::vector<uint8_t> z(n);
        REQUIRE(mz_compress(z.data(), &n, data.data(), mz_ulong(data.size())) == MZ_OK);
        z.resize(n);
        data = z;
    }
    else if (compression == 5)
        data = lzwLiterals(data);

    struct Entry
    {
        uint16_t tag, type;
        uint32_t count;
        std::vector<uint8_t> bytes;
    };
    auto shorts = [](uint16_t tag, std::vector<uint16_t> v) {
        Entry e{tag, 3, uint32_t(v.size()), {}};
        for (uint16_t x : v)
            put16(e.bytes, x);
        return e;
    };
    auto longs = [](uint16_t tag, uint32_t v) {
        Entry e{tag, 4, 1, {}};
        put32(e.bytes, v);
        return e;
    };
    auto doubles = [](uint16_t tag, const std::vector<double>& v) {
        Entry e{tag, 12, uint32_t(v.size()), {}};
        for (double x : v)
        {
            const uint64_t q = std::bit_cast<uint64_t>(x);
            put32(e.bytes, uint32_t(q));
            put32(e.bytes, uint32_t(q >> 32));
        }
        return e;
    };
    const std::string nd = std::string(nodata) + '\0';
    std::vector<Entry> entries = {longs(256, uint32_t(w)), longs(257, uint32_t(h)), shorts(258, {uint16_t(bits)}),
                                  shorts(259, {uint16_t(compression)}), shorts(262, {1}), longs(273, 0), shorts(277, {1}),
                                  longs(278, uint32_t(h)), longs(279, uint32_t(data.size())), shorts(317, {uint16_t(predictor)}),
                                  shorts(339, {uint16_t(format)}), doubles(33550, {step, step, 0.0}), doubles(33922, {0, 0, 0, lon, lat, 0}),
                                  shorts(34735, {1, 1, 0, 2, 1024, 0, 1, 2, 1025, 0, 1, 1}),
                                  Entry{42113, 2, uint32_t(nd.size()), std::vector<uint8_t>(nd.begin(), nd.end())}};
    const size_t extra = 8 + 2 + entries.size() * 12 + 4;
    std::vector<uint8_t> ifd, tail;
    put16(ifd, uint16_t(entries.size()));
    for (Entry& e : entries)
    {
        put16(ifd, e.tag);
        put16(ifd, e.type);
        put32(ifd, e.count);
        if (e.bytes.size() <= 4)
        {
            e.bytes.resize(4, 0);
            ifd.insert(ifd.end(), e.bytes.begin(), e.bytes.end());
        }
        else
        {
            put32(ifd, uint32_t(extra + tail.size()));
            tail.insert(tail.end(), e.bytes.begin(), e.bytes.end());
        }
    }
    put32(ifd, 0);
    // The strip offset is the sixth entry's value.
    const uint32_t dataAt = uint32_t(extra + tail.size());
    std::memcpy(&ifd[2 + 5 * 12 + 8], &dataAt, 4);
    std::vector<uint8_t> b = {'I', 'I'};
    put16(b, 42);
    put32(b, 8);
    b.insert(b.end(), ifd.begin(), ifd.end());
    b.insert(b.end(), tail.begin(), tail.end());
    b.insert(b.end(), data.begin(), data.end());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

template <typename V>
std::vector<uint8_t> bytesOf(const std::vector<V>& v)
{
    std::vector<uint8_t> b(v.size() * sizeof(V));
    std::memcpy(b.data(), v.data(), b.size());
    return b;
}
} // namespace

TEST_CASE("terrain: GeoTIFFs read their samples, voids and placement, uncompressed, deflate and LZW")
{
    const fs::path dir = fs::temp_directory_path() / "lila_terrain_dem_test";
    fs::create_directories(dir);
    const int w = 7, h = 5;
    std::vector<int16_t> ints(size_t(w * h));
    std::vector<float> floats(ints.size());
    for (int i = 0; i < w * h; ++i)
    {
        ints[size_t(i)] = int16_t(100 + i * 3 - (i % 2) * 250);
        floats[size_t(i)] = float(i) * 1.5f - 20.0f;
    }
    ints[9] = -9999;
    floats[3] = -9999.0f;
    struct Case
    {
        const char* name;
        int bits, format, compression, predictor;
    };
    for (const Case& c : {Case{"plain.tif", 16, 2, 1, 1}, Case{"deflate.tif", 16, 2, 8, 1}, Case{"lzw.tif", 16, 2, 5, 1},
                          Case{"float.tif", 32, 3, 8, 3}, Case{"floatlzw.tif", 32, 3, 5, 1}})
    {
        CAPTURE(c.name);
        const fs::path file = dir / c.name;
        writeGeoTiff(file, w, h, c.bits == 16 ? bytesOf(ints) : bytesOf(floats), c.bits, c.format, c.compression, c.predictor, 7.0, 46.0,
                     0.01, "-9999");
        CHECK(terrain::looksLikeElevation(file));
        const terrain::Dem d = terrain::readDem(file);
        REQUIRE(d.width == w);
        REQUIRE(d.height == h);
        CHECK(d.geographic);
        CHECK(d.x0 == doctest::Approx(7.005));
        CHECK(d.y0 == doctest::Approx(45.995));
        CHECK(d.dx == doctest::Approx(0.01));
        for (int i = 0; i < w * h; ++i)
        {
            const bool hole = c.bits == 16 ? i == 9 : i == 3;
            if (hole)
                CHECK(std::isnan(d.metres[size_t(i)]));
            else
                CHECK(d.metres[size_t(i)] == doctest::Approx(c.bits == 16 ? double(ints[size_t(i)]) : double(floats[size_t(i)])));
        }
    }
    fs::remove_all(dir);
}

TEST_CASE("terrain: HGT, ASC, XYZ and Terrarium tiles read, voids fill smoothly and the heightfield has the real size")
{
    const fs::path dir = fs::temp_directory_path() / "lila_terrain_dem_formats";
    fs::create_directories(dir);

    const int n = 11;
    std::vector<uint8_t> hgt;
    for (int i = 0; i < n * n; ++i)
    {
        const int16_t v = i == 60 ? int16_t(-32768) : int16_t(500 + i);
        hgt.push_back(uint8_t(uint16_t(v) >> 8));
        hgt.push_back(uint8_t(v));
    }
    std::ofstream(dir / "S10E020.hgt", std::ios::binary).write(reinterpret_cast<const char*>(hgt.data()), std::streamsize(hgt.size()));
    terrain::Dem d = terrain::readDem(dir / "S10E020.hgt");
    CHECK(d.geographic);
    CHECK(d.x0 == doctest::Approx(20.0));
    CHECK(d.y0 == doctest::Approx(-9.0));
    CHECK(d.dx == doctest::Approx(0.1));
    CHECK(d.metres[1] == doctest::Approx(501.0));
    CHECK(std::isnan(d.metres[60]));
    CHECK(terrain::fillVoids(d) == 1);
    CHECK(d.metres[60] > 540.0f);
    CHECK(d.metres[60] < 580.0f);
    terrain::DemOptions o;
    o.center = {1000.0, 2000.0};
    const geo::Heightfield hf = terrain::demToHeightfield(d, o);
    // A degree of latitude is about 110.6 km at 9.5 degrees south, and the tile spans one.
    CHECK(hf.extent().y == doctest::Approx(110600.0 * 100.0).epsilon(0.01));
    CHECK(hf.origin.x + hf.extent().x * 0.5 == doctest::Approx(1000.0));
    CHECK(hf.heights[hf.index(0, 0)] == doctest::Approx(50000.0));
    REQUIRE(hf.findMask("voids"));
    CHECK(hf.maskAt("voids", hf.samplePos(5, 5).x, hf.samplePos(5, 5).y) > 0.5f);

    {
        std::ofstream f(dir / "grid.asc");
        f << "ncols 4\nnrows 3\nxllcorner 1000\nyllcorner 5000\ncellsize 10\nNODATA_value -1\n";
        f << "1 2 3 4\n5 -1 7 8\n9 10 11 12\n";
    }
    d = terrain::readDem(dir / "grid.asc");
    CHECK_FALSE(d.geographic);
    CHECK(d.x0 == doctest::Approx(1005.0));
    CHECK(d.y0 == doctest::Approx(5025.0));
    CHECK(std::isnan(d.metres[5]));
    terrain::fillVoids(d);
    CHECK(d.metres[5] == doctest::Approx(6.5).epsilon(0.3));
    const geo::Heightfield asc = terrain::demToHeightfield(d, {});
    CHECK(asc.nx == 4);
    CHECK(asc.cell == doctest::Approx(1000.0));
    CHECK(asc.heights[asc.index(3, 2)] == doctest::Approx(1200.0));

    {
        std::ofstream f(dir / "points.xyz");
        f << "x,y,z\n";
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                f << 500000 + x * 2 << "," << 4000000 + y * 2 << "," << x + y * 10 << "\n";
    }
    d = terrain::readDem(dir / "points.xyz");
    CHECK(d.width == 3);
    CHECK(d.dx == doctest::Approx(2.0));
    CHECK_FALSE(d.geographic);
    // North row first: y = 4000004 holds 20, 21, 22.
    CHECK(d.metres[0] == doctest::Approx(20.0));
    CHECK(d.metres[8] == doctest::Approx(2.0));

    std::vector<float> rgba(4 * 4, 1.0f);
    const uint8_t px[4][3] = {{128, 0, 0}, {128, 100, 128}, {127, 0, 0}, {129, 255, 0}};
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 3; ++c)
            rgba[size_t(i * 4 + c)] = px[i][c] / 255.0f;
    image::writePng(dir / "tile.png", 2, 2, rgba, 8);
    d = terrain::readDem(dir / "tile.png");
    CHECK(d.metres[0] == doctest::Approx(0.0));
    CHECK(d.metres[1] == doctest::Approx(100.5));
    CHECK(d.metres[2] == doctest::Approx(-256.0));
    CHECK(d.metres[3] == doctest::Approx(511.0));
    CHECK_FALSE(terrain::looksLikeElevation(dir / "tile.png"));
    fs::remove_all(dir);
}

TEST_CASE("terrain: painted images read as height, guide, uplift or mask")
{
    auto cone = [] {
        geo::Heightfield h = geo::makeHeightfield(65, 65, 500.0, {0.0, 0.0});
        for (int z = 0; z < h.nz; ++z)
            for (int x = 0; x < h.nx; ++x)
            {
                const double dx = x - 32.0, dz = z - 32.0;
                h.heights[h.index(x, z)] = float(std::max(0.0, 1.0 - std::sqrt(dx * dx + dz * dz) / 30.0));
            }
        return h;
    };
    terrain::ImageOptions o;
    o.low = 100.0;
    o.high = 10100.0;
    o.smooth = 0.0;

    geo::Heightfield h = cone();
    o.mode = terrain::ImageMode::Height;
    terrain::interpretImage(h, o);
    CHECK(h.heights[h.index(32, 32)] == doctest::Approx(10100.0));
    CHECK(h.heights[h.index(0, 0)] == doctest::Approx(100.0));
    CHECK(h.masks.empty());

    h = cone();
    o.mode = terrain::ImageMode::Guide;
    o.erosion = 0.3;
    terrain::interpretImage(h, o);
    CHECK(h.findMask("flow"));
    CHECK(h.heights[h.index(32, 32)] > 8000.0f);
    CHECK(h.heights[h.index(32, 32)] < 10500.0f);

    h = cone();
    o.mode = terrain::ImageMode::Uplift;
    terrain::interpretImage(h, o);
    CHECK(h.range().x == doctest::Approx(100.0));
    CHECK(h.range().y == doctest::Approx(10100.0));
    CHECK(h.heights[h.index(32, 32)] > h.heights[h.index(2, 2)] + 3000.0f);

    h = cone();
    o.mode = terrain::ImageMode::Mask;
    o.mask = "snow";
    terrain::interpretImage(h, o);
    CHECK(h.range().y == doctest::Approx(100.0));
    REQUIRE(h.findMask("snow"));
    CHECK(h.maskAt("snow", h.samplePos(32, 32).x, h.samplePos(32, 32).y) == doctest::Approx(1.0));
}

TEST_CASE("terrain: terrainImport makes terrains from elevation data and painted images")
{
    loadTerrain();
    newScene();
    const fs::path dir = fs::temp_directory_path() / "lila_terrain_import_test";
    fs::create_directories(dir);
    const int w = 9, h = 9;
    std::vector<float> floats(size_t(w * h));
    for (int i = 0; i < w * h; ++i)
        floats[size_t(i)] = 200.0f + float(i);
    writeGeoTiff(dir / "dem.tif", w, h, bytesOf(floats), 32, 3, 8, 3, 7.0, 46.0, 0.001, "-9999");
    json t = run("terrainImport", {{"args", {(dir / "dem.tif").string()}}, {"cellSize", 5000.0}});
    CHECK(t["mode"] == "elevation");
    CHECK(t["heightRange"][0].get<double>() == doctest::Approx(20000.0).epsilon(0.01));
    CHECK(t["heightRange"][1].get<double>() == doctest::Approx(28000.0).epsilon(0.01));
    CHECK(t["size"][1].get<double>() == doctest::Approx(0.008 * 111150.0 * 100.0).epsilon(0.02));

    std::vector<float> rgba(size_t(33 * 33 * 4), 1.0f);
    for (int z = 0; z < 33; ++z)
        for (int x = 0; x < 33; ++x)
            for (int c = 0; c < 3; ++c)
                rgba[size_t((z * 33 + x) * 4 + c)] = float(x) / 32.0f;
    image::writePng(dir / "paint.png", 33, 33, rgba, 16);
    json p = run("terrainImport", {{"args", {(dir / "paint.png").string()}}, {"mode", "height"}, {"size", 3200.0}, {"high", 1000.0}});
    CHECK(p["mode"] == "height");
    CHECK(p["size"][0].get<double>() == doctest::Approx(3200.0));
    CHECK(p["heightRange"][1].get<double>() == doctest::Approx(1000.0).epsilon(0.02));
    run("terrainErode", {{"args", {p["mesh"]}}, {"iterations", 10}});
    CHECK(run("terrainInfo", {{"args", {p["mesh"]}}})["masks"].contains("flow"));
    fs::remove_all(dir);
}
