#include "TestUtil.h"
#include "Volume.h"

#include "geo/Heightfield.h"
#include "geo/SurfaceNets.h"

#include <doctest/doctest.h>
#include <tbb/global_control.h>

#include <cmath>
#include <filesystem>
#include <set>

using namespace lila;
using namespace terraintest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
double smoothstepT(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** Flat ground at 0 for x < -2000, rising to a plateau 3000 cm high past x = 2000; 200 cm cells, 128 m a side. */
geo::Heightfield hillside()
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {12800.0, 12800.0}, 65);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
            h.heights[h.index(x, z)] = float(3000.0 * smoothstepT(-2000.0, 2000.0, h.samplePos(x, z).x));
    return h;
}

terrain::VolumeOp cave(std::string id, std::vector<glm::dvec3> path, double radius)
{
    terrain::VolumeOp op;
    op.id = std::move(id);
    op.kind = "cave";
    op.path = std::move(path);
    op.radius = radius;
    op.noise = 0.2;
    op.noiseSize = 300.0;
    op.blend = 0.3 * radius;
    return op;
}

std::vector<std::shared_ptr<const geo::Mesh>> lod0(const terrain::VolumeResult& r)
{
    std::vector<std::shared_ptr<const geo::Mesh>> out;
    for (const terrain::VolumePiece& p : r.pieces)
        out.push_back(p.lods[0]);
    return out;
}

/** The split's own figures and the independent seam and footprint measures all say no cracks and exact holes. */
void checkSplit(const geo::Heightfield& h, const terrain::VolumeResult& r)
{
    CHECK(r.unmeshedColumns == 0);
    CHECK(r.seamUncovered == doctest::Approx(0.0));
    const auto meshes = lod0(r);
    const terrain::SeamReport seams = terrain::measureSeams(h, meshes);
    if (r.holeCells > 0)
    {
        CHECK(seams.length > 0.0);
        CHECK(seams.uncovered == 0.0);
        INFO("worst " << seams.worst.x << " " << seams.worst.y << " " << seams.worst.z << " voxel " << r.voxel);
        CHECK(seams.maxGap < 0.01);
    }
    const terrain::FootprintReport fp = terrain::measureFootprint(h, meshes, 2.0 * r.voxel);
    CHECK(fp.checked > 0);
    CHECK(fp.uncoveredInHole == 0);
    CHECK(fp.meshOutsideHole == 0);
    for (const auto& [id, o] : r.ops)
        CHECK_MESSAGE(o.applied, id);
}
} // namespace

TEST_CASE("surface nets: one grid meshed whole or in blocks gives the same quads and points")
{
    const glm::ivec3 n(24, 20, 22);
    std::vector<float> v(size_t(n.x) * n.y * n.z);
    for (int k = 0; k < n.z; ++k)
        for (int j = 0; j < n.y; ++j)
            for (int i = 0; i < n.x; ++i)
                v[size_t(i) + n.x * (size_t(j) + n.y * size_t(k))] = float(glm::length(glm::dvec3(i, j, k) - glm::dvec3(11.3, 9.7, 10.4)) - 7.2);
    const geo::SurfaceNets whole = geo::surfaceNets({n, v});
    CHECK(whole.numQuads() > 100);
    const geo::Mesh m = geo::surfaceNetsMesh({n, v}, glm::dvec3(0.0), 1.0);
    CHECK(m.validate().empty());
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
            CHECK(m.edgeFaceCount(e) == 2);
    size_t quads = 0;
    std::set<std::tuple<int, int, int>> cells;
    for (int bz = 0; bz < n.z; bz += 8)
        for (int bx = 0; bx < n.x; bx += 8)
        {
            geo::SurfaceNetsOptions o;
            o.faceMin = {bx, 0, bz};
            o.faceMax = {bx + 8, n.y, bz + 8};
            const geo::SurfaceNets part = geo::surfaceNets({n, v}, o);
            quads += part.numQuads();
            for (size_t p = 0; p < part.cells.size(); ++p)
            {
                cells.insert({part.cells[p].x, part.cells[p].y, part.cells[p].z});
                // A cell's point is the same bits whichever block made it.
                for (size_t w = 0; w < whole.cells.size(); ++w)
                    if (whole.cells[w] == part.cells[p])
                        CHECK(whole.offsets[w] == part.offsets[p]);
            }
        }
    CHECK(quads == whole.numQuads());
    CHECK(cells.size() == whole.cells.size());
}

