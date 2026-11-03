#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "core/PrefStore.h"
#include "scene/MeasureMath.h"

#include <cmath>
#include <filesystem>
#include <numbers>
#include <random>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;
namespace measure = lila::scene::measure;

namespace
{
/** Points on a tilted circle from a0 to a1 degrees. */
std::vector<glm::dvec3> arc(glm::dvec3 c, double r, double a0, double a1, int n)
{
    glm::dvec3 u = glm::normalize(glm::dvec3(1, 0.3, 0.2)), w = glm::normalize(glm::cross(u, glm::dvec3(0, 1, 0)));
    glm::dvec3 v = glm::cross(w, u);
    std::vector<glm::dvec3> out;
    for (int i = 0; i < n; ++i)
    {
        double a = (a0 + (a1 - a0) * i / (n - 1)) * std::numbers::pi / 180.0;
        out.push_back(c + (u * std::cos(a) + v * std::sin(a)) * r);
    }
    return out;
}
} // namespace

TEST_CASE("circle fit")
{
    SUBCASE("full circle")
    {
        auto pts = arc({10, -4, 7}, 25.0, 0.0, 345.0, 24);
        measure::Circle c = measure::fitCircle(pts);
        CHECK(c.radius == doctest::Approx(25.0).epsilon(1e-9));
        CHECK(glm::distance(c.center, glm::dvec3(10, -4, 7)) < 1e-8);
        CHECK(c.rms < 1e-8);
        CHECK(c.planarity < 1e-8);
        CHECK(c.arcDegrees > 340.0);
    }
    SUBCASE("half arc")
    {
        auto pts = arc({0, 0, 0}, 3.5, 0.0, 180.0, 9);
        measure::Circle c = measure::fitCircle(pts);
        CHECK(c.radius == doctest::Approx(3.5).epsilon(1e-9));
        CHECK(glm::length(c.center) < 1e-8);
        CHECK(c.arcDegrees == doctest::Approx(180.0).epsilon(1e-6));
    }
    SUBCASE("short arc, three points")
    {
        auto pts = arc({5, 5, 5}, 1000.0, 10.0, 25.0, 3);
        CHECK(measure::fitCircle(pts).radius == doctest::Approx(1000.0).epsilon(1e-6));
    }
    SUBCASE("noisy partial arc")
    {
        std::mt19937 rng(7);
        std::normal_distribution<double> noise(0.0, 0.05);
        auto pts = arc({0, 100, 0}, 40.0, -30.0, 120.0, 60);
        for (glm::dvec3& p : pts)
            p += glm::dvec3(noise(rng), noise(rng), noise(rng));
        measure::Circle c = measure::fitCircle(pts);
        CHECK(c.radius == doctest::Approx(40.0).epsilon(0.005));
        CHECK(glm::distance(c.center, glm::dvec3(0, 100, 0)) < 0.3);
        CHECK(c.rms < 0.15);
        CHECK(c.planarity > 0.0);
    }
    SUBCASE("collinear and degenerate input throw")
    {
        std::vector<glm::dvec3> line = {{0, 0, 0}, {1, 1, 1}, {2, 2, 2}, {5, 5, 5}};
        CHECK_THROWS_AS(measure::fitCircle(line), std::invalid_argument);
        std::vector<glm::dvec3> two = {{0, 0, 0}, {1, 0, 0}};
        CHECK_THROWS_AS(measure::fitCircle(two), std::invalid_argument);
        std::vector<glm::dvec3> same = {{1, 2, 3}, {1, 2, 3}, {1, 2, 3}};
        CHECK_THROWS_AS(measure::fitCircle(same), std::invalid_argument);
    }
    SUBCASE("plane fit reports deviation")
    {
        std::vector<glm::dvec3> pts = {{0, 0, 0}, {10, 0, 0}, {10, 0, 10}, {0, 1, 10}};
        measure::Plane p = measure::fitPlane(pts);
        CHECK(std::abs(p.normal.y) > 0.99);
        CHECK(p.maxDeviation == doctest::Approx(0.25).epsilon(0.05));
    }
}

TEST_CASE("distance, angle and area math")
{
    CHECK(measure::angleDegrees({1, 0, 0}, {0, 0, 0}, {0, 0, 1}) == doctest::Approx(90.0));
    CHECK(measure::angleDegrees({1, 0, 0}, {0, 0, 0}, {-1, 1e-9, 0}) == doctest::Approx(180.0));
    CHECK(measure::angleDegrees({2, 0, 0}, {1, 0, 0}, {2, 1, 0}) == doctest::Approx(45.0));
    CHECK_THROWS_AS(measure::angleDegrees({0, 0, 0}, {0, 0, 0}, {1, 0, 0}), std::invalid_argument);
    CHECK(measure::cornerOf({glm::dvec3(1, 0, 0), glm::dvec3(0, 0, 0), glm::dvec3(0, 1, 0)}) == 1);

    std::vector<glm::dvec3> square = {{0, 0, 0}, {10, 0, 0}, {10, 0, 10}, {0, 0, 10}};
    CHECK(measure::polygonArea(square) == doctest::Approx(100.0));
    CHECK(measure::polylineLength(square, true) == doctest::Approx(40.0));
    CHECK(measure::polylineLength(square, false) == doctest::Approx(30.0));
    std::vector<glm::dvec3> tri = {{0, 5, 0}, {3, 5, 0}, {0, 5, 4}};
    CHECK(measure::polygonArea(tri) == doctest::Approx(6.0));

    CHECK(measure::unitFactor("m") == doctest::Approx(0.01));
    CHECK(measure::unitFactor("in") == doctest::Approx(1.0 / 2.54));
    PrefStore::get().set("general", "/units/linear", "mm");
    CHECK(measure::formatLength(2.5) == "25.00 mm");
    PrefStore::get().set("general", "/units/linear", "cm");
    CHECK(measure::formatLength(-0.0001) == "0.00 cm");
}

