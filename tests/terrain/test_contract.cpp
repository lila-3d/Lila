#include "TestUtil.h"
#include "Analysis.h"
#include "Stamps.h"
#include "Terrain.h"

#include "extensions/ExtensionManager.h"
#include "geo/Heightfield.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"
#include "render/Renderer.h"
#include "ui/Camera.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <set>

using namespace lila;
using namespace terraintest;
using json = nlohmann::json;

namespace
{
/** A valley along X falling toward -X, steep sides, and a bowl that holds a lake. 2.56 km, 10 m cells. */
std::shared_ptr<geo::Heightfield> valley()
{
    auto h = std::make_shared<geo::Heightfield>(geo::makeHeightfieldCovering({0.0, 0.0}, {256000.0, 256000.0}, 257));
    for (int z = 0; z < h->nz; ++z)
        for (int x = 0; x < h->nx; ++x)
        {
            const glm::dvec2 p = h->samplePos(x, z);
            double v = std::abs(p.y) * 0.15 + (p.x + 128000.0) * 0.02;
            v -= 1500.0 * std::exp(-glm::dot(p - glm::dvec2(60000.0, 0.0), p - glm::dvec2(60000.0, 0.0)) / (2.0 * 8000.0 * 8000.0));
            h->heights[h->index(x, z)] = float(v);
        }
    return h;
}
} // namespace

TEST_CASE("analysis: a valley drains to a river with a lake, and the derived fields follow the ground")
{
    auto h = valley();
    terrain::AnalysisParams p;
    p.riverArea = 2.0e6;
    const auto a = terrain::analyse(h, p);
    REQUIRE_FALSE(a->reaches().empty());
    REQUIRE_FALSE(a->lakes().empty());
    // The valley floor is river and low above it; the ridge is high and dry.
    CHECK(a->sample("hand", -60000.0, 0.0) < 100.0);
    CHECK(a->sample("hand", -60000.0, 100000.0) > 5000.0);
    CHECK(a->sample("floodRisk", -60000.0, 0.0) > 0.8);
    CHECK(a->sample("floodRisk", -60000.0, 100000.0) < 0.05);
    CHECK(a->sample("slope", -60000.0, 60000.0) > 5.0);
    CHECK(a->sample("streamOrder", -100000.0, 0.0) >= 1.0);
    CHECK(a->sample("distanceToWater", -60000.0, 50000.0) > 10000.0);
    const terrain::Lake& lake = a->lakes().front();
    CHECK(lake.maxDepth > 200.0);
    CHECK(lake.outlet != UINT32_MAX);

    const json w = terrain::waterGraph(*a, {});
    REQUIRE_FALSE(w["rivers"].empty());
    const json& pt = w["rivers"][0]["points"][0];
    CHECK(pt["width"].get<double>() > 0.0);
    CHECK(pt["waterLevel"].get<double>() > pt["bedHeight"].get<double>());
    CHECK(pt.contains("bankTypeL"));
    REQUIRE(w["lakes"].size() >= 1);
    CHECK_FALSE(w["lakes"][0]["shoreline"].empty());
    CHECK(w["springs"].size() >= 1);

    const json regions = terrain::buildableRegions(*a, {}, 1000.0, 15.0, true, 0.3);
    REQUIRE_FALSE(regions.empty());
    CHECK(regions[0]["area"].get<double>() >= 1000.0);
    CHECK(regions[0]["meanSlope"].get<double>() <= 15.0);

    const json centres = terrain::sites(*a, {}, "townCentre", 3, 20000.0);
    CHECK(centres.size() >= 1);
    const json castles = terrain::sites(*a, {}, "castle", 2, 20000.0);
    CHECK(castles.size() >= 1);

    glm::dvec3 hit;
    REQUIRE(terrain::raycast(*h, {-60000.0, 1e6, 30000.0}, {0.0, -1.0, 0.0}, &hit));
    CHECK(hit.y == doctest::Approx(h->heightAt(-60000.0, 30000.0)).epsilon(1e-4));
    const auto lines = terrain::contourLines(h->heights, h->nx, h->nz, 3000.0f);
    CHECK_FALSE(lines.empty());
}

