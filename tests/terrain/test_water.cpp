#include "TestUtil.h"
#include "Erosion.h"
#include "Stamps.h"
#include "Water.h"

#include "geo/Heightfield.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <tbb/task_arena.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

using namespace lila;
using namespace terraintest;
using json = nlohmann::json;

namespace
{
/** A valley along X falling toward -X with a bowl that holds a lake, and small bumps. 2.56 km, 10 m cells. */
geo::Heightfield waterValley()
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {256000.0, 256000.0}, 257);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            double v = std::abs(p.y) * 0.15 + (p.x + 128000.0) * 0.02;
            v -= 1500.0 * std::exp(-glm::dot(p - glm::dvec2(60000.0, 0.0), p - glm::dvec2(60000.0, 0.0)) / (2.0 * 8000.0 * 8000.0));
            v += 120.0 * std::sin(p.x * 0.0004) * std::cos(p.y * 0.0005);
            h.heights[h.index(x, z)] = float(v);
        }
    return h;
}

/** A gentle valley falling toward -X with a 15 m cliff across it at x = 0. 1 m cells. */
geo::Heightfield cliffValley()
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {30000.0, 12000.0}, 301);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            const double step = 1500.0 / (1.0 + std::exp(-p.x / 40.0));
            h.heights[h.index(x, z)] = float(std::abs(p.y) * 0.1 + (p.x + 15000.0) * 0.01 + step);
        }
    return h;
}

/** Every basin of the filled surface holds some lake or sea: nothing is left undrained. */
int undrainedBasins(const geo::Heightfield& h, const terrain::WaterResult& r)
{
    const terrain::Drainage d = terrain::drain(h);
    std::vector<uint8_t> water(h.size(), 0);
    for (const terrain::LakeResult& l : r.lakes)
        for (uint32_t i : l.cells)
            water[i] = 1;
    // A sample in a basin (the filled surface above the ground) must be under a lake or the sea.
    int dry = 0;
    for (size_t i = 0; i < h.size(); ++i)
        if (d.filled[i] - h.heights[i] > 1.0f && !water[i])
            ++dry;
    return dry;
}

/** The channel floor under a centreline point: the lowest of the four samples round it. */
double floorAt(const geo::Heightfield& h, glm::dvec2 p)
{
    const int x = std::clamp(int(std::floor((p.x - h.origin.x) / h.cell)), 0, h.nx - 2);
    const int z = std::clamp(int(std::floor((p.y - h.origin.y) / h.cell)), 0, h.nz - 2);
    return std::min({h.at(x, z), h.at(x + 1, z), h.at(x, z + 1), h.at(x + 1, z + 1)});
}

/** The worst rise of the carved channel floor along a river from source to mouth, cm. */
double worstRise(const geo::Heightfield& h, const terrain::RiverResult& r)
{
    double worst = 0.0;
    for (size_t k = 1; k < r.points.size(); ++k)
        worst = std::max(worst, floorAt(h, r.points[k].p) - floorAt(h, r.points[k - 1].p));
    return worst;
}
} // namespace

