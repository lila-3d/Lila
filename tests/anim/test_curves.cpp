#include <doctest/doctest.h>

#include "scene/AnimCurve.h"

#include <cmath>

using namespace lila::scene::anim;

namespace
{
Curve make(std::initializer_list<std::pair<double, double>> pts, Tangent in, Tangent out)
{
    Curve c;
    for (auto [t, v] : pts)
        c.keys.push_back({.time = t, .value = v, .in = in, .out = out});
    return c;
}

double maxOver(const Curve& c, double t0, double t1)
{
    double m = -1e300;
    for (int i = 0; i <= 1000; ++i)
        m = std::max(m, c.evaluate(t0 + (t1 - t0) * i / 1000.0));
    return m;
}

double slopeAt(const Curve& c, double t, double side)
{
    const double h = 1e-5;
    return side < 0 ? (c.evaluate(t) - c.evaluate(t - h)) / h : (c.evaluate(t + h) - c.evaluate(t)) / h;
}
} // namespace

TEST_CASE("linear tangents interpolate straight")
{
    Curve c = make({{0, 0}, {1, 10}}, Tangent::Linear, Tangent::Linear);
    CHECK(c.evaluate(0.5) == doctest::Approx(5));
    CHECK(c.evaluate(0.25) == doctest::Approx(2.5));
    CHECK(c.evaluate(1.0) == doctest::Approx(10));
}

TEST_CASE("flat tangents ease in and out with zero slope at keys")
{
    Curve c = make({{0, 0}, {1, 10}}, Tangent::Flat, Tangent::Flat);
    CHECK(c.evaluate(0.5) == doctest::Approx(5));
    CHECK(c.evaluate(0.25) == doctest::Approx(1.5625));
    CHECK(slopeAt(c, 0.0, 1) == doctest::Approx(0).epsilon(1e-3));
    CHECK(slopeAt(c, 1.0, -1) == doctest::Approx(0).epsilon(1e-3));
}

TEST_CASE("step holds until the next key")
{
    Curve c = make({{0, 0}, {1, 10}, {2, 3}}, Tangent::Auto, Tangent::Step);
    CHECK(c.evaluate(0.0) == 0);
    CHECK(c.evaluate(0.999) == 0);
    CHECK(c.evaluate(1.0) == 10);
    CHECK(c.evaluate(1.7) == 10);
    CHECK(c.evaluate(2.0) == 3);
}

TEST_CASE("spline overshoots where auto and clamped hold the plateau")
{
    std::initializer_list<std::pair<double, double>> pts = {{0, 0}, {1, 10}, {2, 10}};
    CHECK(maxOver(make(pts, Tangent::Spline, Tangent::Spline), 1, 2) > 10.1);
    CHECK(maxOver(make(pts, Tangent::Auto, Tangent::Auto), 0, 2) <= doctest::Approx(10));
    CHECK(maxOver(make(pts, Tangent::Clamped, Tangent::Clamped), 0, 2) <= doctest::Approx(10));
}

TEST_CASE("auto is smooth through a key and never overshoots monotonic data")
{
    Curve c = make({{0, 0}, {1, 2}, {3, 10}}, Tangent::Auto, Tangent::Auto);
    CHECK(slopeAt(c, 1.0, -1) == doctest::Approx(slopeAt(c, 1.0, 1)).epsilon(1e-3));
    CHECK(c.outSlope(1) == doctest::Approx(10.0 / 3.0));

    // A near-plateau: spline would bulge above the last key.
    Curve steep = make({{0, 0}, {1, 9}, {2, 10}}, Tangent::Auto, Tangent::Auto);
    double prev = -1;
    for (int i = 0; i <= 200; ++i)
    {
        double v = steep.evaluate(2.0 * i / 200.0);
        CHECK(v >= prev - 1e-9);
        prev = v;
    }
    CHECK(maxOver(steep, 0, 2) <= doctest::Approx(10));
    // Ends of an auto curve are flat, as in Maya.
    CHECK(steep.inSlope(0) == 0);
    CHECK(steep.outSlope(2) == 0);
    // A local extreme flattens.
    Curve peak = make({{0, 0}, {1, 10}, {2, 0}}, Tangent::Auto, Tangent::Auto);
    CHECK(peak.outSlope(1) == 0);
}

TEST_CASE("fixed tangents use their angle")
{
    Curve c = make({{0, 0}, {1, 0}}, Tangent::Fixed, Tangent::Fixed);
    c.keys[0].outAngle = 45;
    CHECK(c.outSlope(0) == doctest::Approx(1));
    CHECK(slopeAt(c, 0.0, 1) == doctest::Approx(1).epsilon(1e-3));
}

TEST_CASE("weighted curves with default weights match unweighted ones")
{
    Curve a = make({{0, 0}, {1, 4}, {3, 1}}, Tangent::Spline, Tangent::Spline);
    Curve b = a;
    b.weighted = true;
    for (double t : {0.1, 0.5, 0.9, 1.5, 2.7})
        CHECK(b.evaluate(t) == doctest::Approx(a.evaluate(t)));
    b.keys[0].outWeight = 2.5;
    CHECK(b.evaluate(0.5) != doctest::Approx(a.evaluate(0.5)));
    CHECK(b.evaluate(1.0) == doctest::Approx(4));
}

TEST_CASE("infinity modes")
{
    Curve c = make({{0, 0}, {1, 10}}, Tangent::Linear, Tangent::Linear);
    CHECK(c.evaluate(-1) == 0);
    CHECK(c.evaluate(2) == 10);

    c.pre = c.post = Infinity::Linear;
    CHECK(c.evaluate(2) == doctest::Approx(20));
    CHECK(c.evaluate(-1) == doctest::Approx(-10));

    c.pre = c.post = Infinity::Cycle;
    CHECK(c.evaluate(1.5) == doctest::Approx(5));
    CHECK(c.evaluate(-0.25) == doctest::Approx(7.5));
    CHECK(c.evaluate(3.25) == doctest::Approx(2.5));

    c.pre = c.post = Infinity::CycleRelative;
    CHECK(c.evaluate(1.5) == doctest::Approx(15));
    CHECK(c.evaluate(-0.25) == doctest::Approx(-2.5));
    CHECK(c.evaluate(2.5) == doctest::Approx(25));

    c.pre = c.post = Infinity::Oscillate;
    CHECK(c.evaluate(1.25) == doctest::Approx(7.5));
    CHECK(c.evaluate(2.25) == doctest::Approx(2.5));
    CHECK(c.evaluate(-0.25) == doctest::Approx(2.5));
}

TEST_CASE("keys round trip through json")
{
    Curve c = make({{0, 1}, {0.5, 2}}, Tangent::Auto, Tangent::Auto);
    c.keys[0].out = Tangent::Fixed;
    c.keys[0].outAngle = 30;
    c.keys[1].in = Tangent::Step;
    c.keys[1].inWeight = 2;
    CHECK(Curve::keysFromJson(c.keysToJson()) == c.keys);
    CHECK_THROWS(Curve::keysFromJson(lila::scene::json::parse(R"([{"t": 0}])")));
    CHECK_THROWS(Curve::keysFromJson(lila::scene::json::parse(R"([{"t": 0, "v": 1, "o": "wobbly"}])")));
}