TEST_CASE("line features: streets hold their grade, canals carve to their water, levees rise, quays split land and water")
{
    geo::Heightfield h = *valley();
    terrain::Stamp street;
    street.kind = "street";
    street.width = 800.0;
    street.falloff = 300.0;
    street.maxGrade = 6.0;
    street.masks = terrain::defaultMasks("street");
    street.path = {{-100000.0, 80000.0}, {-100000.0, -80000.0}};
    const terrain::StampResult r = terrain::applyStamp(h, street, {});
    REQUIRE(r.centreline.size() > 10);
    CHECK(r.maxGrade <= 6.0 + 1e-6);
    CHECK(h.maskAt("paved", -100000.0, 0.0) == doctest::Approx(1.0));

    terrain::Stamp canal;
    canal.kind = "canal";
    canal.width = 1000.0;
    canal.depth = 300.0;
    canal.level = terrain::StampLevel::Value;
    canal.height = 1000.0;
    canal.path = {{-120000.0, -20000.0}, {-120000.0, 20000.0}};
    geo::Heightfield c = *valley();
    const terrain::StampResult cr = terrain::applyStamp(c, canal, {});
    CHECK(cr.waterLevel == doctest::Approx(1000.0));
    CHECK(c.heightAt(-120000.0, 10000.0) == doctest::Approx(std::min(700.0, double(valley()->heightAt(-120000.0, 10000.0)))).epsilon(1e-3));

    terrain::Stamp levee;
    levee.kind = "levee";
    levee.width = 600.0;
    levee.rise = 250.0;
    levee.path = {{0.0, -3000.0}, {40000.0, -3000.0}};
    geo::Heightfield l = *valley();
    terrain::applyStamp(l, levee, {});
    CHECK(l.heightAt(20000.0, -3000.0) == doctest::Approx(valley()->heightAt(20000.0, -3000.0) + 250.0).epsilon(1e-3));

    terrain::Stamp quay;
    quay.kind = "quay";
    quay.width = 4000.0;
    quay.depth = 400.0;
    quay.level = terrain::StampLevel::Value;
    quay.height = 2000.0;
    quay.side = "left";
    quay.path = {{-50000.0, 10000.0}, {-30000.0, 10000.0}};
    geo::Heightfield q = *valley();
    terrain::applyStamp(q, quay, {});
    // Travelling +X, left is +Z: land there at the quay top, the water side dredged below it.
    CHECK(q.heightAt(-40000.0, 11000.0) == doctest::Approx(2000.0).epsilon(1e-3));
    CHECK(q.heightAt(-40000.0, 9000.0) <= 1600.0 + 1e-3);
}

