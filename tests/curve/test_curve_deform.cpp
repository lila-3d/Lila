#include <doctest/doctest.h>

#include "curve/CurveDeform.h"
#include "curve/Fit.h"
#include "curve/Tessellate.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace lila;
using namespace lila::curve;
using D3 = glm::dvec3;

namespace
{
Spline polyLine(int points, double length)
{
    Spline s;
    s.kind = Kind::Poly;
    for (int i = 0; i < points; ++i)
        s.points.push_back(D3(length * i / (points - 1), 0.0, 0.0));
    s.normalize();
    return s;
}

Spline bezier(std::vector<D3> pts)
{
    Spline s;
    s.kind = Kind::Bezier;
    s.points = std::move(pts);
    s.handleL.assign(s.size(), D3(0.0));
    s.handleR.assign(s.size(), D3(0.0));
    s.typeL.assign(s.size(), HandleType::Auto);
    s.typeR = s.typeL;
    updateHandles(s);
    s.normalize();
    return s;
}
} // namespace

TEST_CASE("bend curve")
{
    Spline s = polyLine(21, 100.0);
    BendCurveOptions o;
    o.angle = glm::half_pi<double>();
    o.axis = D3(0, 1, 0);
    Spline b = bendCurve(s, o);
    REQUIRE(b.size() == s.size());
    // A quarter turn of a 100 cm line: radius 200 / pi, ending a quarter of the way round.
    double R = 200.0 / glm::pi<double>();
    CHECK(b.points.front() == s.points.front());
    for (const D3& p : b.points)
        CHECK(glm::length(p - D3(0, 0, -R)) == doctest::Approx(R));
    CHECK(length(b) == doctest::Approx(100.0).epsilon(0.01));

    // From the centre, both halves bend and the middle stays.
    o.fromCenter = true;
    Spline c = bendCurve(s, o);
    CHECK(glm::distance(c.points[10], s.points[10]) < 1e-9);
    CHECK(c.points.front().z == doctest::Approx(c.points.back().z));

    // Masked: only the selected points move.
    std::vector<uint8_t> mask(s.size(), 0);
    mask[20] = 1;
    o.fromCenter = false;
    Spline m = bendCurve(s, o, mask);
    CHECK(m.points[19] == s.points[19]);
    CHECK(m.points[20] != s.points[20]);
    CHECK(bendCurve(s, {.angle = 0.0}).points == s.points);
}

TEST_CASE("curl curve")
{
    Spline s = polyLine(2, 100.0);
    CurlCurveOptions o;
    o.amplitude = 3.0;
    o.turns = 5.0;
    Spline c = curlCurve(s, o);
    CHECK(c.size() >= size_t(5 * o.samplesPerTurn));
    CHECK(glm::distance(c.points.front(), s.points.front()) < 1e-9);
    CHECK(glm::distance(c.points.back(), s.points.back()) < 1e-9);
    double far = 0.0;
    for (const D3& p : c.points)
        far = std::max(far, glm::length(D3(0.0, p.y, p.z)));
    CHECK(far == doctest::Approx(3.0).epsilon(0.02));

    Spline n = curlCurve(bezier({D3(0), D3(30, 10, 0), D3(60, 0, 0), D3(90, 10, 0)}), o);
    CHECK(n.kind == Kind::Bezier);
    CHECK(isValid(n));
}