TEST_CASE("water: auto rivers have monotonic beds, size with drainage, and every sample drains to the edge or a lake")
{
    geo::Heightfield h = waterValley();
    terrain::WaterSpec spec;
    terrain::RiverSpec river;
    river.id = "main";
    river.source = "auto";
    river.threshold = 3.0e6;
    river.minWidth = 10.0;
    river.minDepth = 5.0;
    spec.rivers.push_back(river);
    terrain::LakeSpec lakes;
    lakes.id = "lakes";
    lakes.source = "auto";
    lakes.minDepth = 100.0;
    spec.lakes.push_back(lakes);
    const terrain::WaterResult r = terrain::applyWater(h, spec);
    REQUIRE(r.rivers.size() >= 2);
    REQUIRE_FALSE(r.lakes.empty());

    double worstDesigned = 0.0, worstGround = 0.0;
    int grows = 0, checked = 0;
    for (const terrain::RiverResult& rr : r.rivers)
    {
        REQUIRE(rr.points.size() >= 2);
        for (size_t k = 1; k < rr.points.size(); ++k)
            worstDesigned = std::max(worstDesigned, rr.points[k].bed - rr.points[k - 1].bed);
        worstGround = std::max(worstGround, worstRise(h, rr));
        // Width and depth follow drainage: more area downstream, never narrower or shallower.
        for (size_t k = 1; k < rr.points.size(); ++k)
        {
            CHECK(rr.points[k].area >= rr.points[k - 1].area);
            CHECK(rr.points[k].width >= rr.points[k - 1].width - 1e-9);
            CHECK(rr.points[k].depth >= rr.points[k - 1].depth - 1e-9);
        }
        const terrain::WaterPoint& last = rr.points.back();
        if (last.width > 10.0)
        {
            // Hydraulic geometry: width 300 sqrt(Q) cm and depth 30 Q^0.4 cm, Q = 0.02 m3/s per km2.
            CHECK(last.width == doctest::Approx(300.0 * std::sqrt(last.discharge)).epsilon(1e-6));
            CHECK(last.depth == doctest::Approx(30.0 * std::pow(last.discharge, 0.4)).epsilon(1e-6));
            ++checked;
        }
        if (rr.points.back().width > rr.points.front().width)
            ++grows;
    }
    CHECK(worstDesigned <= 0.0);
    MESSAGE("worst rise of a channel floor: ", worstGround, " cm");
    CHECK(worstGround < 1.0);
    CHECK(checked >= 1);
    CHECK(grows >= 1);
    // The biggest river carries the most water and is the widest.
    const auto widest = std::max_element(r.rivers.begin(), r.rivers.end(), [](const auto& a, const auto& b) { return a.points.back().width < b.points.back().width; });
    const auto wettest = std::max_element(r.rivers.begin(), r.rivers.end(), [](const auto& a, const auto& b) { return a.points.back().area < b.points.back().area; });
    CHECK(widest == wettest);

    CHECK(undrainedBasins(h, r) == 0);
    const terrain::Drainage d = terrain::drain(h);
    for (const terrain::LakeResult& l : r.lakes)
    {
        // Flat at the spill level: the lowest ground round it is the level, and the filled surface over it is that level.
        CHECK(std::abs(l.level - l.spillHeight) < 1.0);
        for (uint32_t i : l.cells)
        {
            INFO(l.id, " ", l.kind, " level ", l.level, " spill ", l.spillHeight, " cells ", l.cells.size(), " at ", i, " h ", h.heights[i], " filled ", d.filled[i]);
            REQUIRE(h.heights[i] < l.level);
            REQUIRE(std::abs(double(d.filled[i]) - l.level) < 1.0);
        }
    }
    for (const char* m : {"riverBed", "riverBank", "water", "lakeBed", "flow"})
    {
        INFO(std::string(m));
        CHECK(h.findMask(m) != nullptr);
    }
}

TEST_CASE("water: a drawn river is turned downhill, keeps a waterfall at a cliff with a pool below, and gravel bars form in bends")
{
    geo::Heightfield h = cliffValley();
    const float cliffTop = float(h.heightAt(500.0, 0.0));
    terrain::WaterSpec spec;
    terrain::RiverSpec river;
    river.id = "falls";
    river.path = {{-15000.0, 0.0}, {-6000.0, 1500.0}, {0.0, 0.0}, {6000.0, -1500.0}, {15000.0, 0.0}};
    river.width = 400.0;
    river.depth = 80.0;
    spec.rivers.push_back(river);
    const terrain::WaterResult r = terrain::applyWater(h, spec);
    REQUIRE(r.rivers.size() == 1);
    const terrain::RiverResult& rr = r.rivers[0];
    CHECK(rr.reversed);
    CHECK(rr.mouth == "edge");
    REQUIRE(r.waterfalls.size() == 1);
    const terrain::Waterfall& f = r.waterfalls[0];
    CHECK(f.drop > 1000.0);
    CHECK(f.top.x > f.bottom.x);
    CHECK(std::abs(f.top.x) < 400.0);
    CHECK_FALSE(f.pool.empty());
    const auto pool = std::find_if(r.lakes.begin(), r.lakes.end(), [&](const terrain::LakeResult& l) { return l.id == f.pool; });
    REQUIRE(pool != r.lakes.end());
    CHECK(pool->kind == "pool");
    CHECK(pool->maxDepth > 50.0);
    // The bed falls everywhere but in the plunge pool, which is a lake of its own.
    for (size_t k = 1; k < rr.points.size(); ++k)
        CHECK(rr.points[k].bed <= rr.points[k - 1].bed);
    double worst = 0.0;
    for (size_t k = 1; k < rr.points.size(); ++k)
    {
        const glm::dvec2 p = rr.points[k].p;
        if (glm::length(p - glm::dvec2(f.bottom.x, f.bottom.z)) < 1500.0)
            continue;
        worst = std::max(worst, floorAt(h, p) - floorAt(h, rr.points[k - 1].p));
    }
    CHECK(worst < 1.0);
    // The fall stays sheer: the ground at the cliff still drops over a few metres.
    CHECK(h.heightAt(300.0, 0.0) - h.heightAt(-300.0, 0.0) > 600.0);
    // Upstream of it the channel is cut in: the centreline sits a depth below the old ground.
    const geo::Heightfield before = cliffValley();
    for (const terrain::WaterPoint& p : rr.points)
        if (p.p.x > 3000.0 && p.p.x < 12000.0)
        {
            CHECK(h.heightAt(p.p.x, p.p.y) < before.heightAt(p.p.x, p.p.y) - 0.9 * river.depth);
            CHECK(h.heightAt(p.p.x, p.p.y) < cliffTop + 200.0);
        }
    REQUIRE(h.findMask("waterfall"));
    REQUIRE(h.findMask("gravelBar"));
    CHECK(*std::max_element(h.findMask("gravelBar")->begin(), h.findMask("gravelBar")->end()) > 0.2f);
    CHECK(undrainedBasins(h, r) == 0);
    CHECK(r.springs.size() == 1);
}