TEST_CASE("volume: a cave opening from a hillside is a mesh over an exact hole, its buried end under intact ground")
{
    geo::Heightfield h = hillside();
    const geo::Heightfield before = h;
    // One cave enters the hill at ground level and runs under the plateau; another lies wholly under it.
    const std::vector<terrain::VolumeOp> ops{cave("mouth", {{-3000.0, 150.0, 0.0}, {0.0, 800.0, 300.0}, {4000.0, 1500.0, 0.0}}, 300.0),
                                             cave("deep", {{2500.0, 1200.0, 4000.0}, {5500.0, 1000.0, 4500.0}}, 250.0)};
    terrain::VolumeSettings s;
    const terrain::VolumeResult r = terrain::buildVolume(h, ops, s);
    INFO("voxel " << r.voxel << " pieces " << r.pieces.size() << " holes " << r.holeCells << " buried " << r.buriedCells);
    CHECK(r.voxel == doctest::Approx(200.0 / 3.0));
    CHECK(r.holeCells > 0);
    CHECK(r.buriedCells > 0);
    std::set<int> regions;
    for (const terrain::VolumePiece& p : r.pieces)
    {
        regions.insert(p.region);
        CHECK(p.lods[0]->numFaces() > 0);
        CHECK(p.lods[0]->faceAttrs().has(geo::attr::kMaterial));
        CHECK(p.lods[0]->cornerAttrs().has(geo::attr::uvName(0)));
    }
    CHECK(regions.size() == 2);
    checkSplit(h, r);

    // The hole lies where the cave goes under the hill, not over the deep one; the plateau's height there is untouched.
    const std::vector<uint8_t> holes = terrain::holeCells(h);
    auto holeAt = [&](double x, double z) {
        const int cx = int((x - h.origin.x) / h.cell), cz = int((z - h.origin.y) / h.cell);
        return holes[size_t(cz) * (h.nx - 1) + cx] != 0;
    };
    // Out on the flat the cave is an open trench the heightfield holds; it goes under the hill on the slope.
    CHECK(h.heightAt(-2900.0, 0.0) < -100.0);
    bool slopeHole = false;
    for (double x = -2500.0; x <= 500.0; x += 100.0)
        for (double z = -800.0; z <= 1100.0; z += 100.0)
            slopeHole = slopeHole || holeAt(x, z);
    CHECK(slopeHole);
    CHECK_FALSE(holeAt(4000.0, 4200.0));
    CHECK(h.heightAt(4000.0, 4200.0) == doctest::Approx(before.heightAt(4000.0, 4200.0)).epsilon(1e-4));
    // The deep cave's piece lies under the ground: nothing of it rises above the plateau.
    for (const terrain::VolumePiece& p : r.pieces)
        if (p.holeCells == 0)
            CHECK(p.bounds.max.y < 3000.0);
}

TEST_CASE("volume: the split is the same bytes at any thread count and on every run")
{
    const std::vector<terrain::VolumeOp> ops{cave("c", {{-3000.0, 150.0, 0.0}, {0.0, 800.0, 300.0}, {4000.0, 1500.0, 0.0}}, 300.0)};
    auto build = [&](size_t threads) {
        tbb::global_control limit(tbb::global_control::max_allowed_parallelism, threads);
        geo::Heightfield h = hillside();
        terrain::VolumeResult r = terrain::buildVolume(h, ops, {});
        return std::pair(std::move(h), std::move(r));
    };
    const auto [h1, r1] = build(1);
    const auto [h8, r8] = build(8);
    const auto [h8b, r8b] = build(8);
    CHECK(h1.heights == h8.heights);
    CHECK(*h1.findMask("holes") == *h8.findMask("holes"));
    REQUIRE(r1.pieces.size() == r8.pieces.size());
    for (size_t p = 0; p < r1.pieces.size(); ++p)
    {
        const auto a = r1.pieces[p].lods[0]->positions(), b = r8.pieces[p].lods[0]->positions(), c = r8b.pieces[p].lods[0]->positions();
        REQUIRE(a.size() == b.size());
        CHECK(std::equal(a.begin(), a.end(), b.begin()));
        CHECK(std::equal(b.begin(), b.end(), c.begin()));
    }
}