TEST_CASE("scale curvature flattens and exaggerates")
{
    Spline arcS = arc(D3(0, 0, 50), D3(0, -1, 0), D3(0.0), glm::half_pi<double>());
    Spline poly = convert(arcS, Kind::Poly);
    double L = length(poly);
    Spline flat = scaleCurvature(poly, 0.0);
    // Straightened along the first segment, same length.
    D3 dir = glm::normalize(flat.points[1] - flat.points[0]);
    for (const D3& p : flat.points)
        CHECK(glm::length(glm::cross(p - flat.points[0], dir)) < 1e-6);
    CHECK(glm::distance(flat.points.back(), flat.points.front()) == doctest::Approx(L).epsilon(1e-6));
    Spline same = scaleCurvature(poly, 1.0);
    CHECK(same.points == poly.points);
    // Doubling a quarter turn makes a half turn.
    Spline twice = scaleCurvature(poly, 2.0);
    D3 endDir = glm::normalize(twice.points.back() - twice.points[twice.size() - 2]);
    CHECK(endDir.x == doctest::Approx(-1.0).epsilon(0.05));

    // NURBS control points follow the frame.
    Spline flatArc = scaleCurvature(arcS, 0.0);
    CHECK(isValid(flatArc));
    CHECK(std::abs(pointAt(flatArc, spanCount(flatArc)).z) < 2.0);
}

TEST_CASE("smooth curve")
{
    Spline zig;
    zig.kind = Kind::Poly;
    for (int i = 0; i < 9; ++i)
        zig.points.push_back(D3(10.0 * i, i % 2 ? 5.0 : -5.0, 0.0));
    zig.normalize();
    SmoothCurveOptions o;
    o.iterations = 4;
    Spline sm = smoothCurve(zig, o);
    CHECK(sm.points.front() == zig.points.front());
    CHECK(sm.points.back() == zig.points.back());
    CHECK(std::abs(sm.points[4].y) < 2.0);
    o.keepEnds = false;
    CHECK(smoothCurve(zig, o).points.front() != zig.points.front());
    std::vector<uint8_t> mask(zig.size(), 0);
    mask[3] = 1;
    Spline one = smoothCurve(zig, {}, mask);
    CHECK(one.points[2] == zig.points[2]);
    CHECK(one.points[3] != zig.points[3]);
}

TEST_CASE("even spacing keeps kind, count and shape")
{
    // Anchors bunched at one end of a smooth Bezier.
    Spline b = bezier({D3(0), D3(2, 3, 0), D3(5, 5, 0), D3(50, 20, 0), D3(100, 0, 0)});
    Spline e = evenSpacing(b);
    REQUIRE(e.size() == b.size());
    CHECK(e.kind == Kind::Bezier);
    std::vector<double> gaps;
    for (size_t i = 0; i + 1 < e.size(); ++i)
        gaps.push_back(glm::distance(e.points[i], e.points[i + 1]));
    double lo = *std::min_element(gaps.begin(), gaps.end()), hi = *std::max_element(gaps.begin(), gaps.end());
    CHECK(hi / lo < 1.3);
    // Five evenly spread anchors cannot hold the tight start exactly; under 1% of the length off.
    CHECK(shapeDeviation(e, b) < 1.0);
    CHECK(shapeDeviation(b, e) < 1.0);

    // A poly keeps its corners; the rest spread evenly.
    Spline p;
    p.kind = Kind::Poly;
    p.points = {D3(0), D3(1, 0, 0), D3(2, 0, 0), D3(10, 0, 0), D3(10, 10, 0)};
    p.normalize();
    Spline pe = evenSpacing(p);
    REQUIRE(pe.size() == 5);
    CHECK(pe.points[2] == D3(10, 0, 0));
    CHECK(pe.points[1].x == doctest::Approx(5.0));
    CHECK(pe.points[3].y == doctest::Approx(5.0));
    CHECK(shapeDeviation(pe, p) < 1e-6);

    // NURBS refit with the same number of control points.
    Spline n = interpolate(std::vector<D3>{D3(0), D3(5, 2, 0), D3(8, 3, 0), D3(60, 0, 0), D3(100, 10, 0)}, 3, false);
    Spline ne = evenSpacing(n);
    CHECK(ne.size() == n.size());
    CHECK(shapeDeviation(ne, n) < 2.0);

    // Rebuild's keepPointCount option is even spacing.
    RebuildOptions ro;
    ro.keepPointCount = true;
    Spline rb = rebuild(b, ro);
    CHECK(rb.size() == b.size());
    CHECK(rb.kind == Kind::Bezier);
}
