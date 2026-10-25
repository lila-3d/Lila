#include <doctest/doctest.h>

#include "curve/CurveBoolean.h"
#include "curve/Planar.h"
#include "curve/Skeleton.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace lila;
using D2 = glm::dvec2;
using D3 = glm::dvec3;

namespace
{
curve::Spline polyLoop(std::initializer_list<D3> list)
{
    curve::Spline s;
    s.kind = curve::Kind::Poly;
    s.closed = true;
    s.points = list;
    s.normalize();
    return s;
}

curve::Spline rect(double x0, double z0, double x1, double z1)
{
    return polyLoop({D3(x0, 0, z0), D3(x1, 0, z0), D3(x1, 0, z1), D3(x0, 0, z1)});
}

double polyArea(const std::vector<D2>& p) { return curve::signedArea(p); }

double faceArea(const curve::StraightSkeleton& s, const std::vector<int>& f)
{
    std::vector<D2> p;
    for (int n : f)
        p.push_back(s.nodes[size_t(n)]);
    return polyArea(p);
}

/** Every face traced, all counter-clockwise, their areas summing to the region's. */
void checkSkeleton(const curve::StraightSkeleton& s, double area)
{
    INFO(s.error);
    CHECK(s.ok());
    double sum = 0.0;
    for (const auto& f : s.faces)
    {
        REQUIRE(f.size() >= 3);
        double a = faceArea(s, f);
        CHECK(a > -1e-9);
        sum += a;
    }
    CHECK(sum == doctest::Approx(area).epsilon(1e-6));
}

double splineArea(const curve::Spline& s, const curve::PlaneFrame& f)
{
    curve::TessOptions o;
    o.chordError = 0.0005;
    curve::Polyline pl = curve::tessellate(s, o);
    std::vector<D2> p;
    for (const D3& q : pl.points)
        p.push_back(f.to2(q));
    return polyArea(p);
}
} // namespace

TEST_CASE("straight skeleton: square, rectangle and regular polygon")
{
    std::vector<D2> sq = {D2(0, 0), D2(10, 0), D2(10, 10), D2(0, 10)};
    curve::StraightSkeleton s = curve::straightSkeleton({sq});
    checkSkeleton(s, 100.0);
    CHECK(s.endTime == doctest::Approx(5.0));

    std::vector<D2> r = {D2(0, 0), D2(20, 0), D2(20, 6), D2(0, 6)};
    s = curve::straightSkeleton({r});
    checkSkeleton(s, 120.0);
    CHECK(s.endTime == doctest::Approx(3.0));
    // The ridge runs from (3, 3) to (17, 3).
    std::vector<std::vector<D2>> centre = curve::skeletonCenterLines(s);
    REQUIRE(centre.size() == 1);
    CHECK(glm::length(centre[0].front() - centre[0].back()) == doctest::Approx(14.0));

    std::vector<D2> hex;
    for (int k = 0; k < 6; ++k)
        hex.push_back(D2(std::cos(k * glm::pi<double>() / 3.0), std::sin(k * glm::pi<double>() / 3.0)) * 5.0);
    s = curve::straightSkeleton({hex});
    checkSkeleton(s, polyArea(hex));
    CHECK(s.endTime == doctest::Approx(5.0 * std::cos(glm::pi<double>() / 6.0)));
}

TEST_CASE("straight skeleton: T, L, U shapes and a hole")
{
    // T: equal widths give simultaneous events where the stem meets the bar.
    std::vector<D2> t = {D2(-2, 0), D2(2, 0), D2(2, 10), D2(8, 10), D2(8, 14), D2(-8, 14), D2(-8, 10), D2(-2, 10)};
    checkSkeleton(curve::straightSkeleton({t}), polyArea(t));
    std::vector<D2> l = {D2(0, 0), D2(10, 0), D2(10, 3), D2(3, 3), D2(3, 10), D2(0, 10)};
    checkSkeleton(curve::straightSkeleton({l}), polyArea(l));
    std::vector<D2> u = {D2(0, 0), D2(9, 0), D2(9, 9), D2(6, 9), D2(6, 3), D2(3, 3), D2(3, 9), D2(0, 9)};
    checkSkeleton(curve::straightSkeleton({u}), polyArea(u));
    // A plus: four reflex corners all splitting at once.
    std::vector<D2> plus = {D2(-1, -3), D2(1, -3), D2(1, -1), D2(3, -1), D2(3, 1), D2(1, 1), D2(1, 3), D2(-1, 3), D2(-1, 1), D2(-3, 1), D2(-3, -1), D2(-1, -1)};
    checkSkeleton(curve::straightSkeleton({plus}), polyArea(plus));

    std::vector<D2> outer = {D2(0, 0), D2(10, 0), D2(10, 10), D2(0, 10)};
    std::vector<D2> hole = {D2(3, 3), D2(3, 7), D2(7, 7), D2(7, 3)};
    curve::StraightSkeleton s = curve::straightSkeleton({outer, hole});
    checkSkeleton(s, 100.0 - 16.0);
    CHECK(s.endTime == doctest::Approx(1.5));
}