TEST_CASE("volume: what the heightfield can hold goes into it; split off keeps the heights and meshes the change")
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {8000.0, 8000.0}, 41);
    terrain::VolumeOp box;
    box.id = "plinth";
    box.kind = "box";
    box.subtract = false;
    box.center = {0.0, 0.0, 0.0};
    box.size = {1200.0, 1000.0, 1200.0};
    terrain::VolumeResult r = terrain::buildVolume(h, {box}, {});
    CHECK(r.heightChanged > 0);
    CHECK(r.pieces.empty());
    CHECK(r.holeCells == 0);
    CHECK(h.heightAt(0.0, 0.0) == doctest::Approx(500.0).epsilon(0.01));
    CHECK(h.findMask("holes") == nullptr);
    CHECK(r.heightError > 100.0);

    // Asked to, the split meshes the plinth's sheer sides the heightfield's cells can't hold.
    geo::Heightfield fine = geo::makeHeightfieldCovering({0.0, 0.0}, {8000.0, 8000.0}, 41);
    terrain::VolumeSettings strict;
    strict.maxHeightError = 50.0;
    r = terrain::buildVolume(fine, {box}, strict);
    CHECK(r.pieces.size() == 1);
    CHECK(r.heightError <= 50.0);
    checkSplit(fine, r);

    geo::Heightfield flat = geo::makeHeightfieldCovering({0.0, 0.0}, {8000.0, 8000.0}, 41);
    terrain::VolumeSettings off;
    off.split = false;
    r = terrain::buildVolume(flat, {box}, off);
    CHECK(r.heightChanged == 0);
    CHECK(flat.heightAt(0.0, 0.0) == doctest::Approx(0.0));
    CHECK(r.pieces.size() == 1);
    CHECK(r.holeCells > 0);
    checkSplit(flat, r);
}

TEST_CASE("volume: overhangs, arches, tunnels and mesh-free stamps all become pieces with clean seams")
{
    SUBCASE("overhang")
    {
        geo::Heightfield h = hillside();
        terrain::VolumeOp op;
        op.id = "ledge";
        op.kind = "overhang";
        op.path = {{-1000.0, 0.0, -3000.0}, {-1000.0, 0.0, 3000.0}};
        op.depth = 600.0;
        op.height = 1200.0;
        op.strata = 250.0;
        op.lip = 200.0;
        const terrain::VolumeResult r = terrain::buildVolume(h, {op}, {});
        CHECK(r.pieces.size() >= 1);
        CHECK(r.holeCells > 0);
        checkSplit(h, r);
    }
    SUBCASE("arch")
    {
        geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {12800.0, 12800.0}, 65);
        terrain::VolumeOp op;
        op.id = "arch";
        op.kind = "arch";
        op.subtract = false;
        op.path = {{-2000.0, 0.0, 0.0}, {2000.0, 0.0, 0.0}};
        op.height = 1500.0;
        op.thickness = 300.0;
        op.width = 500.0;
        op.noise = 0.1;
        const terrain::VolumeResult r = terrain::buildVolume(h, {op}, {});
        CHECK(r.pieces.size() >= 1);
        CHECK(r.holeCells > 0);
        geo::Bounds b;
        for (const auto& m : lod0(r))
            b.extend(m->bounds());
        CHECK(b.max.y > 1400.0);
        checkSplit(h, r);
    }
    SUBCASE("tunnel through a ridge")
    {
        geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {12800.0, 12800.0}, 65);
        for (int z = 0; z < h.nz; ++z)
            for (int x = 0; x < h.nx; ++x)
                h.heights[h.index(x, z)] = float(2500.0 * std::exp(-std::pow(h.samplePos(x, z).x / 2000.0, 2.0)));
        terrain::VolumeOp op;
        op.id = "bore";
        op.kind = "tunnel";
        op.path = {{-4500.0, 0.0, 0.0}, {4500.0, 0.0, 0.0}};
        op.radius = 400.0;
        op.noise = 0.0;
        const terrain::VolumeResult r = terrain::buildVolume(h, {op}, {});
        CHECK(r.pieces.size() >= 1);
        CHECK(r.buriedCells > 0);
        CHECK(r.holeCells > 0);
        checkSplit(h, r);
    }
}

