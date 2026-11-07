#include "TestUtil.h"

#include "core/ImageIO.h"
#include "scene/Graph.h"
#include "render/HeightfieldRenderer.h"
#include "ui/HeightfieldDisplay.h"

#include <cmath>
#include <filesystem>
#include <set>

using namespace terraintest;

namespace
{
/** Two round hills 16000 cm apart along X joined by a lower ridge: a saddle between them at the origin. */
json twoHills(const std::string& name)
{
    json t = run("terrainCreate", {{"size", {40000.0, 40000.0}}, {"resolution", 129}, {"flat", true}, {"name", name}});
    for (double x : {-8000.0, 8000.0})
        run("terrainStroke", {{"args", {name}}, {"brush", "raise"}, {"points", {{x, 0.0}}}, {"radius", 8000.0}, {"strength", 3000.0}});
    // A lower ridge joining them, so the ground between is a saddle.
    run("terrainStroke", {{"args", {name}}, {"brush", "raise"}, {"path", {{-8000.0, 0.0}, {8000.0, 0.0}}}, {"radius", 4000.0}, {"strength", 1000.0}});
    return run("terrainInfo", {{"args", {name}}});
}
} // namespace

TEST_CASE("agents: terrainQuery finds the peak, the pass and slopes, and stores regions other commands take by name")
{
    loadTerrain();
    run("newScene");
    const json t = twoHills("QueryHills");
    const double top = t["heightRange"][1].get<double>();

    json peak = run("terrainQuery", {{"args", {"QueryHills"}}, {"named", "highestPeak"}, {"store", "peak"}});
    REQUIRE(peak["points"].size() == 1);
    CHECK(peak["points"][0][1].get<double>() == doctest::Approx(top).epsilon(0.001));
    CHECK(std::abs(std::abs(peak["points"][0][0].get<double>()) - 8000.0) < 1500.0);
    CHECK(peak["mask"] == "region_peak");
    CHECK(run("terrainInfo", {{"args", {"QueryHills"}}})["masks"].contains("region_peak"));

    // A reference resolves to the stored point wherever a command takes one.
    const json s = run("terrainSample", {{"args", {"QueryHills"}}, {"points", {"region:peak"}}});
    CHECK(s[0]["height"].get<double>() == doctest::Approx(top).epsilon(0.001));

    json pass = run("terrainQuery", {{"args", {"QueryHills"}}, {"named", "passes"}, {"store", "pass"}, {"pick", 0}});
    REQUIRE(pass["points"].size() >= 1);
    CHECK(std::abs(pass["points"][0][0].get<double>()) < 1000.0);
    CHECK(std::abs(pass["points"][0][2].get<double>()) < 1000.0);

    // North-facing (downhill toward -Z) slopes lie on the -Z side of the hills.
    json north = run("terrainQuery", {{"args", {"QueryHills"}}, {"named", "northSlopes"}});
    REQUIRE(north["regions"].size() >= 1);
    CHECK(north["regions"][0]["centroid"][1].get<double>() < -1000.0);
    json high = run("terrainQuery", {{"args", {"QueryHills"}}, {"where", {{"height", {top * 0.8, nullptr}}}}});
    REQUIRE(high["regions"].size() == 2);
    CHECK(high["regions"][0]["height"]["min"].get<double>() >= top * 0.8 - 1.0);

    json flat = run("terrainQuery", {{"args", {"QueryHills"}}, {"named", "flatAreas"}, {"minArea", 1000.0}, {"store", "plain"}});
    REQUIRE(flat["regions"].size() >= 1);
    CHECK(flat["regions"][0]["slope"]["max"].get<double>() <= 5.0);
    // A polygon reference: a lake outlined by the stored region's largest part.
    json stats = run("terrainRegionStats", {{"args", {"QueryHills"}}, {"polygon", "region:plain"}});
    CHECK(stats.dump().size() > 10);

    json listed = run("terrainQuery", {{"args", {"QueryHills"}}, {"list", true}});
    CHECK(listed["regions"].contains("peak"));
    CHECK(listed["regions"].contains("pass"));
    run("terrainQuery", {{"args", {"QueryHills"}}, {"remove", "plain"}});
    CHECK_FALSE(run("terrainQuery", {{"args", {"QueryHills"}}, {"list", true}})["regions"].contains("plain"));
    CHECK_THROWS(run("terrainSample", {{"args", {"QueryHills"}}, {"points", {"region:nowhere"}}}));
    run("newScene");
}

