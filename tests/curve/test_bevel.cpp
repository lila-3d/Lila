#include <doctest/doctest.h>

#include "curve/BevelPlus.h"
#include "curve/CapFill.h"
#include "curve/Planar.h"
#include "curve/Text.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace lila;
using D2 = glm::dvec2;
using D3 = glm::dvec3;

namespace
{
curve::Spline polyLoop(std::vector<D3> pts)
{
    curve::Spline s;
    s.kind = curve::Kind::Poly;
    s.closed = true;
    s.points = std::move(pts);
    s.normalize();
    return s;
}

/** A closed, manifold, consistently wound mesh with no degenerate faces. */
void checkSolid(const geo::Mesh& m)
{
    std::vector<std::string> problems = m.validate();
    INFO((problems.empty() ? std::string() : problems.front()));
    CHECK(problems.empty());
    int open = 0, nonManifold = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            open += m.isBoundaryEdge(e) ? 1 : 0;
            nonManifold += m.edgeFaceCount(e) > 2 ? 1 : 0;
        }
    CHECK(open == 0);
    CHECK(nonManifold == 0);
    int degenerate = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceArea(f) < 1e-10)
            ++degenerate;
    CHECK(degenerate == 0);
}

/** Signed volume by the divergence theorem: positive when faces wind outward. */
double volume(const geo::Mesh& m)
{
    double v = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<geo::Index> p = m.facePoints(f);
        for (size_t i = 1; i + 1 < p.size(); ++i)
            v += glm::dot(m.position(p[0]), glm::cross(m.position(p[i]), m.position(p[i + 1]))) / 6.0;
    }
    return v;
}

int countFaces(const geo::Mesh& m, size_t n)
{
    int c = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.facePoints(f).size() == n)
            ++c;
    return c;
}

int partFaces(const geo::Mesh& m, int part)
{
    auto p = m.faceAttrs().get<int32_t>("part");
    int c = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && f < p.size() && p[f] == part)
            ++c;
    return c;
}
} // namespace

TEST_CASE("cap fill: square, ring and L are quad dominant and cover the area")
{
    auto area = [](const curve::CapFillResult& r) {
        double a = 0.0;
        for (const auto& f : r.faces)
        {
            std::vector<D2> p;
            for (int v : f)
                p.push_back(r.points[size_t(v)]);
            double fa = 0.0;
            for (size_t i = 0; i < p.size(); ++i)
                fa += p[i].x * p[(i + 1) % p.size()].y - p[(i + 1) % p.size()].x * p[i].y;
            CHECK(fa > 0.0);
            a += fa * 0.5;
        }
        return a;
    };
    std::vector<D2> sq;
    for (int i = 0; i < 10; ++i)
        sq.push_back(D2(i, 0));
    for (int i = 0; i < 10; ++i)
        sq.push_back(D2(10, i));
    for (int i = 10; i > 0; --i)
        sq.push_back(D2(i, 10));
    for (int i = 10; i > 0; --i)
        sq.push_back(D2(0, i));
    curve::CapFillResult r = curve::capFill({sq});
    REQUIRE(r.ok());
    CHECK(area(r) == doctest::Approx(100.0));
    int quads = 0;
    for (const auto& f : r.faces)
        quads += f.size() == 4 ? 1 : 0;
    CHECK(quads >= int(r.faces.size()) * 9 / 10);

    std::vector<D2> outer, inner;
    for (int k = 0; k < 48; ++k)
    {
        double a = k * glm::two_pi<double>() / 48.0;
        outer.push_back(D2(std::cos(a), std::sin(a)) * 10.0);
        inner.push_back(D2(std::cos(-a), std::sin(-a)) * 5.0);
    }
    r = curve::capFill({outer, inner});
    REQUIRE(r.ok());
    double expect = 0.5 * 48 * std::sin(glm::two_pi<double>() / 48.0) * (100.0 - 25.0);
    CHECK(area(r) == doctest::Approx(expect));
    quads = 0;
    for (const auto& f : r.faces)
        quads += f.size() == 4 ? 1 : 0;
    CHECK(quads * 3 >= int(r.faces.size()) * 2);
}

