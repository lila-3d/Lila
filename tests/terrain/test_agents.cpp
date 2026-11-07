#include "TestUtil.h"

#include <cmath>

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
