#include <doctest/doctest.h>

#include "curve/Along.h"
#include "curve/Spline.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <stdexcept>

using namespace lila;
using namespace lila::curve;
using D3 = glm::dvec3;
using D2 = glm::dvec2;

namespace
{
Spline line(D3 a, D3 b)
{
    Spline s;
    s.kind = Kind::Poly;
    s.points = {a, b};
    s.normalize();
    return s;
}

/** A quarter circle of radius r in the XZ plane, from (0, 0, 0) toward +X, turning toward +Z. */
Spline quarter(double r) { return arc(D3(0, 0, r), D3(0, -1, 0), D3(0.0), glm::half_pi<double>()); }

/** A beam along +X from 0 to length, cut into pieces so it can bend. */
geo::Mesh beam(double length, int pieces, double size = 2.0)
{
    geo::Mesh m = geo::makeCube({.width = length, .height = size, .depth = size, .subdivX = pieces});
    for (D3& p : m.positions())
        p.x += length * 0.5;
    return m;
}

/** The beam without its end caps: an open tube whose end rings weld onto the next copy. */
geo::Mesh tube(double length, int pieces, double size = 2.0)
{
    geo::Mesh m = beam(length, pieces, size);
    std::vector<geo::Index> caps;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && std::abs(m.faceNormal(f).x) > 0.9)
            caps.push_back(f);
    geo::deleteFaces(m, caps);
    m.compact();
    return m;
}

double maxError(const geo::Mesh& m, const std::function<double(const D3&)>& err)
{
    double worst = 0.0;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            worst = std::max(worst, err(m.position(p)));
    return worst;
}
} // namespace

TEST_CASE("rail stations, wrap and extrapolation")
{
    Rail r(line(D3(0), D3(100, 0, 0)));
    REQUIRE(r.valid());
    CHECK(r.length() == doctest::Approx(100.0));
    Rail::Station mid = r.at(30.0);
    CHECK(mid.p.x == doctest::Approx(30.0));
    CHECK(mid.f.t.x == doctest::Approx(1.0));
    // Up stays where the up vector lies across the curve.
    CHECK(mid.f.n.y == doctest::Approx(1.0));
    CHECK(r.at(130.0).p.x == doctest::Approx(130.0));
    CHECK(r.at(130.0, false).p.x == doctest::Approx(100.0));
    CHECK(r.at(-20.0).p.x == doctest::Approx(-20.0));

    Rail loop(circle(D3(0), D3(0, 1, 0), 10.0));
    double L = loop.length();
    CHECK(L == doctest::Approx(2 * glm::pi<double>() * 10.0).epsilon(1e-3));
    CHECK(glm::distance(loop.at(L * 1.25).p, loop.at(L * 0.25).p) < 1e-6);
    CHECK(glm::distance(loop.at(-L * 0.25).p, loop.at(L * 0.75).p) < 1e-6);

    CHECK(!Rail(line(D3(5), D3(5))).valid());
}