TEST_CASE("bevel plus: a square makes a closed solid with quad caps")
{
    curve::BevelPlusOptions o;
    o.extrudeDepth = 2.0;
    o.front.width = o.front.depth = 0.5;
    o.back.width = o.back.depth = 0.5;
    curve::BevelPlusResult r = curve::bevelPlus({polyLoop({D3(0, 0, 0), D3(10, 0, 0), D3(10, 0, -10), D3(0, 0, -10)})}, o);
    INFO((r.warnings.empty() ? std::string() : r.warnings.front()));
    CHECK(r.warnings.empty());
    checkSolid(r.mesh);
    CHECK(countFaces(r.mesh, 3) == 0);
    // Sides 4, bevels 4 + 4, caps one quad each.
    CHECK(r.mesh.numFaces() == 14);
    // A 10 x 10 x 2 box plus two frustums from 10 x 10 to 9 x 9, 0.5 high.
    double frustum = 0.5 / 3.0 * (100.0 + 81.0 + std::sqrt(100.0 * 81.0));
    CHECK(volume(r.mesh) == doctest::Approx(200.0 + 2 * frustum));
    // On the ground plane it extrudes up.
    CHECK(r.mesh.bounds().max.y == doctest::Approx(2.5));
    CHECK(r.mesh.bounds().min.y == doctest::Approx(-0.5));
    CHECK(r.mesh.cornerAttrs().has(geo::attr::uvName(0)));
    CHECK(partFaces(r.mesh, 0) == 1);
    CHECK(partFaces(r.mesh, 2) == 4);
}

TEST_CASE("bevel plus: holes, profiles and bevels wider than the shape")
{
    curve::Spline outer = curve::circle(D3(0), D3(0, 0, 1), 10.0), inner = curve::circle(D3(0), D3(0, 0, 1), 6.0);
    for (curve::BevelProfile prof : {curve::BevelProfile::Straight, curve::BevelProfile::Convex, curve::BevelProfile::Concave, curve::BevelProfile::Step})
    {
        curve::BevelPlusOptions o;
        o.front.profile = o.back.profile = prof;
        o.front.segments = o.back.segments = 3;
        o.front.width = 0.8;
        o.front.depth = 0.6;
        o.back.width = 0.4;
        o.back.depth = 0.4;
        curve::BevelPlusResult r = curve::bevelPlus({outer, inner}, o);
        INFO(curve::bevelProfileName(prof));
        INFO((r.warnings.empty() ? std::string() : r.warnings.front()));
        CHECK(r.warnings.empty());
        checkSolid(r.mesh);
        CHECK(volume(r.mesh) > 0.0);
        // Ring caps: mostly quads.
        CHECK(countFaces(r.mesh, 4) > countFaces(r.mesh, 3) * 3);
    }
    // A 2 cm wide L with a 3 cm bevel: the bevel meets itself in a ridge and the caps vanish, still closed.
    curve::BevelPlusOptions o;
    o.front.width = o.back.width = 3.0;
    o.front.depth = o.back.depth = 1.0;
    curve::BevelPlusResult r =
        curve::bevelPlus({polyLoop({D3(0, 0, 0), D3(10, 0, 0), D3(10, 2, 0), D3(2, 2, 0), D3(2, 10, 0), D3(0, 10, 0)})}, o);
    checkSolid(r.mesh);
    CHECK(partFaces(r.mesh, 0) == 0);
    CHECK(r.mesh.bounds().max.z == doctest::Approx(2.0 + 1.0 / 3.0));
}

TEST_CASE("bevel plus: degenerate input warns")
{
    curve::BevelPlusOptions o;
    curve::Spline open;
    open.kind = curve::Kind::Poly;
    open.points = {D3(0), D3(1, 0, 0), D3(1, 1, 0)};
    open.normalize();
    curve::BevelPlusResult r = curve::bevelPlus({open}, o);
    CHECK(r.mesh.numFaces() == 0);
    CHECK(!r.warnings.empty());
    curve::Spline flat = polyLoop({D3(0), D3(1, 0, 0), D3(2, 0, 0)});
    r = curve::bevelPlus({flat}, o);
    CHECK(r.mesh.numFaces() == 0);
    CHECK(!r.warnings.empty());
}