TEST_CASE("straight skeleton: stopped early gives the inset front")
{
    std::vector<D2> l = {D2(0, 0), D2(10, 0), D2(10, 3), D2(3, 3), D2(3, 10), D2(0, 10)};
    curve::SkeletonOptions o;
    o.maxTime = 0.5;
    curve::StraightSkeleton s = curve::straightSkeleton({l}, o);
    CHECK(s.ok());
    REQUIRE(s.front.size() == 1);
    std::vector<D2> front;
    for (int n : s.front[0])
        front.push_back(s.nodes[size_t(n)]);
    // The L inset by 0.5: arms 9 x 2 and 2 x 6.5 overlapping in 2 x 2.
    CHECK(polyArea(front) == doctest::Approx(9.0 * 2.0 + 2.0 * 9.0 - 4.0));
    double sum = 0.0;
    for (const auto& f : s.faces)
        sum += faceArea(s, f);
    CHECK(sum == doctest::Approx(polyArea(l) - polyArea(front)));

    // Inset past the narrow neck splits the region in two.
    std::vector<D2> dumbbell = {D2(0, 0), D2(6, 0), D2(6, 2), D2(10, 2), D2(10, 0), D2(16, 0), D2(16, 6), D2(10, 6), D2(10, 4), D2(6, 4), D2(6, 6), D2(0, 6)};
    o.maxTime = 1.5;
    s = curve::straightSkeleton({dumbbell}, o);
    CHECK(s.ok());
    CHECK(s.front.size() == 2);
}

TEST_CASE("straight skeleton: jittered star and circle-ish polygons")
{
    std::vector<D2> star;
    for (int k = 0; k < 14; ++k)
    {
        double a = k * glm::two_pi<double>() / 14.0;
        double r = (k % 2 ? 3.0 : 7.0) + 0.37 * std::sin(k * 1.7);
        star.push_back(D2(std::cos(a), std::sin(a)) * r);
    }
    checkSkeleton(curve::straightSkeleton({star}), polyArea(star));
    std::vector<D2> circ;
    for (int k = 0; k < 64; ++k)
    {
        double a = k * glm::two_pi<double>() / 64.0;
        circ.push_back(D2(std::cos(a), std::sin(a)) * 4.0);
    }
    checkSkeleton(curve::straightSkeleton({circ}), polyArea(circ));
}

TEST_CASE("curve boolean: union, subtract, intersect, xor of rectangles")
{
    curve::Spline a = rect(0, 0, 10, 10), b = rect(5, 5, 15, 15);
    curve::CurveBooleanOptions o;
    curve::CurveBooleanResult r = curve::curveBoolean({{a}, {b}}, o);
    REQUIRE(r.curves.size() == 1);
    CHECK(r.curves[0].kind == curve::Kind::Poly);
    CHECK(std::abs(splineArea(r.curves[0], r.plane)) == doctest::Approx(175.0));
    CHECK(r.curves[0].size() == 8);

    o.op = curve::BooleanOp::Subtract;
    r = curve::curveBoolean({{a}, {b}}, o);
    REQUIRE(r.curves.size() == 1);
    CHECK(std::abs(splineArea(r.curves[0], r.plane)) == doctest::Approx(75.0));

    o.op = curve::BooleanOp::Intersect;
    r = curve::curveBoolean({{a}, {b}}, o);
    REQUIRE(r.curves.size() == 1);
    CHECK(std::abs(splineArea(r.curves[0], r.plane)) == doctest::Approx(25.0));

    o.op = curve::BooleanOp::Xor;
    r = curve::curveBoolean({{a}, {b}}, o);
    double total = 0.0;
    for (const curve::Spline& s : r.curves)
        total += splineArea(s, r.plane);
    CHECK(std::abs(total) == doctest::Approx(150.0));

    // A tool inside the target makes a hole: one outer loop and one clockwise loop.
    o.op = curve::BooleanOp::Subtract;
    r = curve::curveBoolean({{a}, {rect(3, 3, 6, 6)}}, o);
    REQUIRE(r.curves.size() == 2);
    double signedSum = splineArea(r.curves[0], r.plane) + splineArea(r.curves[1], r.plane);
    CHECK(std::abs(signedSum) == doctest::Approx(91.0));
}