TEST_CASE("deform along curve keeps lengths and bends sections")
{
    const double R = 100.0, L = R * glm::half_pi<double>();
    Rail rail(quarter(R));
    REQUIRE(rail.length() == doctest::Approx(L).epsilon(1e-4));
    geo::Mesh m = beam(L, 32);
    auto uvBefore = std::vector<glm::vec2>(m.cornerAttrs().get<glm::vec2>("uv0").begin(), m.cornerAttrs().get<glm::vec2>("uv0").end());
    deformAlongCurve(m, rail, {});
    // Each point sits at its old distance from the arc's centre (radius minus its side offset), and its height is kept.
    double err = maxError(m, [&](const D3& p) { return std::abs(std::abs(p.y) - 1.0); });
    CHECK(err < 1e-6);
    double rErr = maxError(m, [&](const D3& p) {
        double d = glm::length(D3(p.x, 0.0, p.z - R));
        return std::min(std::abs(d - (R - 1.0)), std::abs(d - (R + 1.0)));
    });
    CHECK(rErr < 0.01);
    geo::Bounds b = m.bounds();
    CHECK(b.max.x == doctest::Approx(R + 1.0).epsilon(1e-3));
    CHECK(b.max.z == doctest::Approx(R).epsilon(1e-3));
    auto uvAfter = m.cornerAttrs().get<glm::vec2>("uv0");
    CHECK(std::equal(uvBefore.begin(), uvBefore.end(), uvAfter.begin()));
    CHECK(m.validate().empty());

    // Stretch fits the mesh to the whole curve; offsetFraction starts it halfway.
    geo::Mesh s = beam(10.0, 4);
    DeformAlongOptions so;
    so.stretch = true;
    deformAlongCurve(s, Rail(line(D3(0), D3(200, 0, 0))), so);
    CHECK(s.bounds().max.x == doctest::Approx(200.0));
    geo::Mesh h = beam(10.0, 4);
    DeformAlongOptions ho;
    ho.offsetFraction = 0.5;
    deformAlongCurve(h, Rail(line(D3(0), D3(200, 0, 0))), ho);
    CHECK(h.bounds().min.x == doctest::Approx(100.0));
    CHECK(h.bounds().max.x == doctest::Approx(110.0));

    // Forward -X runs the mesh the other way along the curve; envelope 0 leaves it.
    geo::Mesh n = beam(10.0, 1);
    DeformAlongOptions no;
    no.forward = AlongAxis::NegX;
    no.offset = 50.0;
    deformAlongCurve(n, Rail(line(D3(0, 0, 0), D3(0, 0, 100))), no);
    CHECK(n.bounds().min.z == doctest::Approx(50.0));
    CHECK(n.bounds().max.z == doctest::Approx(60.0));
    geo::Mesh e = beam(10.0, 1);
    geo::Mesh e0 = e;
    DeformAlongOptions eo;
    eo.envelope = 0.0;
    deformAlongCurve(e, Rail(quarter(50.0)), eo);
    for (geo::Index p = 0; p < e.pointCapacity(); ++p)
        CHECK(glm::distance(e.position(p), e0.position(p)) < 1e-12);

    CHECK_THROWS_AS(deformAlongCurve(m, Rail(line(D3(1), D3(1))), {}), std::invalid_argument);
}

TEST_CASE("deform along curve: twist, scale, loop, ends and stored normals")
{
    Rail rail(line(D3(0), D3(100, 0, 0)));
    geo::Mesh m = beam(100.0, 10);
    DeformAlongOptions o;
    o.scale = [](double t) { return 1.0 + t; };
    o.twist = [](double t) { return glm::half_pi<double>() * t; };
    deformAlongCurve(m, rail, o);
    // At the far end the 2 x 2 section is twice the size and turned a quarter: its corners still lie at radius 2 sqrt 2.
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.position(p).x > 99.0)
            CHECK(glm::length(D3(0, m.position(p).y, m.position(p).z)) == doctest::Approx(2.0 * std::sqrt(2.0)));

    // A beam longer than a closed loop wraps round it.
    Rail loop(circle(D3(0), D3(0, 1, 0), 10.0));
    geo::Mesh w = beam(loop.length() * 1.5, 48, 1.0);
    deformAlongCurve(w, loop, {});
    CHECK(w.bounds().max.x < 11.0);
    DeformAlongOptions open;
    open.loop = false;
    geo::Mesh w2 = beam(loop.length() * 1.5, 48, 1.0);
    deformAlongCurve(w2, loop, open);
    CHECK(maxError(w2, [](const D3& p) { return glm::length(p); }) > 15.0);

    // Clamped ends pile up instead of running on.
    geo::Mesh c = beam(150.0, 3);
    DeformAlongOptions co;
    co.extrapolate = false;
    deformAlongCurve(c, rail, co);
    CHECK(c.bounds().max.x == doctest::Approx(100.0));

    // Stored normals turn with the curve.
    geo::Mesh q = geo::makePlane({.width = 10.0, .depth = 10.0});
    auto nrm = q.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
    for (glm::vec3& v : nrm)
        v = glm::vec3(0, 1, 0);
    DeformAlongOptions qo;
    qo.roll = glm::half_pi<double>();
    deformAlongCurve(q, rail, qo);
    for (glm::vec3 v : q.cornerAttrs().get<glm::vec3>(geo::attr::kNormal))
        CHECK(std::abs(v.z) == doctest::Approx(1.0f));
}

TEST_CASE("aim curve and auto normals turn the frame")
{
    Spline path = line(D3(0), D3(100, 0, 0));
    RailOptions ro;
    ro.up = UpMode::AimCurve;
    ro.aim = line(D3(0, 0, 50), D3(100, 0, 50));
    Rail aimed(path, ro);
    CHECK(aimed.at(50.0).f.n.z == doctest::Approx(1.0));
    // Auto normals keep up toward Y on a vertical climb's sides too.
    RailOptions auto_;
    auto_.up = UpMode::AutoNormals;
    Rail climb(line(D3(0), D3(100, 100, 0)), auto_);
    CHECK(climb.at(10.0).f.n.y > 0.7);
}