TEST_CASE("measure nodes and measureSelection")
{
    PrefStore::get().set("general", "/units/linear", "cm");
    run("newScene");

    std::string d = run("distanceMeasure", {{"start", {0, 0, 0}}, {"end", {30, 40, 0}}});
    CHECK(run("getAttr", {{"args", {d + ".distance"}}}).get<double>() == doctest::Approx(50.0));
    // The endpoints are locators: moving one updates the measurement.
    json locs = run("ls", {{"type", "locator"}});
    REQUIRE(locs.size() == 2);
    std::string endLoc = run("getAttr", {{"args", {locs[0].get<std::string>() + ".tx"}}}).get<double>() > 1.0 ? locs[0] : locs[1];
    run("move", {{"args", {endLoc}}, {"to", {0, 10, 0}}});
    CHECK(run("getAttr", {{"args", {d + ".distance"}}}).get<double>() == doctest::Approx(10.0));
    run("undo");
    CHECK(run("getAttr", {{"args", {d + ".distance"}}}).get<double>() == doctest::Approx(50.0));

    run("select", {{"args", {d}}});
    json r = run("measureSelection");
    CHECK(r["kind"] == "distance");
    CHECK(r["distance"].get<double>() == doctest::Approx(50.0));
    CHECK(r["dy"].get<double>() == doctest::Approx(40.0));
    CHECK(r["text"] == "50.00 cm");

    std::string a = run("angleMeasure", {{"points", {{10, 0, 0}, {0, 0, 0}, {0, 0, 10}}}});
    CHECK(run("getAttr", {{"args", {a + ".angle"}}}).get<double>() == doctest::Approx(90.0));

    // A cylinder: its cap ring is a circle, the object reads as cylinder-like.
    std::string cyl = run("polyCylinder", {{"radius", 12.5}, {"height", 40}, {"subdivisionsAxis", 24}})[0];
    run("select", {{"args", {cyl}}});
    r = run("measureSelection");
    CHECK(r["kind"] == "cylinder");
    CHECK(r["diameter"].get<double>() == doctest::Approx(25.0).epsilon(1e-6));
    CHECK(r["height"].get<double>() == doctest::Approx(40.0).epsilon(1e-6));

    std::string cube = run("polyCube", {{"width", 20}, {"height", 20}, {"depth", 20}})[0];
    run("select", {{"args", {cube}}});
    r = run("measureSelection");
    CHECK(r["kind"] == "bounds");
    CHECK(r["width"].get<double>() == doctest::Approx(20.0));
    CHECK_FALSE(r.contains("diameter"));

    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    r = run("measureSelection");
    CHECK(r["kind"] == "area");
    CHECK(r["area"].get<double>() == doctest::Approx(400.0));
    CHECK(r["perimeter"].get<double>() == doctest::Approx(80.0));

    run("selectComponents", {{"args", {cube}}, {"vertices", {0, 1}}});
    r = run("measureSelection");
    CHECK(r["kind"] == "distance");
    CHECK(r["distance"].get<double>() == doctest::Approx(20.0));

    run("selectComponents", {{"args", {cube}}, {"vertices", {0, 1, 3}}});
    r = run("measureSelection");
    CHECK(r["kind"] == "angle");
    CHECK(r["angle"].get<double>() == doctest::Approx(90.0));

    // The border of one cube face is a closed loop of four edges: circle through its corners.
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("convertSelection", {{"args", {"edge"}}});
    r = run("measureSelection");
    CHECK(r["kind"] == "loop");
    CHECK(r["length"].get<double>() == doctest::Approx(80.0));
    CHECK(r["diameter"].get<double>() == doctest::Approx(20.0 * std::sqrt(2.0)));

    // Live radius from the selected vertices: scaling the mesh scales the measured radius.
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("convertSelection", {{"args", {"vertex"}}});
    std::string rad = run("radiusMeasure");
    double r0 = run("getAttr", {{"args", {rad + ".radius"}}}).get<double>();
    CHECK(r0 == doctest::Approx(10.0 * std::sqrt(2.0)));
    run("selectMode", {{"args", {"object"}}});
    run("setAttr", {{"args", {cube + ".scale", {2, 2, 2}}}});
    CHECK(run("getAttr", {{"args", {rad + ".radius"}}}).get<double>() == doctest::Approx(2.0 * r0));

    std::string fixed = run("radiusMeasure", {{"points", {{1, 0, 0}, {0, 1, 0}, {-1, 0, 0}}}, {"display", "diameter"}});
    CHECK(run("getAttr", {{"args", {fixed + ".diameter"}}}).get<double>() == doctest::Approx(2.0));
    CHECK_THROWS_AS(run("radiusMeasure", {{"points", {{0, 0, 0}, {1, 0, 0}}}}), CommandError);

    // Measure nodes survive a save and load.
    auto path = (std::filesystem::temp_directory_path() / "lila_tests" / "measure.lilaa").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(run("getAttr", {{"args", {d + ".distance"}}}).get<double>() == doctest::Approx(50.0));
    CHECK(run("getAttr", {{"args", {rad + ".radius"}}}).get<double>() == doctest::Approx(2.0 * r0));
    CHECK(run("getAttr", {{"args", {a + ".angle"}}}).get<double>() == doctest::Approx(90.0));
}