TEST_CASE("bevel plus: sharp corners mitre without crossing")
{
    std::vector<D3> star;
    for (int k = 0; k < 10; ++k)
    {
        double a = k * glm::two_pi<double>() / 10.0 + glm::half_pi<double>();
        star.push_back(D3(std::cos(a), std::sin(a), 0.0) * (k % 2 ? 4.0 : 10.0));
    }
    curve::BevelPlusOptions o;
    o.front.width = 1.5;
    o.front.depth = 1.0;
    o.front.segments = 3;
    o.front.profile = curve::BevelProfile::Convex;
    o.back.width = 0.0;
    curve::BevelPlusResult r = curve::bevelPlus({polyLoop(star)}, o);
    CHECK(r.warnings.empty());
    checkSolid(r.mesh);
    CHECK(partFaces(r.mesh, 3) == 0);
    // A T whose stem is as wide as the bevel is deep: the stem's bevel closes in a ridge.
    curve::BevelPlusOptions t;
    t.front.width = t.back.width = 1.0;
    curve::BevelPlusResult rt = curve::bevelPlus(
        {polyLoop({D3(-1, 0, 0), D3(1, 0, 0), D3(1, 8, 0), D3(5, 8, 0), D3(5, 10, 0), D3(-5, 10, 0), D3(-5, 8, 0), D3(-1, 8, 0)})}, t);
    CHECK(rt.warnings.empty());
    checkSolid(rt.mesh);
}

TEST_CASE("cap fill: star")
{
    std::vector<D2> star;
    for (int k = 0; k < 10; ++k)
    {
        double a = k * glm::two_pi<double>() / 10.0 + glm::half_pi<double>();
        star.push_back(D2(std::cos(a), std::sin(a)) * (k % 2 ? 4.0 : 10.0));
    }
    curve::CapFillResult r = curve::capFill({star});
    REQUIRE(r.ok());
    double a = 0.0;
    for (const auto& f : r.faces)
    {
        double fa = 0.0;
        for (size_t i = 0; i < f.size(); ++i)
        {
            D2 p = r.points[size_t(f[i])], q = r.points[size_t(f[(i + 1) % f.size()])];
            fa += p.x * q.y - q.x * p.y;
        }
        CHECK(fa > 0.0);
        a += fa * 0.5;
    }
    CHECK(a == doctest::Approx(curve::signedArea(star)));
}

TEST_CASE("bevel plus: text with counters and curves stays closed")
{
    curve::TextOptions t;
    t.text = "Lila&8gB";
    t.size = 10.0;
    curve::TextResult text = curve::textCurves(t);
    REQUIRE(!text.curves.empty());
    curve::BevelPlusOptions o;
    o.extrudeDepth = 1.5;
    o.front.width = o.back.width = 0.35;
    o.front.depth = o.back.depth = 0.35;
    o.front.profile = curve::BevelProfile::Convex;
    o.front.segments = 3;
    curve::BevelPlusResult r = curve::bevelPlus(text.curves, o);
    INFO((r.warnings.empty() ? std::string() : r.warnings.front()));
    CHECK(r.warnings.empty());
    checkSolid(r.mesh);
    CHECK(countFaces(r.mesh, 4) > countFaces(r.mesh, 3) * 3);
}

TEST_CASE("bevel plus: many segments and no depth stay closed")
{
    curve::TextOptions t;
    t.text = "Lila Bevel";
    t.size = 10.0;
    curve::TextResult text = curve::textCurves(t);
    REQUIRE(!text.curves.empty());
    for (double depth : {2.0, 0.0})
    {
        curve::BevelPlusOptions o;
        o.extrudeDepth = depth;
        o.front.profile = curve::BevelProfile::Convex;
        o.front.segments = 16;
        curve::BevelPlusResult r = curve::bevelPlus(text.curves, o);
        INFO("depth " << depth);
        CHECK(r.warnings.empty());
        checkSolid(r.mesh);
    }
}