TEST_CASE("array placements: count, spacing, loops, offsets and pieces")
{
    Rail rail(line(D3(0), D3(100, 0, 0)));
    ArrayAlongOptions o;
    o.count = 5;
    auto p = arrayPlacements(rail, o, 10.0);
    REQUIRE(p.size() == 5);
    for (size_t k = 0; k < 5; ++k)
        CHECK(p[k].transform[3].x == doctest::Approx(25.0 * double(k)));

    o.fill = ArrayFill::Spacing;
    o.spacing = 30.0;
    p = arrayPlacements(rail, o, 10.0);
    CHECK(p.size() == 4); // round(100 / 30) = 3 steps, evened to 33.3
    CHECK(p.back().transform[3].x == doctest::Approx(100.0));
    o.fitToLength = false;
    p = arrayPlacements(rail, o, 10.0);
    CHECK(p.size() == 4);
    CHECK(p.back().transform[3].x == doctest::Approx(90.0));

    o.startOffset = 10.0;
    o.endOffset = 20.0;
    o.fitToLength = true;
    o.fill = ArrayFill::Count;
    o.count = 3;
    p = arrayPlacements(rail, o, 10.0);
    CHECK(p.front().transform[3].x == doctest::Approx(10.0));
    CHECK(p.back().transform[3].x == doctest::Approx(80.0));

    // A whole loop doesn't put a copy on top of the first.
    Rail loop(circle(D3(0), D3(0, 1, 0), 10.0));
    ArrayAlongOptions lo;
    lo.count = 4;
    p = arrayPlacements(loop, lo, 1.0);
    REQUIRE(p.size() == 4);
    CHECK(glm::distance(D3(p[0].transform[3]), D3(p[3].transform[3])) > 10.0);

    // Start and end pieces take the ends.
    ArrayAlongOptions so;
    so.count = 4;
    p = arrayPlacements(rail, so, 10.0, 5.0, 5.0);
    CHECK(p.front().piece == ArrayPiece::Start);
    CHECK(p.back().piece == ArrayPiece::End);
    CHECK(p[1].piece == ArrayPiece::Middle);

    ArrayAlongOptions bad;
    bad.startOffset = 60.0;
    bad.endOffset = 60.0;
    CHECK_THROWS_AS(arrayPlacements(rail, bad, 10.0), std::invalid_argument);
    ArrayAlongOptions many;
    many.fill = ArrayFill::Spacing;
    many.spacing = 1e-4;
    CHECK_THROWS_AS(arrayPlacements(rail, many, 10.0), std::invalid_argument);
    ArrayAlongOptions flat;
    flat.fill = ArrayFill::Spacing;
    CHECK_THROWS_AS(arrayPlacements(rail, flat, 0.0), std::invalid_argument);
}

TEST_CASE("array along curve: rigid, deformed and welded, random")
{
    const double R = 50.0;
    Rail rail(quarter(R));
    geo::Mesh plank = tube(10.0, 2, 1.0);
    ArrayAlongOptions o;
    o.fill = ArrayFill::Spacing;
    o.deform = true;
    geo::Mesh merged = arrayAlongCurve(plank, nullptr, nullptr, rail, o);
    size_t copies = size_t(std::round(rail.length() / 10.0));
    // Welded seams: each copy after the first shares its start ring (4 points) with the last one.
    CHECK(merged.numPoints() == geo::Index(copies * plank.numPoints() - (copies - 1) * 4));
    CHECK(merged.validate().empty());
    o.weld = false;
    geo::Mesh loose = arrayAlongCurve(plank, nullptr, nullptr, rail, o);
    CHECK(loose.numPoints() == geo::Index(copies * plank.numPoints()));

    // Rigid copies are the piece turned onto the frame: lengths inside each copy stay.
    ArrayAlongOptions r;
    r.count = 6;
    std::vector<ArrayPlacement> places;
    geo::Mesh rigid = arrayAlongCurve(plank, nullptr, nullptr, rail, r, &places);
    REQUIRE(places.size() == 6);
    CHECK(rigid.numFaces() == plank.numFaces() * 6);
    CHECK(glm::distance(rigid.position(0), rigid.position(1)) == doctest::Approx(glm::distance(plank.position(0), plank.position(1))));

    // The same seed gives the same jitter; another seed differs.
    ArrayAlongOptions j;
    j.count = 4;
    j.randomRotate = D3(0.5);
    j.randomScale = 0.3;
    j.randomOffset = D3(2.0);
    j.seed = 7;
    geo::Mesh a = arrayAlongCurve(plank, nullptr, nullptr, rail, j);
    geo::Mesh b = arrayAlongCurve(plank, nullptr, nullptr, rail, j);
    j.seed = 8;
    geo::Mesh c = arrayAlongCurve(plank, nullptr, nullptr, rail, j);
    CHECK(a.positions()[5] == b.positions()[5]);
    CHECK(glm::distance(a.positions()[5], c.positions()[5]) > 1e-6);

    // Distinct end pieces.
    geo::Mesh cap = geo::makeCube({.width = 2.0, .height = 4.0, .depth = 4.0});
    ArrayAlongOptions e;
    e.count = 5;
    geo::Mesh withCaps = arrayAlongCurve(plank, &cap, &cap, rail, e);
    CHECK(withCaps.numFaces() == plank.numFaces() * 3 + cap.numFaces() * 2);
}