TEST_CASE("water: lakes fill a basin to its spill, a drawn lake digs and dams, and the sea makes beaches")
{
    geo::Heightfield h = waterValley();
    terrain::WaterSpec spec;
    terrain::LakeSpec bowl;
    bowl.id = "bowl";
    bowl.at = {60000.0, 0.0};
    spec.lakes.push_back(bowl);
    terrain::LakeSpec pond;
    pond.id = "pond";
    pond.source = "region";
    pond.polygon = {{-60000.0, 30000.0}, {-50000.0, 30000.0}, {-50000.0, 40000.0}, {-60000.0, 40000.0}};
    pond.level = 2000.0 + 35000.0 * 0.15 + 50.0;
    spec.lakes.push_back(pond);
    spec.settings.seaLevel = 500.0;
    const terrain::WaterResult r = terrain::applyWater(h, spec);
    auto find = [&](const std::string& id) { return std::find_if(r.lakes.begin(), r.lakes.end(), [&](const auto& l) { return l.id == id; }); };
    for (const std::string& w : r.warnings)
        MESSAGE(w);
    REQUIRE(find("bowl") != r.lakes.end());
    REQUIRE(find("pond") != r.lakes.end());
    REQUIRE(find("sea") != r.lakes.end());
    const terrain::LakeResult& b = *find("bowl");
    CHECK(b.maxDepth > 500.0);
    CHECK(std::abs(b.level - b.spillHeight) < 1.0);
    const terrain::LakeResult& p = *find("pond");
    CHECK(p.level == doctest::Approx(pond.level));
    CHECK(p.damHeight > 0.0);
    CHECK(p.maxDepth >= 250.0);
    // The dam holds it: the lowest ground round it is at the level, so the filled surface over it is too.
    CHECK(std::abs(p.spillHeight - p.level) < 1.0);
    const terrain::Drainage d = terrain::drain(h);
    for (uint32_t i : p.cells)
        REQUIRE(std::abs(double(d.filled[i]) - p.level) < 1.0);
    CHECK(find("sea")->kind == "sea");
    REQUIRE(h.findMask("beach"));
    REQUIRE(h.findMask("shore"));
    CHECK(*std::max_element(h.findMask("beach")->begin(), h.findMask("beach")->end()) > 0.5f);
    CHECK(undrainedBasins(h, r) == 0);
    CHECK(r.warnings.empty());

    // A level above what a basin holds is refused out loud, never silently.
    geo::Heightfield h2 = waterValley();
    terrain::WaterSpec over;
    terrain::LakeSpec high = bowl;
    high.level = 20000.0;
    over.lakes.push_back(high);
    const terrain::WaterResult r2 = terrain::applyWater(h2, over);
    REQUIRE_FALSE(r2.warnings.empty());
    CHECK(r2.warnings[0].find("spill") != std::string::npos);
}

TEST_CASE("water: the same spec gives the same bytes at any thread count")
{
    terrain::WaterSpec spec;
    terrain::RiverSpec river;
    river.id = "main";
    river.source = "auto";
    river.threshold = 3.0e6;
    river.meander = 1.0;
    spec.rivers.push_back(river);
    spec.settings.seaLevel = 300.0;
    geo::Heightfield a = waterValley(), b = waterValley();
    terrain::applyWater(a, spec);
    tbb::task_arena one(1);
    one.execute([&] { terrain::applyWater(b, spec); });
    CHECK(a.heights == b.heights);
    CHECK(a.masks == b.masks);
}