TEST_CASE("curve boolean: circles stay exact")
{
    curve::Spline c1 = curve::circle(D3(0), D3(0, 1, 0), 5.0), c2 = curve::circle(D3(6, 0, 0), D3(0, 1, 0), 5.0);
    curve::CurveBooleanOptions o;
    curve::CurveBooleanResult r = curve::curveBoolean({{c1}, {c2}}, o);
    REQUIRE(r.curves.size() == 1);
    CHECK(r.curves[0].kind == curve::Kind::Nurbs);
    // Two circles of radius 5, centres 6 apart: lens area 2 r^2 acos(d / 2r) - (d / 2) sqrt(4 r^2 - d^2).
    double lens = 2 * 25 * std::acos(0.6) - 3.0 * std::sqrt(100.0 - 36.0);
    CHECK(std::abs(splineArea(r.curves[0], r.plane)) == doctest::Approx(2 * glm::pi<double>() * 25 - lens).epsilon(1e-4));
    // Every point of the result lies on one of the circles.
    for (int k = 0; k <= 200; ++k)
    {
        D3 p = curve::pointAt(r.curves[0], curve::spanCount(r.curves[0]) * k / 200.0);
        double d = std::min(std::abs(glm::length(p) - 5.0), std::abs(glm::length(p - D3(6, 0, 0)) - 5.0));
        CHECK(d < 1e-6);
    }
    o.op = curve::BooleanOp::Intersect;
    r = curve::curveBoolean({{c1}, {c2}}, o);
    REQUIRE(r.curves.size() == 1);
    CHECK(std::abs(splineArea(r.curves[0], r.plane)) == doctest::Approx(lens).epsilon(1e-4));
}

TEST_CASE("region offset grows, shrinks and merges")
{
    curve::RegionOffsetOptions o;
    o.distance = 1.0;
    o.join = curve::JoinType::Miter;
    curve::CurveBooleanResult r = curve::regionOffset({rect(0, 0, 10, 10)}, o);
    REQUIRE(r.curves.size() == 1);
    CHECK(r.curves[0].kind == curve::Kind::Poly);
    CHECK(std::abs(splineArea(r.curves[0], r.plane)) == doctest::Approx(144.0));
    // Two squares 1 apart merge when grown by 0.6.
    o.distance = 0.6;
    r = curve::regionOffset({rect(0, 0, 4, 4), rect(5, 0, 9, 4)}, o);
    CHECK(r.curves.size() == 1);
    o.distance = -2.5;
    r = curve::regionOffset({rect(0, 0, 4, 4)}, o);
    CHECK(r.curves.empty());
    o.distance = -1.0;
    o.join = curve::JoinType::Round;
    r = curve::regionOffset({curve::circle(D3(0), D3(0, 1, 0), 5.0)}, o);
    REQUIRE(r.curves.size() == 1);
    CHECK(std::abs(splineArea(r.curves[0], r.plane)) == doctest::Approx(glm::pi<double>() * 16.0).epsilon(1e-3));
}

TEST_CASE("straight skeleton: random stars and rectilinear unions")
{
    uint32_t seed = 12345;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return double(seed >> 8) / double(1u << 24);
    };
    for (int trial = 0; trial < 40; ++trial)
    {
        std::vector<D2> star;
        int n = 5 + int(rnd() * 60);
        for (int k = 0; k < n; ++k)
        {
            double a = k * glm::two_pi<double>() / n;
            star.push_back(D2(std::cos(a), std::sin(a)) * (2.0 + rnd() * 8.0));
        }
        INFO("star trial " << trial);
        checkSkeleton(curve::straightSkeleton({star}), polyArea(star));
    }
    int trials = 40;
    for (int trial = 0; trial < trials; ++trial)
    {
        // Unions of grid cells: rectilinear outlines full of equal widths and aligned edges.
        curve::Splines cells;
        int cellCount = 4 + trial % 20;
        for (int k = 0; k < cellCount; ++k)
        {
            double x = std::floor(rnd() * 8.0), z = std::floor(rnd() * 8.0);
            double w = 1.0 + std::floor(rnd() * 3.0), h = 1.0 + std::floor(rnd() * 3.0);
            cells.push_back(rect(x, z, x + w, z + h));
        }
        std::vector<curve::Splines> ops;
        for (const curve::Spline& c : cells)
            ops.push_back({c});
        curve::CurveBooleanResult u = curve::curveBoolean(ops, {});
        curve::TessOptions to;
        curve::Loops2 loops = curve::loopsOf(u.curves, u.plane, to);
        for (const curve::Region2& reg : curve::nestLoops(loops))
        {
            std::vector<std::vector<D2>> poly = {curve::cleanLoop(reg.outer.points, 1e-9)};
            double area = polyArea(poly[0]);
            for (const curve::Loop2& h : reg.holes)
            {
                poly.push_back(curve::cleanLoop(h.points, 1e-9));
                area += polyArea(poly.back());
            }
            INFO("grid trial " << trial);
            curve::StraightSkeleton sk = curve::straightSkeleton(poly);
            checkSkeleton(sk, area);
        }
    }
}