TEST_CASE("agents: terrainSpec builds a terrain from one spec and reads it back as a spec that rebuilds it")
{
    loadTerrain();
    run("newScene");
    const json spec = {
        {"name", "SpecHills"},
        {"size", {60000.0, 60000.0}},
        {"resolution", 129},
        {"seed", 7},
        {"source", {{"type", "noise"}, {"amplitude", 8000.0}, {"featureSize", 30000.0}, {"ridged", 0.4}}},
        {"erosion", {{"preset", "hills"}, {"age", 0.4}}},
        {"regions", {{{"name", "low"}, {"named", "lowestPoint"}}, {{"name", "top"}, {"named", "highestPeak"}}}},
        {"roads", {{{"id", "climb"}, {"kind", "track"}, {"route", {{"start", "region:low"}, {"end", "region:top"}, {"maxGrade", 15.0}}}}}},
        {"materials", true},
    };
    json r = run("terrainSpec", {{"spec", spec}});
    INFO(r.dump().substr(0, 2000));
    CHECK(r["ok"] == true);
    std::vector<std::string> steps;
    for (const json& s : r["steps"])
    {
        CHECK(s["ok"] == true);
        steps.push_back(s["step"]);
    }
    CHECK(steps == std::vector<std::string>{"source", "erosion", "regions.low", "regions.top", "roads", "materials", "check"});
    CHECK(r["stats"]["resolution"][0] == 129);
    const std::string rev = r["revision"];

    // Read back while unchanged: the authored spec; rebuilt under another name, the same ground.
    json back = run("terrainSpec", {{"args", {"SpecHills"}}});
    CHECK(back["from"] == "spec");
    json again = back["spec"];
    again["name"] = "SpecHills2";
    json r2 = run("terrainSpec", {{"spec", again}});
    CHECK(r2["revision"] == rev);

    // Edited after the build: read back as its node group, which rebuilds the same ground too.
    run("terrainStroke", {{"args", {"SpecHills"}}, {"brush", "raise"}, {"points", {{0.0, 0.0}}}, {"radius", 8000.0}, {"strength", 1500.0}, {"stage", "final"}});
    const std::string edited = run("terrainInfo", {{"args", {"SpecHills"}}})["revision"];
    CHECK(edited != rev);
    json graph = run("terrainSpec", {{"args", {"SpecHills"}}});
    CHECK(graph["from"] == "graph");
    json g2 = graph["spec"];
    g2["name"] = "SpecHills3";
    json r3 = run("terrainSpec", {{"spec", g2}});
    CHECK(r3["revision"] == edited);

    // Same name again needs replace; unknown keys are refused.
    CHECK_THROWS(run("terrainSpec", {{"spec", spec}}));
    CHECK(run("terrainSpec", {{"spec", spec}, {"replace", true}})["revision"] == rev);
    CHECK_THROWS(run("terrainSpec", {{"spec", {{"name", "X"}, {"mountains", 3}}}}));
    run("newScene");
}

TEST_CASE("agents: every terrainSpec preset builds and passes its steps")
{
    loadTerrain();
    run("newScene");
    const json presets = run("terrainSpec", {{"list", true}});
    REQUIRE(presets.size() == 5);
    for (const json& p : presets)
    {
        const std::string name = p["name"];
        CAPTURE(name);
        CHECK(!p["description"].get<std::string>().empty());
        json r = run("terrainSpec", {{"preset", name}, {"overrides", {{"resolution", 129}}}});
        for (const json& s : r["steps"])
        {
            INFO(s.dump().substr(0, 800));
            CHECK(s["ok"] == true);
        }
        INFO(r["check"].dump());
        CHECK(r["ok"] == true);
    }
    run("newScene");
}

