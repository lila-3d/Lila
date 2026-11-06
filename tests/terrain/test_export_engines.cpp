#include "TestUtil.h"
#include "Export.h"
#include "ExportEngines.h"

#include "core/ImageIO.h"
#include "fbx/Fbx.h"
#include "geo/Heightfield.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "gltf/Gltf.h"
#include "ops/Undo.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

using namespace lila;
using namespace terraintest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
geo::Heightfield hills(int nx, int nz, double cell, glm::dvec2 origin)
{
    geo::Heightfield h = geo::makeHeightfield(nx, nz, cell, origin);
    std::vector<float>& rock = h.mask("rock");
    std::vector<float>& grass = h.mask("grass");
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            const size_t k = h.index(x, z);
            h.heights[k] = float(400.0 * std::sin(p.x / 900.0) * std::cos(p.y / 700.0) + 0.04 * p.x + 0.02 * p.y);
            rock[k] = float(0.5 + 0.5 * std::sin(p.x / 1300.0));
            grass[k] = float(0.6 * (0.5 + 0.5 * std::cos(p.y / 1100.0)));
        }
    return h;
}

fs::path scratch(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::vector<uint16_t> read16(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    REQUIRE(f);
    std::vector<uint16_t> v(size_t(f.tellg()) / 2);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), std::streamsize(v.size() * 2));
    return v;
}

std::vector<uint8_t> read8(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    REQUIRE(f);
    std::vector<uint8_t> v(size_t(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), std::streamsize(v.size()));
    return v;
}

std::string readText(const fs::path& p)
{
    std::vector<uint8_t> b = read8(p);
    return std::string(b.begin(), b.end());
}

/** A 16-bit greyscale PNG with unfiltered rows, as writeGray16Png writes it. */
std::vector<uint16_t> readGray16Png(const fs::path& p, int& w, int& h)
{
    const std::vector<uint8_t> b = read8(p);
    REQUIRE(b.size() > 33);
    REQUIRE(std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
    auto be32 = [&](size_t at) { return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | uint32_t(b[at + 3]); };
    std::vector<uint8_t> idat;
    for (size_t at = 8; at + 8 <= b.size();)
    {
        const uint32_t n = be32(at);
        const std::string type(reinterpret_cast<const char*>(&b[at + 4]), 4);
        if (type == "IHDR")
        {
            w = int(be32(at + 8));
            h = int(be32(at + 12));
            CHECK(b[at + 16] == 16);
            CHECK(b[at + 17] == 0);
        }
        else if (type == "IDAT")
            idat.insert(idat.end(), b.begin() + long(at + 8), b.begin() + long(at + 8 + n));
        const uint32_t crc = mz_crc32(MZ_CRC32_INIT, &b[at + 4], n + 4);
        CHECK(crc == be32(at + 8 + n));
        at += 12 + n;
    }
    std::vector<uint8_t> rows(size_t(w * 2 + 1) * size_t(h));
    mz_ulong size = mz_ulong(rows.size());
    REQUIRE(mz_uncompress(rows.data(), &size, idat.data(), mz_ulong(idat.size())) == MZ_OK);
    std::vector<uint16_t> out(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* row = &rows[size_t(y) * size_t(w * 2 + 1)];
        REQUIRE(row[0] == 0);
        for (int x = 0; x < w; ++x)
            out[size_t(y) * size_t(w) + size_t(x)] = uint16_t(row[1 + 2 * x] << 8 | row[2 + 2 * x]);
    }
    return out;
}
} // namespace