TEST_CASE("water: the export is splines and locators, and Unreal gets the same in its axes")
{
    geo::Heightfield h = cliffValley();
    terrain::WaterSpec spec;
    terrain::RiverSpec river;
    river.id = "falls";
    river.path = {{14000.0, 0.0}, {-14000.0, 0.0}};
    spec.rivers.push_back(river);
    terrain::LakeSpec pond;
    pond.id = "pond";
    pond.source = "region";
    pond.polygon = {{-12000.0, 3000.0}, {-9000.0, 3000.0}, {-9000.0, 5000.0}, {-12000.0, 5000.0}};
    spec.lakes.push_back(pond);
    const terrain::WaterResult r = terrain::applyWater(h, spec);
    const json w = terrain::waterJson(r, {}, std::numeric_limits<double>::quiet_NaN());
    CHECK(w["format"] == "lila.terrainWater");
    REQUIRE(w["splines"].size() >= 2);
    const json& s = w["splines"][0];
    CHECK(s["kind"] == "river");
    for (const char* k : {"width", "depth", "bedHeight", "waterLevel", "flowSpeed", "flowDirection"})
        CHECK(s["points"][0].contains(k));
    std::set<std::string> kinds;
    for (const json& l : w["locators"])
        kinds.insert(l["kind"].get<std::string>());
    for (const char* k : {"spring", "waterfallTop", "waterfallBottom", "lakeLevel"})
        CHECK(kinds.count(k) == 1);
    const json ue = terrain::unrealWaterParams(w, "abc", "Terrain");
    CHECK(ue["id"] == "abc");
    const json& p = ue["splines"][0]["points"][0];
    // Unreal is Z-up: the water level is the point's Z there.
    CHECK(p["waterLevel"].get<double>() == doctest::Approx(p["position"][2].get<double>()));
    CHECK(p["position"][2].get<double>() == doctest::Approx(s["points"][0]["waterLevel"].get<double>()));
}

TEST_CASE("water: terrainAddRiver, terrainAddLake and terrainSeaLevel author water that exports and undoes")
{
    loadTerrain();
    run("newScene");
    const json t = run("terrainCreate", {{"size", {100000.0, 100000.0}}, {"resolution", 129}, {"amplitude", 8000.0}, {"seed", 3}});
    const std::string mesh = t["mesh"];
    const json river = run("terrainAddRiver", {{"args", {mesh}}, {"id", "main"}, {"auto", true}, {"threshold", 2.0e4}});
    CHECK(river["id"] == "main");
    CHECK_FALSE(river["rivers"].empty());
    CHECK_FALSE(river["splines"].empty());
    CHECK(river["preview"].is_string());
    const json lakes = run("terrainAddLake", {{"args", {mesh}}, {"id", "lakes"}, {"auto", true}, {"minDepth", 20.0}, {"minArea", 100.0}});
    const json sea = run("terrainSeaLevel", {{"args", {mesh}}, {"level", 1500.0}});
    CHECK(sea["seaLevel"].get<double>() == doctest::Approx(1500.0));
    const json info = run("terrainWater", {{"args", {mesh}}});
    CHECK(info["features"]["rivers"].size() == 1);
    CHECK(info["features"]["lakes"].size() == 1);
    CHECK_FALSE(info["water"]["locators"].empty());

    const auto dir = std::filesystem::temp_directory_path() / "lila_water_export";
    const json files = run("terrainExport", {{"args", {mesh}}, {"target", "files"}, {"folder", dir.string()}, {"name", "w"}});
    REQUIRE(files.contains("water"));
    std::ifstream in(dir / "w_water.json");
    REQUIRE(in);
    const json back = json::parse(in);
    CHECK(back["splines"].size() == info["water"]["splines"].size());

    run("terrainWaterRemove", {{"args", {mesh}}, {"ids", {"main"}}});
    CHECK(run("terrainWater", {{"args", {mesh}}})["features"]["rivers"].empty());
    run("undo");
    CHECK(run("terrainWater", {{"args", {mesh}}})["features"]["rivers"].size() == 1);
    run("terrainWaterRemove", {{"args", {mesh}}, {"all", true}});
    CHECK(run("terrainWater", {{"args", {mesh}}})["water"].is_null());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("water: a 1K terrain's river network carves in seconds and still drains")
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {1024000.0, 1024000.0}, 1025);
    terrain::NoiseParams np;
    np.seed = 7;
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(terrain::noiseHeight(np, p.x, p.y));
        }
    terrain::ErosionParams ep;
    ep.iterations = 20;
    terrain::erode(h, ep);
    terrain::WaterSpec spec;
    terrain::RiverSpec river;
    river.id = "net";
    river.source = "auto";
    river.threshold = 2.0e6;
    river.meander = 0.5;
    spec.rivers.push_back(river);
    terrain::LakeSpec lakes;
    lakes.id = "lakes";
    lakes.source = "auto";
    spec.lakes.push_back(lakes);
    const auto t0 = std::chrono::steady_clock::now();
    const terrain::WaterResult r = terrain::applyWater(h, spec);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("1025^2: ", r.rivers.size(), " rivers, ", r.lakes.size(), " lakes, ", r.pitsFilled, " pits filled, ", seconds, " s");
    CHECK_FALSE(r.rivers.empty());
    CHECK(undrainedBasins(h, r) == 0);
    double worst = 0.0;
    for (const terrain::RiverResult& rr : r.rivers)
        worst = std::max(worst, worstRise(h, rr));
    MESSAGE("worst rise of a channel floor: ", worst, " cm");
    CHECK(worst < 1.0);
}