TEST_CASE("agents: terrainWedge builds spec variants in parallel lilabatch processes into a labelled contact sheet")
{
    loadTerrain();
    run("newScene");
    const json spec = {{"name", "WedgeHills"},
                       {"size", {40000.0, 40000.0}},
                       {"resolution", 65},
                       {"source", {{"type", "noise"}, {"amplitude", 6000.0}, {"featureSize", 20000.0}}},
                       {"erosion", {{"preset", "hills"}, {"age", 0.3}}}};
    const std::string folder = (std::filesystem::temp_directory_path() / "lila_terrain_tests_wedge").string();
    json w = run("terrainWedge", {{"spec", spec}, {"vary", {{"seed", {1, 2, 3}}, {"erosion.age", {0.2, 0.5, 0.2}}}}, {"count", 3}, {"tile", 96}, {"folder", folder}});
    INFO(w.dump().substr(0, 3000));
    CHECK(w["mode"] == "parallel");
    REQUIRE(w["variants"].size() == 3);
    std::set<std::string> revisions;
    for (const json& v : w["variants"])
    {
        CHECK(v["ok"] == true);
        CHECK(v["image"].is_string());
        revisions.insert(v["revision"].get<std::string>());
    }
    CHECK(revisions.size() == 3);
    CHECK(w["variants"][1]["vary"]["erosion.age"] == 0.5);
    const lila::image::Image sheet = lila::image::read(w["sheet"].get<std::string>());
    CHECK(sheet.width == 2 * (96 + 6) + 6);
    CHECK(sheet.height > 2 * 96);
    CHECK(std::filesystem::exists(w["json"].get<std::string>()));
    // Nothing is left in the scene, and the same variant built here gives the worker's ground.
    CHECK(run("ls", {{"type", "mesh"}}).size() == 0);
    json one = spec;
    one["seed"] = 2;
    one["erosion"]["age"] = 0.5;
    CHECK(run("terrainSpec", {{"spec", one}})["revision"] == w["variants"][1]["revision"]);

    json seq = run("terrainWedge", {{"spec", spec}, {"count", 2}, {"tile", 64}, {"parallel", false}, {"folder", folder}});
    CHECK(seq["mode"] == "inProcess");
    CHECK(seq["note"] == "parallel=False");
    CHECK(seq["variants"][0]["ok"] == true);
    CHECK(seq["variants"][1]["vary"]["seed"] == 1);
    run("newScene");
}

TEST_CASE("agents: material rules set the heightfield draw's paint layers, one per layer in its colour")
{
    loadTerrain();
    if (!ops::find("terrainDisplay"))
        ui::heightfieldDisplay::registerCommands();
    run("newScene");
    const json t = run("terrainCreate", {{"size", {60000.0, 60000.0}}, {"resolution", 129}, {"erosion", 20}, {"name", "Painted"}});
    json rules = run("terrainMaterialRules", {{"args", {"Painted"}}, {"set", {{"name", "moss"}, {"color", {0.1, 0.4, 0.05}}, {"when", {{"slope", {0.0, 20.0}}}}}}});
    const size_t count = rules["layers"].size();
    REQUIRE(count >= 3);
    REQUIRE(rules["displayLayers"].size() == count);

    json shown = run("terrainDisplayLayers", {{"args", {"Painted"}}});
    REQUIRE(shown["layers"].size() == count);
    for (size_t i = 0; i < count; ++i)
    {
        const std::string name = rules["layers"][i]["name"];
        CHECK(shown["layers"][i]["name"] == name);
        CHECK(shown["layers"][i]["mask"] == "layer_" + name);
        CHECK(shown["layers"][i]["found"] == true);
    }
    CHECK(rules["displayLayers"].back()["color"] == "#59aa3f");

    scene::Graph& g = scene::Graph::current();
    ui::heightfieldDisplay::Settings set;
    std::optional<render::HeightfieldDraw> d = ui::heightfieldDisplay::drawFor(g, g.find("Painted")->id(), set, true);
    REQUIRE(d);
    REQUIRE(d->layers.size() == count);
    CHECK(d->layers.back().color.g == doctest::Approx(0.4f).epsilon(0.02));

    // A rule change reaches the draw again.
    run("terrainMaterialRules", {{"args", {"Painted"}}, {"remove", {"moss"}}});
    CHECK(run("terrainDisplayLayers", {{"args", {"Painted"}}})["layers"].size() == count - 1);
    run("newScene");
}