TEST_CASE("terrain export: tiles abut or share edges, and tiled files rebuild the whole grid exactly")
{
    for (bool shared : {false, true})
    {
        const auto tiles = terrain::tileGrid(70, 50, 32, shared);
        std::vector<int> cover(70 * 50, 0);
        for (const auto& t : tiles)
            for (int z = 0; z < t.nz; ++z)
                for (int x = 0; x < t.nx; ++x)
                    ++cover[size_t(t.z0 + z) * 70 + size_t(t.x0 + x)];
        CHECK(*std::min_element(cover.begin(), cover.end()) == 1);
        CHECK(*std::max_element(cover.begin(), cover.end()) == (shared ? 4 : 1));
        CHECK(tiles.size() == (shared ? 3u * 2u : 3u * 2u));
    }
    CHECK(terrain::tileSuffix(3, 12) == "_x3_y12");

    const geo::Heightfield h = hills(70, 50, 100.0, {-3500.0, -2500.0});
    const fs::path dir = scratch("lila_terrain_tiled_files");
    json whole = terrain::exportFiles(h, dir, "W");
    const std::vector<uint16_t> ref = read16(whole["files"]["heightR16"].get<std::string>());
    for (bool shared : {false, true})
    {
        json s = terrain::exportFilesTiled(h, dir, shared ? "S" : "A", 32, shared);
        std::vector<int> seen(ref.size(), 0);
        for (const json& t : s["tiles"])
        {
            const int x0 = t["samples"][0], z0 = t["samples"][1], nx = t["samples"][2], nz = t["samples"][3];
            CHECK(fs::exists(dir / t["exr"].get<std::string>()));
            CHECK(fs::exists(dir / t["json"].get<std::string>()));
            CHECK(t["masks"].contains("rock"));
            const std::vector<uint16_t> r = read16(dir / t["r16"].get<std::string>());
            REQUIRE(r.size() == size_t(nx) * size_t(nz));
            for (int z = 0; z < nz; ++z)
                for (int x = 0; x < nx; ++x)
                {
                    // Every tile encodes over the whole terrain's range, so each sample decodes to the untiled value.
                    CHECK_MESSAGE(r[size_t(z) * nx + x] == ref[size_t(z0 + z) * 70 + size_t(x0 + x)], "tile ", t["tile"].dump());
                    ++seen[size_t(z0 + z) * 70 + size_t(x0 + x)];
                }
        }
        CHECK(*std::min_element(seen.begin(), seen.end()) == 1);
    }
    json splats = terrain::writeSplatPngs(h, dir, "W", {});
    REQUIRE(splats.size() == 1);
    CHECK(splats[0]["channels"].size() == 2);
    const image::Image sp = image::read(splats[0]["file"].get<std::string>());
    CHECK(sp.width == 70);
    // Masks go in name order: grass in red, rock in green.
    CHECK(sp.rgba[(size_t(10) * 70 + 20) * 4 + 1] == doctest::Approx(h.masks.at("rock")[h.index(20, 10)]).epsilon(0.01));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("terrain export: Unity tiles are 2^n+1, share their edges byte for byte, decode to the heights and carry splatmaps")
{
    for (int r : {33, 65, 129, 257, 513, 1025, 2049, 4097})
        CHECK(terrain::validUnityResolution(r));
    for (int r : {0, 17, 32, 64, 100, 4096, 8193})
        CHECK_FALSE(terrain::validUnityResolution(r));

    const fs::path dir = scratch("lila_terrain_unity");
    // 65 x 65 samples fit one 65 tile with the same cells: an exact copy.
    {
        const geo::Heightfield h = hills(65, 65, 100.0, {-3200.0, -3200.0});
        terrain::UnityOptions o;
        o.script = LILA_TEST_TERRAIN_SCRIPTS "/LilaTerrainImporter.cs";
        json s = terrain::exportUnity(h, glm::dmat4(1.0), dir, "U", o);
        CHECK(s["heightmapResolution"] == 65);
        CHECK(s["tileCount"] == json::array({1, 1}));
        CHECK(s["resampled"] == false);
        CHECK(s["alphamapResolution"] == 64);
        CHECK(s["layers"].size() == 3);
        CHECK(s["layers"][0]["name"] == "Base");
        const std::vector<uint16_t> raw = read16(dir / s["tiles"][0]["heightmap"].get<std::string>());
        REQUIRE(raw.size() == 65u * 65u);
        const double low = s["heightRange"][0], span = s["tileSize"][1].get<double>() * 100.0;
        double worst = 0.0;
        for (int r = 0; r < 65; ++r)
            for (int c = 0; c < 65; ++c)
            {
                // Unity X is Lila -X, so column 0 is the terrain's +X edge; rows run along +Z.
                const double decoded = low + raw[size_t(r) * 65 + size_t(c)] / 65535.0 * span;
                worst = std::max(worst, std::abs(decoded - h.at(64 - c, r)));
            }
        CHECK(worst <= span / 65535.0);
        CHECK(s["tiles"][0]["position"][0].get<double>() == doctest::Approx(-32.0));
        CHECK(s["tiles"][0]["position"][2].get<double>() == doctest::Approx(-32.0));
        const std::vector<uint8_t> splat = read8(dir / s["tiles"][0]["splatmap"].get<std::string>());
        REQUIRE(splat.size() == 64u * 64u * 3u);
        for (size_t t = 0; t < 64u * 64u; ++t)
        {
            const int sum = splat[t * 3] + splat[t * 3 + 1] + splat[t * 3 + 2];
            CHECK(std::abs(sum - 255) <= 2);
        }

        // The importer reads what the export writes.
        const std::string cs = readText(dir / "LilaTerrainImporter.cs");
        for (const char* needle : {"SetHeights", "SetAlphamaps", "SetNeighbors", "heightmapResolution", "alphamapResolution", "tileCount", "tileSize",
                                   "splatmap", "heightmap", "position", "terrainLayers", "(bytes[i] | (bytes[i + 1] << 8)) / 65535f"})
            CHECK_MESSAGE(cs.find(needle) != std::string::npos, needle);
        for (const char* key : {"target", "name", "heightmapResolution", "alphamapResolution", "tileCount", "tileSize", "layers", "tiles"})
            CHECK_MESSAGE(json::parse(readText(dir / "U.json")).contains(key), key);
    }
    // A bigger terrain capped at 65 per tile: 4 x 3 tiles whose shared edges are identical.
    {
        const geo::Heightfield h = hills(200, 130, 100.0, {0.0, 0.0});
        terrain::UnityOptions o;
        o.maxResolution = 65;
        json s = terrain::exportUnity(h, glm::dmat4(1.0), dir, "B", o);
        CHECK(s["heightmapResolution"] == 65);
        CHECK(s["tileCount"] == json::array({4, 3}));
        std::map<std::pair<int, int>, std::vector<uint16_t>> tiles;
        for (const json& t : s["tiles"])
            tiles[{t["x"].get<int>(), t["y"].get<int>()}] = read16(dir / t["heightmap"].get<std::string>());
        int compared = 0;
        for (const auto& [at, raw] : tiles)
        {
            if (auto right = tiles.find({at.first + 1, at.second}); right != tiles.end())
                for (int r = 0; r < 65; ++r, ++compared)
                    CHECK(raw[size_t(r) * 65 + 64] == right->second[size_t(r) * 65]);
            if (auto up = tiles.find({at.first, at.second + 1}); up != tiles.end())
                for (int c = 0; c < 65; ++c, ++compared)
                    CHECK(raw[size_t(64) * 65 + size_t(c)] == up->second[size_t(c)]);
        }
        CHECK(compared == (3 * 3 + 4 * 2) * 65);
        // Tile positions step by the tile size.
        CHECK(s["tiles"][1]["position"][0].get<double>() - s["tiles"][0]["position"][0].get<double>() ==
              doctest::Approx(s["tileSize"][0].get<double>()));
    }
    CHECK_THROWS(terrain::exportUnity(hills(33, 33, 100.0, {}), glm::dmat4(1.0), dir, "X", {.resolution = 100}));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("terrain export: Terrain3D regions sit on its grid with heights in metres and an importer that names the export")
{
    const fs::path dir = scratch("lila_terrain_godot");
    const geo::Heightfield h = hills(100, 80, 100.0, {-5000.0, -4000.0});
    terrain::GodotOptions o;
    o.script = LILA_TEST_TERRAIN_SCRIPTS "/lila_terrain3d_import.gd";
    json s = terrain::exportGodot(h, glm::dmat4(1.0), dir, "G", o);
    CHECK(s["regionSize"] == 256);
    CHECK(s["vertexSpacing"].get<double>() == doctest::Approx(1.0));
    CHECK(s["regions"].size() == 4);
    CHECK(s["resampled"] == false);
    for (const json& r : s["regions"])
    {
        const image::Image img = image::read(dir / r["heightmap"].get<std::string>());
        REQUIRE(img.width == 256);
        REQUIRE(img.height == 256);
        const int rx = r["x"], rz = r["y"];
        CHECK(r["position"][0].get<double>() == doctest::Approx(rx * 256.0));
        // Pixel (px, pz) is the vertex at ((rx * 256 + px) m, (rz * 256 + pz) m), Lila's axes in metres.
        for (auto [px, pz] : {std::pair{0, 0}, std::pair{10, 20}, std::pair{255, 255}, std::pair{40, 30}})
        {
            const double wx = (rx * 256 + px) * 100.0, wz = (rz * 256 + pz) * 100.0;
            if (!h.contains(wx, wz))
                continue;
            CHECK(img.rgba[(size_t(pz) * 256 + size_t(px)) * 4] == doctest::Approx(h.heightAt(wx, wz) / 100.0).epsilon(1e-5));
        }
        CHECK(r["splats"].size() == 1);
    }
    const std::string gd = readText(dir / "G_terrain3d.gd");
    CHECK(gd.find("const TERRAIN_JSON := \"G.json\"") != std::string::npos);
    for (const char* needle : {"import_images", "Terrain3DUtil.enc_base", "Terrain3DUtil.as_float", "vertex_spacing", "change_region_size", "regionSize",
                               "vertexSpacing", "footprint", "splats"})
        CHECK_MESSAGE(gd.find(needle) != std::string::npos, needle);
    // Too far from the origin for the region map at the largest size.
    CHECK_THROWS(terrain::exportGodot(hills(10, 10, 100.0, {1e8, 0.0}), glm::dmat4(1.0), dir, "F", {}));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("terrain export: mesh tiles round trip, keep full-resolution borders at every LOD and hang skirts")
{
    const geo::Heightfield h = hills(70, 50, 100.0, {-3500.0, -2500.0});
    terrain::MeshTileOptions o;
    o.tileSize = 33;
    o.lods = 3;
    o.lodRatio = 0.5;
    const std::vector<terrain::MeshTile> tiles = terrain::buildMeshTiles(h, o);
    REQUIRE(tiles.size() == 3u * 2u);

    // LOD0 back to a heightfield on the same grid gives the same heights.
    std::vector<const geo::Mesh*> lod0;
    for (const auto& t : tiles)
        lod0.push_back(&t.lods[0]);
    const geo::Mesh all = geo::combine(lod0);
    const geo::Heightfield back = geo::meshToHeightfield(all, h.nx, h.nz, h.cell, h.origin, -1e9f);
    double worst = 0.0;
    for (size_t k = 0; k < h.size(); ++k)
        worst = std::max(worst, double(std::abs(back.heights[k] - h.heights[k])));
    CHECK(worst < 1e-3);

    for (const auto& t : tiles)
    {
        REQUIRE(t.lods.size() == 3);
        CHECK(t.lods[1].numFaces() < t.lods[0].numFaces() * 2);
        CHECK(t.lods[2].numFaces() < t.lods[1].numFaces());
        CHECK(t.errors[0] == 0.0);
        CHECK(t.errors[1] <= t.errors[2]);
        // The hills span about 1100 cm; LOD2 keeps a quarter of the triangles, its border all of its samples.
        CHECK(t.errors[2] < 60.0);
        CHECK_FALSE(t.collision.empty());
        // Every level keeps each border sample exactly, so any two levels of neighbours meet without a crack.
        const double x0 = h.origin.x + t.rect.x0 * h.cell, x1 = x0 + (t.rect.nx - 1) * h.cell;
        const double z0 = h.origin.y + t.rect.z0 * h.cell, z1 = z0 + (t.rect.nz - 1) * h.cell;
        for (const geo::Mesh& m : t.lods)
        {
            std::set<std::pair<long, long>> border;
            for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            {
                if (!m.pointValid(p))
                    continue;
                const glm::dvec3 q = m.position(p);
                if (std::abs(q.x - x0) < 1e-6 || std::abs(q.x - x1) < 1e-6 || std::abs(q.z - z0) < 1e-6 || std::abs(q.z - z1) < 1e-6)
                {
                    const long i = std::lround((q.x - h.origin.x) / h.cell), j = std::lround((q.z - h.origin.y) / h.cell);
                    CHECK(q.y == doctest::Approx(h.at(int(i), int(j))));
                    border.insert({i, j});
                }
            }
            CHECK(border.size() == size_t(2 * (t.rect.nx + t.rect.nz) - 4));
        }
    }

    o.skirt = 500.0;
    o.lods = 1;
    o.collision = false;
    const std::vector<terrain::MeshTile> skirted = terrain::buildMeshTiles(h, o);
    const geo::Mesh& m = skirted[0].lods[0];
    const int quads = (skirted[0].rect.nx - 1) * (skirted[0].rect.nz - 1);
    CHECK(int(m.numFaces()) == quads + 2 * (skirted[0].rect.nx - 1 + skirted[0].rect.nz - 1));
    double lowest = 1e30, lowestBorder = 1e30;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        lowest = std::min(lowest, m.position(p).y);
    for (int x = 0; x < skirted[0].rect.nx; ++x)
        lowestBorder = std::min({lowestBorder, double(h.at(x, 0)), double(h.at(x, skirted[0].rect.nz - 1))});
    for (int z = 0; z < skirted[0].rect.nz; ++z)
        lowestBorder = std::min({lowestBorder, double(h.at(0, z)), double(h.at(skirted[0].rect.nx - 1, z))});
    CHECK(lowest == doctest::Approx(lowestBorder - 500.0));
    CHECK(m.validate().empty());
}

TEST_CASE("terrain export: the Mesh Terrain heightmap puts every FHeightmapImporter vertex on its terrain sample")
{
    const geo::Heightfield h = hills(70, 50, 100.0, {-3500.0, -2500.0});
    const fs::path dir = scratch("lila_terrain_meshterrain");
    json s = terrain::exportUnrealMeshTerrain(h, glm::dmat4(1.0), dir, "M", {.sectionQuads = 16});
    const int mx = s["meshResolution"][0], my = s["meshResolution"][1];
    CHECK(mx == 49);
    CHECK(my == 69);
    CHECK(s["sections"] == json::array({4, 5}));
    int w = 0, hgt = 0;
    const std::vector<uint16_t> px = readGray16Png(s["files"]["heightmap"].get<std::string>(), w, hgt);
    CHECK(w == 2 * mx);
    CHECK(hgt == 2 * my);
    const double low = s["heightRange"][0], span = s["meshSize"][2];

    // The importer's GetHeight (MeshPartitionHeightmapImporter.cpp): pixels as cells, float UVs, truncating casts.
    auto value = [&](int x, int y) { return double(px[size_t(y) * size_t(w) + size_t(x)]) / 65535.0; };
    auto importer = [&](float u, float v) {
        const double X = double(u) * w, Y = double(v) * hgt;
        const int x0 = std::clamp(int(X - 0.5), 0, w - 1), y0 = std::clamp(int(Y - 0.5), 0, hgt - 1);
        const int x1 = std::clamp(x0 + 1, 0, w - 1), y1 = std::clamp(y0 + 1, 0, hgt - 1);
        const double dx = X - (x0 + 0.5), dy = Y - (y0 + 0.5);
        const double a = value(x0, y0) * (1 - dx) + value(x1, y0) * dx, b = value(x0, y1) * (1 - dx) + value(x1, y1) * dx;
        return a * (1 - dy) + b * dy;
    };
    double worst = 0.0;
    for (int l = 0; l <= my; ++l)
        for (int k = 0; k <= mx; ++k)
        {
            const double z = low + importer(float(k) / float(mx), float(l) / float(my)) * span;
            // Unreal X (k) is Lila +Z, Unreal Y (l) is Lila -X.
            worst = std::max(worst, std::abs(z - h.at(h.nx - 1 - l, k)));
        }
    CHECK(worst <= span / 65535.0 * 1.01);
    CHECK(s["meshSize"][0].get<double>() == doctest::Approx(4900.0));
    CHECK(s["meshSize"][1].get<double>() == doctest::Approx(6900.0));
    // The centre of the footprint at the lowest height, in Unreal axes (X = Lila Z, Y = -Lila X).
    CHECK(s["location"][0].get<double>() == doctest::Approx(-2500.0 + 2450.0));
    CHECK(s["location"][1].get<double>() == doctest::Approx(-(-3500.0 + 3450.0)));
    CHECK(s["location"][2].get<double>() == doctest::Approx(low));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("terrain export: Unreal regions cover the landscape and reassemble its heights and weights exactly")
{
    const geo::Heightfield h = hills(127, 253, 100.0, {-6300.0, -12600.0});
    const fs::path dir = scratch("lila_terrain_regions");
    json settings = terrain::exportUnreal(h, glm::dmat4(1.0), dir, "R", {});
    const int vx = settings["resolution"][0], vy = settings["resolution"][1];
    const int quads = settings["quadsPerSection"].get<int>() * settings["sectionsPerComponent"].get<int>();
    json split = terrain::splitUnrealRegions(settings, quads);
    const std::vector<uint16_t> full = read16(settings["files"]["height"].get<std::string>());
    const std::vector<uint8_t> rock = read8(settings["layers"][1]["file"].get<std::string>());
    std::vector<int> seen(full.size(), 0);
    for (const json& r : split["regions"])
    {
        const int x0 = r["rect"][0], y0 = r["rect"][1], x1 = r["rect"][2], y1 = r["rect"][3];
        const std::vector<uint16_t> t = read16(r["heightmap"].get<std::string>());
        REQUIRE(t.size() == size_t(x1 - x0 + 1) * size_t(y1 - y0 + 1));
        REQUIRE(r["layers"].size() == 2);
        CHECK(r["layers"][1]["name"] == "rock");
        const std::vector<uint8_t> tr = read8(r["layers"][1]["weightmap"].get<std::string>());
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const size_t local = size_t(y - y0) * size_t(x1 - x0 + 1) + size_t(x - x0), global = size_t(y) * size_t(vx) + size_t(x);
                CHECK(t[local] == full[global]);
                CHECK(tr[local] == rock[global]);
                ++seen[global];
            }
    }
    CHECK(*std::min_element(seen.begin(), seen.end()) >= 1);
    CHECK(split["regionCount"][0].get<int>() * quads >= vx - 1);
    CHECK(split["regionCount"][1].get<int>() * quads >= vy - 1);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("terrain export commands: unity, godot, mesh and tiled files from a terrain, the scene left as it was")
{
    loadTerrain();
    if (!ops::find("exportFbx"))
        fbx::registerCommands();
    if (!ops::find("exportGltf"))
        gltf::registerCommands();
    run("newScene");
    json t = run("terrainCreate", {{"size", {12800.0, 6400.0}}, {"resolution", 129}, {"amplitude", 2000.0}, {"seed", 5}});
    const std::string mesh = t["mesh"];
    const fs::path dir = scratch("lila_terrain_export_cmds");
    scene::Graph& g = scene::Graph::current();
    const size_t nodesBefore = g.nodes().size();
    const std::string undoBefore = UndoStack::get().undoLabel();

    json u = run("terrainExport", {{"args", {mesh}}, {"target", "unity"}, {"folder", (dir / "unity").generic_string()}});
    CHECK(u["heightmapResolution"] == 129);
    CHECK(fs::exists(dir / "unity" / "LilaTerrainImporter.cs"));

    json gd = run("terrainExport", {{"args", {mesh}}, {"target", "godot"}, {"folder", (dir / "godot").generic_string()}});
    CHECK(gd["regions"].size() >= 1);
    CHECK(fs::exists(gd["files"]["importer"].get<std::string>()));

    json m = run("terrainExport", {{"args", {mesh}}, {"target", "mesh"}, {"folder", (dir / "mesh").generic_string()}, {"tileSize", 65}, {"format", "fbx,glb"}});
    CHECK(m["tiles"] == 2 * 1);
    CHECK(m["files"].size() == 4);
    for (const json& f : m["files"])
        CHECK(fs::file_size(f.get<std::string>()) > 1000);
    CHECK(m["trianglesPerLod"][0] == 128 * 64 * 2);
    CHECK(m["trianglesPerLod"][2].get<int64_t>() < m["trianglesPerLod"][1].get<int64_t>());

    json gm = run("terrainExport", {{"args", {mesh}}, {"target", "godot"}, {"mode", "mesh"}, {"folder", (dir / "godotmesh").generic_string()}, {"tileSize", 129}});
    CHECK(gm["formats"] == json::array({"glb"}));
    CHECK(gm["collisionSuffix"] == "_colonly");

    json f = run("terrainExport", {{"args", {mesh}}, {"target", "files"}, {"folder", (dir / "files").generic_string()}, {"tileSize", 64}, {"splats", true}});
    CHECK(f["tiled"] == true);
    CHECK(f["tileCount"] == json::array({3, 2}));

    // Exports build their meshes in the scene only for as long as the exporters need them.
    CHECK(g.nodes().size() == nodesBefore);
    CHECK(UndoStack::get().undoLabel() == undoBefore);

    json tiles = run("terrainTiles", {{"args", {mesh}}, {"tileSize", 65}, {"skirt", 100.0}});
    CHECK(tiles["tiles"] == 2);
    CHECK(g.find(tiles["group"].get<std::string>()));
    CHECK(tiles["tileList"][0]["levels"].size() == 3);
    run("undo");
    CHECK(g.nodes().size() == nodesBefore);

    CHECK_THROWS(run("terrainExport", {{"args", {mesh}}, {"target", "unity"}, {"folder", dir.generic_string()}, {"layers", {"nope"}}}));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