TEST_CASE("contract commands: water, fields, profiles, stats, sites, contours, features and checks on a terrain")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {400000.0, 400000.0}}, {"resolution", 129}, {"amplitude", 30000.0}, {"seed", 2}, {"erosion", 30}, {"name", "City"}});
    const std::string mesh = t["mesh"].get<std::string>();
    const json common = {{"riverArea", 1.0e6}};

    json w = run("terrainWaterInfo", {{"args", {mesh}}, {"riverArea", 1.0e6}});
    CHECK(w.contains("revision"));
    REQUIRE_FALSE(w["rivers"].empty());
    CHECK(w["rivers"][0]["points"][0].contains("discharge"));

    json f = run("terrainField", {{"args", {mesh}}, {"field", "buildability"}, {"resolution", 16}});
    CHECK(f["grid"]["values"].size() > 100);
    CHECK(f["max"].get<double>() <= 1.0);

    json prof = run("terrainProfile", {{"args", {mesh}}, {"path", {{-150000.0, 0.0}, {150000.0, 0.0}}}, {"step", 2000.0}, {"fields", {"floodRisk"}}, {"riverArea", 1.0e6}});
    CHECK(prof["samples"].size() == 151);
    CHECK(prof["samples"][10].contains("crossSlope"));
    CHECK(prof["samples"][10].contains("floodRisk"));

    json stats = run("terrainRegionStats", {{"args", {mesh}}, {"polygon", {{0.0, 0.0}, {20000.0, 0.0}, {20000.0, 20000.0}, {0.0, 20000.0}}}});
    CHECK(stats["fields"]["height"]["max"].get<double>() >= stats["fields"]["height"]["min"].get<double>());
    CHECK(stats["area"].get<double>() > 30000.0);

    json pts = run("terrainSample", {{"args", {mesh}}, {"points", {{0.0, 0.0}}}, {"fields", {"hand", "slopeClass", "solar"}}});
    CHECK(pts[0].contains("hand"));
    CHECK(pts[0]["slopeClass"].get<double>() >= 0.0);

    CHECK(run("terrainBuildable", {{"args", {mesh}}, {"minArea", 500.0}, {"maxSlope", 25.0}})["regions"].size() >= 1);
    CHECK(run("terrainSites", {{"args", {mesh}}, {"kind", "temple"}, {"count", 2}})["sites"].size() >= 1);
    CHECK_FALSE(run("terrainContours", {{"args", {mesh}}, {"interval", 3000.0}})["levels"].empty());
    json streets = run("terrainContourStreets", {{"args", {mesh}}, {"region", {{-50000.0, -50000.0}, {50000.0, -50000.0}, {50000.0, 50000.0}, {-50000.0, 50000.0}}},
                                                  {"spacing", 4000.0}});
    CHECK_FALSE(streets["streets"].empty());
    json ray = run("terrainRaycast", {{"args", {mesh}}, {"origin", {1000.0, 1e6, 2000.0}}, {"direction", {0.0, -1.0, 0.0}}});
    REQUIRE(ray["hit"] == true);
    CHECK(ray["position"][1].get<double>() == doctest::Approx(heightAt(t, 1000.0, 2000.0)).epsilon(1e-3));
    json solid = run("terrainRaycast", {{"args", {mesh}}, {"isSolid", {{1000.0, -1e6, 2000.0}, {1000.0, 1e6, 2000.0}}}});
    CHECK(solid == json::array({true, false}));

    // Features: a street across the land, a canal, a quay; all live in the stamp layers and come back from terrainStamps.
    const std::string rev0 = run("terrainInfo", {{"args", {mesh}}})["revision"].get<std::string>();
    json st = run("terrainAddFeature", {{"args", {mesh}}, {"kind", "street"}, {"id", "main"}, {"layer", "City"},
                                         {"path", {{-120000.0, -30000.0}, {0.0, 10000.0}, {120000.0, -20000.0}}}, {"maxGrade", 8.0}});
    CHECK(st["applied"] == true);
    CHECK(st["maxGrade"].get<double>() <= 8.0 + 1e-6);
    CHECK(st["centreline"].size() > 10);
    json canal = run("terrainAddFeature", {{"args", {mesh}}, {"kind", "canal"}, {"id", "cut"}, {"path", {{-20000.0, 60000.0}, {20000.0, 60000.0}}}, {"width", 1200.0}});
    CHECK(canal.contains("waterLevel"));
    run("terrainStampQuay", {{"args", {mesh}}, {"kind", "levee"}, {"id", "bank"}, {"path", {{-20000.0, 90000.0}, {20000.0, 90000.0}}}, {"rise", 300.0}});
    json dredged = run("terrainStampRegion", {{"args", {mesh}}, {"id", "dock"}, {"mode", "dredge"}, {"depth", 250.0}, {"falloff", 0.0},
                                               {"footprint", {{60000.0, 60000.0}, {70000.0, 60000.0}, {70000.0, 70000.0}, {60000.0, 70000.0}}}});
    CHECK(dredged["cutM3"].get<double>() > 0.0);
    json all = run("terrainStamps", {{"args", {mesh}}});
    CHECK(all["layers"]["City"].size() == 1);
    CHECK(all["layers"]["Stamps"].size() == 3);
    json water = run("terrainWaterInfo", {{"args", {mesh}}, {"riverArea", 1.0e6}});
    CHECK(water["canals"].size() == 1);

    // What changed since rev0 lies around the edits.
    json dirty = run("terrainDirtyRegions", {{"args", {mesh}}, {"since", rev0}});
    CHECK(dirty["known"] == true);
    CHECK_FALSE(dirty["regions"].empty());
    CHECK(run("terrainDirtyRegions", {{"args", {mesh}}, {"since", dirty["revision"]}})["regions"].empty());

    // A box hovering over the terrain floats; one sunk into it doesn't.
    json box = run("polyCube", {{"width", 1000.0}, {"height", 1000.0}, {"depth", 1000.0}, {"name", "Hut"}});
    const std::string hut = box.is_array() ? box[0].get<std::string>() : box.get<std::string>();
    const double g0 = heightAt(t, 30000.0, -60000.0);
    run("setAttr", {{"args", {hut + ".translate", {30000.0, g0 + 2000.0, -60000.0}}}});
    json check = run("terrainCheck", {{"args", {mesh}}, {"buildings", {hut}}, {"riverArea", 1.0e6}});
    bool floating = false;
    for (const json& i : check["issues"])
        floating = floating || (i["type"] == "floating" && i["about"] == hut);
    CHECK(floating);
    run("setAttr", {{"args", {hut + ".translate", {30000.0, g0 - 2000.0, -60000.0}}}});
    check = run("terrainCheck", {{"args", {mesh}}, {"buildings", {hut}}, {"riverArea", 1.0e6}});
    for (const json& i : check["issues"])
        CHECK_FALSE((i["type"] == "floating" && i["about"] == hut));
}