TEST_CASE("flow along surface by UV and by grid")
{
    // A unit tile flowed onto a plane with the same footprint stays put; height rides the normal.
    geo::Mesh target = geo::makePlane({.width = 100.0, .depth = 100.0, .subdivX = 4, .subdivZ = 4});
    geo::Mesh src = geo::makeCube({.width = 10.0, .height = 10.0, .depth = 10.0, .baseAtOrigin = true});
    geo::Mesh flowed = src;
    FlowOptions o;
    o.footprint = glm::dvec4(-50.0, -50.0, 50.0, 50.0);
    flowAlongSurface(flowed, target, o);
    for (geo::Index p = 0; p < src.pointCapacity(); ++p)
        CHECK(glm::distance(flowed.position(p), src.position(p)) < 1e-6);

    // Onto a cylinder-like bend: the bottom face lands on the surface, the top 10 cm off it.
    geo::Mesh bent = geo::makePlane({.width = 100.0, .depth = 100.0, .subdivX = 32, .subdivZ = 2});
    const double R = 100.0 / glm::pi<double>();
    for (D3& p : bent.positions())
    {
        double a = (p.x + 50.0) / R;
        p = D3(-R * std::cos(a), R * std::sin(a), p.z);
    }
    geo::Mesh tile = src;
    FlowOptions bo;
    bo.footprint = glm::dvec4(-50.0, -50.0, 50.0, 50.0);
    flowAlongSurface(tile, bent, bo);
    for (geo::Index p = 0; p < src.pointCapacity(); ++p)
    {
        double r = glm::length(D3(tile.position(p).x, tile.position(p).y, 0.0));
        CHECK(r == doctest::Approx(src.position(p).y > 5.0 ? R + 10.0 : R).epsilon(0.01));
    }
    CHECK(tile.validate().empty());

    // Grid parametrisation on a patch with no UVs; points outside run on past its edge.
    geo::Mesh noUv = bent;
    noUv.cornerAttrs().remove("uv0");
    geo::Mesh viaUv = src;
    CHECK_THROWS_AS(flowAlongSurface(viaUv, noUv, bo), std::invalid_argument);
    FlowOptions go = bo;
    go.param = FlowParam::Grid;
    geo::Mesh viaGrid = src;
    flowAlongSurface(viaGrid, noUv, go);
    for (geo::Index p = 0; p < src.pointCapacity(); ++p)
        CHECK(glm::distance(viaGrid.position(p), tile.position(p)) < 0.05);
}

TEST_CASE("grid parametrisation of quad patches")
{
    geo::Mesh plane = geo::makePlane({.width = 30.0, .depth = 20.0, .subdivX = 3, .subdivZ = 2});
    std::vector<geo::Index> faces;
    for (geo::Index f = 0; f < plane.faceCapacity(); ++f)
        faces.push_back(f);
    std::vector<D2> g = gridParam(plane, faces);
    // Corners of the patch take the corners of the unit square, and rows are evenly spaced.
    int corners = 0;
    for (geo::Index p = 0; p < plane.pointCapacity(); ++p)
        if ((g[p].x == 0.0 || g[p].x == 1.0) && (g[p].y == 0.0 || g[p].y == 1.0))
            ++corners;
    CHECK(corners == 4);
    CHECK(g[1].x == doctest::Approx(1.0 / 3.0));

    std::vector<geo::Index> some = {0, 1, 3};
    CHECK_THROWS_AS(gridParam(plane, some), std::invalid_argument); // an L shape is not a rectangle
    geo::Mesh cube = geo::makeCube({});
    std::vector<geo::Index> all;
    for (geo::Index f = 0; f < cube.faceCapacity(); ++f)
        all.push_back(f);
    CHECK_THROWS_AS(gridParam(cube, all), std::invalid_argument); // closes on itself
}