TEST_CASE("volume: levels of detail coarsen each piece and keep their seams")
{
    geo::Heightfield h = hillside();
    terrain::VolumeSettings s;
    s.lods = 3;
    const terrain::VolumeResult r =
        terrain::buildVolume(h, {cave("c", {{-3000.0, 150.0, 0.0}, {0.0, 800.0, 300.0}, {4000.0, 1500.0, 0.0}}, 300.0)}, s);
    REQUIRE(!r.pieces.empty());
    for (int level = 0; level < 3; ++level)
    {
        std::vector<std::shared_ptr<const geo::Mesh>> meshes;
        for (const terrain::VolumePiece& p : r.pieces)
            if (p.lods.size() > size_t(level))
                meshes.push_back(p.lods[size_t(level)]);
        REQUIRE(!meshes.empty());
        const terrain::SeamReport seams = terrain::measureSeams(h, meshes);
        CHECK_MESSAGE(seams.uncovered == 0.0, "LOD" << level);
        CHECK_MESSAGE(seams.maxGap < 0.01, "LOD" << level);
    }
    const terrain::VolumePiece& big = *std::max_element(r.pieces.begin(), r.pieces.end(), [](const auto& a, const auto& b) {
        return a.lods[0]->numFaces() < b.lods[0]->numFaces();
    });
    REQUIRE(big.lods.size() == 3);
    CHECK(big.lods[1]->numFaces() < big.lods[0]->numFaces());
    CHECK(big.lods[2]->numFaces() < big.lods[1]->numFaces());
}

TEST_CASE("volume commands: caves split, check clean, export their pieces and undo")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {20000.0, 20000.0}}, {"resolution", 101}, {"amplitude", 3000.0}, {"featureSize", 15000.0}, {"seed", 2},
                                   {"name", "Hill"}});
    const double g0 = heightAt(t, -6000.0, 0.0), g1 = heightAt(t, 6000.0, 0.0);
    json c = run("terrainCave", {{"args", {t["mesh"]}},
                                 {"path", json::array({json::array({-6000.0, g0 - 100.0, 0.0}), json::array({0.0, std::min(g0, g1) - 1500.0, 0.0}),
                                                       json::array({6000.0, g1 - 100.0, 0.0})})},
                                 {"radius", 300.0},
                                 {"branches", 1},
                                 {"chambers", 1}});
    INFO(c.dump());
    CHECK(c["applied"] == true);
    CHECK(c["volume"]["pieceCount"].get<int>() > 0);
    CHECK(c["volume"]["unmeshedColumns"].get<int>() == 0);
    CHECK(c["volume"]["seamUncovered"].get<double>() == doctest::Approx(0.0));

    json check = run("terrainCheck", {{"args", {t["mesh"]}}});
    for (const json& i : check["issues"])
        CHECK_MESSAGE(!i["type"].get<std::string>().starts_with("volume"), i.dump());

    json split = run("terrainSplit", {{"args", {t["mesh"]}}, {"lods", 2}});
    CHECK(split["volume"]["pieces"][0]["lodFaces"].size() == 2);

    const fs::path dir = fs::temp_directory_path() / "lila_terrain_volume_export";
    fs::remove_all(dir);
    json ex = run("terrainExport", {{"args", {t["mesh"]}}, {"target", "unreal"}, {"folder", dir.string()}});
    REQUIRE(ex.contains("volume"));
    CHECK(fs::exists(ex["files"]["visibility"].get<std::string>()));
    for (const json& p : ex["volume"]["pieces"])
        CHECK(fs::file_size(p["file"].get<std::string>()) > 1000);
    for (const json& l : ex["layers"])
        CHECK(l["mask"] != "holes");

    run("undo");
    run("undo");
    json v = run("terrainVolume", {{"args", {t["mesh"]}}});
    CHECK(v["ops"].empty());
    fs::remove_all(dir);
}