TEST_CASE("straight skeleton: a T with points along its edges closes at the stop")
{
    std::vector<D2> p = {D2(-1, 0), D2(1, 0), D2(1, 8), D2(5, 8), D2(5, 10), D2(5.0 - 10.0 / 3.0, 10), D2(5.0 - 20.0 / 3.0, 10), D2(-5, 10), D2(-5, 8), D2(-1, 8)};
    curve::StraightSkeleton s = curve::straightSkeleton({p});
    checkSkeleton(s, polyArea(p));
    curve::SkeletonOptions o;
    o.maxTime = 1.0;
    s = curve::straightSkeleton({p}, o);
    checkSkeleton(s, polyArea(p));
    CHECK(s.front.empty());
}

TEST_CASE("straight skeleton: unions of rotated rectangles and circles")
{
    uint32_t seed = 777;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return double(seed >> 8) / double(1u << 24);
    };
    int trials = 30;
    for (int trial = 0; trial < trials; ++trial)
    {
        std::vector<curve::Splines> ops;
        for (int k = 0; k < 6; ++k)
        {
            D3 c(rnd() * 10.0, 0.0, rnd() * 10.0);
            if (rnd() < 0.4)
            {
                ops.push_back({curve::circle(c, D3(0, 1, 0), 1.0 + rnd() * 3.0)});
                continue;
            }
            double a = rnd() * 3.0, w = 1.0 + rnd() * 4.0, h = 0.5 + rnd() * 3.0;
            D3 u(std::cos(a), 0, std::sin(a)), v(-std::sin(a), 0, std::cos(a));
            ops.push_back({polyLoop({c - u * w - v * h, c + u * w - v * h, c + u * w + v * h, c - u * w + v * h})});
        }
        curve::CurveBooleanResult un = curve::curveBoolean(ops, {});
        curve::TessOptions to;
        to.chordError = 0.02;
        curve::Loops2 loops = curve::loopsOf(un.curves, un.plane, to);
        for (const curve::Region2& reg : curve::nestLoops(loops))
        {
            std::vector<std::vector<D2>> poly = {curve::cleanLoop(reg.outer.points, 1e-7)};
            double area = polyArea(poly[0]);
            for (const curve::Loop2& h : reg.holes)
            {
                poly.push_back(curve::cleanLoop(h.points, 1e-7));
                area += polyArea(poly.back());
            }
            INFO("trial " << trial);
            curve::StraightSkeleton sk = curve::straightSkeleton(poly);
            checkSkeleton(sk, area);
        }
    }
}

TEST_CASE("curve boolean: curves on a tilted plane, and a warning when they are not planar")
{
    glm::dmat4 tilt = glm::rotate(glm::dmat4(1.0), 0.7, D3(1, 0.3, 0.2));
    tilt = glm::translate(tilt, D3(3, -2, 5));
    curve::Spline a = rect(0, 0, 10, 10), b = rect(5, 5, 15, 15);
    curve::transform(a, tilt);
    curve::transform(b, tilt);
    curve::CurveBooleanResult r = curve::curveBoolean({{a}, {b}}, {});
    CHECK(r.warnings.empty());
    REQUIRE(r.curves.size() == 1);
    D3 n = glm::normalize(D3(tilt * glm::dvec4(0, 1, 0, 0)));
    D3 o = D3(tilt * glm::dvec4(0, 0, 0, 1));
    for (const D3& p : r.curves[0].points)
        CHECK(std::abs(glm::dot(p - o, n)) < 1e-9);
    CHECK(std::abs(splineArea(r.curves[0], r.plane)) == doctest::Approx(175.0));

    curve::Spline lifted = rect(5, 5, 15, 15);
    lifted.points[2].y = 1.0;
    r = curve::curveBoolean({{rect(0, 0, 10, 10)}, {lifted}}, {});
    CHECK(!r.warnings.empty());
}