TEST_CASE("geometry nodes: Terrain Field, Terrain Water, Buildable Regions and Terrain Sites run in a group")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {200000.0, 200000.0}}, {"resolution", 65}, {"amplitude", 20000.0}, {"name", "Nodes"}});
    json spec = {{"name", "WaterNodes"},
                 {"nodes", json::array({{{"name", "Grid"}, {"type", "HeightfieldGrid"}, {"values", {{"sizeX", 200000.0}, {"sizeZ", 200000.0}, {"resolution", 65}}}},
                                        {{"name", "Noise"}, {"type", "TerrainNoise"}, {"values", {{"amplitude", 20000.0}}}},
                                        {{"name", "Field"}, {"type", "TerrainField"}, {"props", {{"field", "floodRisk"}}}},
                                        {{"name", "Water"}, {"type", "TerrainWater"}, {"values", {{"riverArea", 1.0e6}}}},
                                        {{"name", "Build"}, {"type", "BuildableRegions"}},
                                        {{"name", "Sites"}, {"type", "TerrainSites"}, {"props", {{"kind", "castle"}}}}})},
                 {"links", json::array({"Grid.heightfield -> Noise.heightfield", "Noise.heightfield -> Field.heightfield", "Field.heightfield -> Water.heightfield",
                                        "Field.heightfield -> Build.heightfield", "Field.heightfield -> Sites.heightfield", "Field.heightfield -> GroupOutput.geometry"})}};
    run("gnBuildGroup", {{"args", {spec}}});
    run("gnApply", {{"args", {t["mesh"], "WaterNodes"}}});
    json info = run("terrainInfo", {{"args", {t["mesh"]}}});
    CHECK(info["masks"].contains("floodRisk"));
}

TEST_CASE("sculpt: an edit after erosion reuses the eroded result instead of eroding again")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {200000.0, 200000.0}}, {"resolution", 513}, {"amplitude", 20000.0}, {"erosion", 60}, {"name", "Slow"}});
    auto timed = [&](json call) {
        const auto t0 = std::chrono::steady_clock::now();
        run("terrainSculpt", call);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    const double base = timed({{"args", {t["mesh"]}}, {"op", "raise"}, {"points", {{0.0, 0.0}}}, {"radius", 10000.0}, {"strength", 500.0}, {"stage", "base"}});
    const double fin1 = timed({{"args", {t["mesh"]}}, {"op", "raise"}, {"points", {{20000.0, 0.0}}}, {"radius", 10000.0}, {"strength", 500.0}, {"stage", "final"}});
    const double fin2 = timed({{"args", {t["mesh"]}}, {"op", "lower"}, {"points", {{-20000.0, 0.0}}}, {"radius", 10000.0}, {"strength", 500.0}, {"stage", "final"}});
    MESSAGE("base stroke " << base << " s, final strokes " << fin1 << " s and " << fin2 << " s");
    CHECK(fin2 < base * 0.5);
    // The replace and remove flags the viewport tool uses keep one growing stroke.
    run("terrainSculpt", {{"args", {t["mesh"]}}, {"op", "raise"}, {"path", {{0.0, 50000.0}, {10000.0, 50000.0}}}, {"stage", "final"}, {"replaceLast", true}});
    json n = run("terrainSculpt", {{"args", {t["mesh"]}}, {"stage", "final"}, {"removeLast", true}});
    CHECK(n["strokes"] == 1);
}

TEST_CASE("heightfield draw path: a heightfield renders offscreen from its heights alone")
{
    auto& r = render::Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU for the heightfield draw test");
        return;
    }
    auto h = valley();
    render::FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = 1;
    s.background = glm::vec3(0.0f);
    ui::Camera cam;
    cam.frame(glm::dvec3(0.0, 5000.0, 0.0), 180000.0, 1.0);
    s.view = cam.view();
    s.proj = cam.proj(1.0);
    s.eye = cam.eye();
    std::vector<uint8_t> empty, drawn;
    std::string err;
    REQUIRE_MESSAGE(r.renderOffscreen(s, 64, 64, empty, &err), err);
    render::HeightfieldDraw d;
    d.heights = std::shared_ptr<const std::vector<float>>(h, &h->heights);
    d.nx = h->nx;
    d.nz = h->nz;
    d.cell = float(h->cell);
    d.origin = glm::vec2(h->origin);
    d.color = glm::vec3(0.6f, 0.5f, 0.4f);
    s.heightfields.push_back(d);
    REQUIRE_MESSAGE(r.renderOffscreen(s, 64, 64, drawn, &err), err);
    auto lit = [](const std::vector<uint8_t>& px) {
        size_t n = 0;
        for (size_t i = 0; i + 3 < px.size(); i += 4)
            if (px[i] + px[i + 1] + px[i + 2] > 30)
                ++n;
        return n;
    };
    CHECK(lit(empty) < 10);
    CHECK(lit(drawn) > 64 * 64 / 8);
    // Lit by its slopes: the valley's two sides shade differently.
    std::set<int> shades;
    for (size_t i = 0; i + 3 < drawn.size(); i += 4)
        shades.insert(drawn[i + 1] / 16);
    CHECK(shades.size() >= 3);
}
