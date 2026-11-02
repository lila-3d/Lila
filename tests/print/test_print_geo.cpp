#include <doctest/doctest.h>

#include "PrintGeo.h"
#include "ThreeMf.h"

#include <lila/Mesh.h>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace print;

namespace
{
Mesh cube(double half = 5.0) { return box(glm::dvec3(-half), glm::dvec3(half)); }

Mesh merged(const Mesh& a, const Mesh& b)
{
    Mesh out = a;
    out.append(b);
    return out;
}

/** A T: a 2 x 10 x 2 stem standing on the bed under a 10 x 2 x 2 bar, as two touching shells. */
Mesh tee() { return merged(box({-1, 0, -1}, {1, 10, 1}), box({-5, 10, -1}, {5, 12, 1})); }

double polygonArea(double r, int n) { return 0.5 * n * r * r * std::sin(2.0 * std::numbers::pi / n); }

const glm::dvec3 kUp(0, 1, 0);
} // namespace

TEST_CASE("print: a closed cube passes every check and measures right")
{
    const Mesh m = cube();
    const CheckReport r = checkPrintability(m, {.minWall = 0.08});
    CHECK(r.clean());
    CHECK(r.of(Check::Shells).count == 1);
    CHECK(volume(m) == doctest::Approx(1000.0));
    CHECK(surfaceArea(m) == doctest::Approx(600.0));
    const std::vector<double> th = wallThickness(m);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        CHECK(th[p] == doctest::Approx(10.0).epsilon(1e-6));
}

TEST_CASE("print: a cube with a missing face has one hole of four edges")
{
    Mesh m = cube();
    m.removeFace(0);
    const CheckReport r = checkPrintability(m);
    CHECK(r.of(Check::Holes).count == 1);
    CHECK(r.of(Check::Holes).edges.size() == 4);
    CHECK_FALSE(r.clean());
}

TEST_CASE("print: flipped faces, overlapping shells and self-intersections are found")
{
    Mesh flipped = cube();
    flipped.flipFace(2);
    CHECK(checkPrintability(flipped).of(Check::Flipped).count == 1);

    const Mesh overlap = merged(cube(), box(glm::dvec3(2.0), glm::dvec3(8.0)));
    const CheckReport r = checkPrintability(overlap);
    CHECK(r.of(Check::SelfIntersections).count > 0);
    CHECK(r.of(Check::Shells).count == 2);
    CHECK_FALSE(r.of(Check::Shells).faces.empty());

    CHECK(selfIntersectingFaces(merged(cube(), box(glm::dvec3(20.0), glm::dvec3(30.0)))).empty());
}

TEST_CASE("print: thin walls fall under the minimum")
{
    const Mesh wall = box({-5, -5, -0.025}, {5, 5, 0.025});
    const std::vector<double> th = wallThickness(wall);
    for (Index p = 0; p < wall.pointCapacity(); ++p)
        CHECK(th[p] == doctest::Approx(0.05).epsilon(1e-3));
    const CheckReport r = checkPrintability(wall, {.minWall = 0.08});
    CHECK(r.of(Check::ThinWalls).count == 8);
    CHECK(checkPrintability(cube(), {.minWall = 0.08}).of(Check::ThinWalls).count == 0);
}

TEST_CASE("print: overhangs, support area and the best orientation")
{
    const Mesh t = tee();
    const std::vector<double> angles = overhangAngles(t, kUp);
    int flatUnder = 0;
    for (Index f = 0; f < t.faceCapacity(); ++f)
        if (t.faceValid(f) && angles[f] > 89.0)
            ++flatUnder;
    // The bar's underside; the stem's foot rests on the bed and is no overhang.
    CHECK(flatUnder == 1);
    CHECK(supportArea(t, kUp, 45.0) == doctest::Approx(20.0));
    CHECK(supportArea(cube(), kUp, 45.0) == doctest::Approx(0.0));

    // Lying on its flat side the T needs no support at all.
    const Orientation o = bestOrientation(t, kUp, 45.0);
    CHECK(o.currentArea == doctest::Approx(20.0));
    CHECK(o.supportArea == doctest::Approx(0.0));
    CHECK(std::abs((o.rotation * glm::dvec3(0, 0, 1)).y) == doctest::Approx(1.0));

    // Upside down the bar rests on the bed and only the stem's end hangs.
    Mesh flipped = t;
    for (Index p = 0; p < flipped.pointCapacity(); ++p)
        flipped.setPosition(p, glm::dvec3(-1, -1, 1) * flipped.position(p));
    CHECK(supportArea(flipped, kUp, 45.0) == doctest::Approx(4.0));
}

TEST_CASE("print: hollowing keeps the wall and a drain hole opens the cavity")
{
    Mesh m = hollow(cube(), 1.0);
    CHECK(volume(m) == doctest::Approx(1000.0 - 512.0));
    CHECK(shells(m).size() == 2);
    const CheckReport r = checkPrintability(m, {.minWall = 0.08});
    CHECK(r.of(Check::Shells).count == 1);
    CHECK(r.of(Check::SelfIntersections).count == 0);
    CHECK(r.of(Check::Holes).count == 0);
    const std::vector<double> th = wallThickness(m);
    CHECK(*std::ranges::min_element(th) == doctest::Approx(1.0).epsilon(1e-6));

    const SurfacePoint low = lowestPoint(m, kUp);
    CHECK(low.position.y == doctest::Approx(-5.0));
    CHECK(glm::length(glm::dvec3(low.position.x, 0.0, low.position.z)) < 1e-9);
    CHECK(low.normal.y == doctest::Approx(-1.0));
    REQUIRE(drainHole(m, low, 0.5, 1.5).empty());
    CHECK(shells(m).size() == 1);
    CHECK(checkPrintability(m).of(Check::Holes).count == 0);
    CHECK(volume(m) == doctest::Approx(488.0 - polygonArea(0.5, 24) * 1.0).epsilon(1e-6));
}

TEST_CASE("print: a plane split gives two closed parts with a peg and its hole")
{
    const Mesh m = cube();
    const SplitResult s = splitByPlane(m, glm::dvec3(0.0), kUp, {.count = 1, .radius = 1.0, .length = 2.0, .tolerance = 0.1});
    REQUIRE(s.ok());
    CHECK(s.pegs == 1);
    for (const Mesh* part : {&s.above, &s.below})
    {
        const CheckReport r = checkPrintability(*part);
        CHECK(r.of(Check::Holes).count == 0);
        CHECK(r.of(Check::NonManifold).count == 0);
        CHECK(r.of(Check::Shells).count == 1);
    }
    CHECK(s.below.bounds().max.y == doctest::Approx(2.0));
    CHECK(s.above.bounds().min.y == doctest::Approx(0.0));
    CHECK(volume(s.below) == doctest::Approx(500.0 + polygonArea(1.0, 24) * 2.0).epsilon(1e-6));
    CHECK(volume(s.above) == doctest::Approx(500.0 - polygonArea(1.1, 24) * 2.1).epsilon(1e-6));

    // A hollow's cut is a ring: the pegs move into the wall, and one too wide for it is left out.
    const Mesh shell = hollow(m, 1.0);
    const SplitResult ring = splitByPlane(shell, glm::dvec3(0.0), kUp, {.count = 2, .radius = 0.2, .length = 0.5, .tolerance = 0.05});
    REQUIRE(ring.ok());
    CHECK(ring.pegs == 2);
    CHECK(checkPrintability(ring.below).of(Check::Holes).count == 0);
    CHECK(checkPrintability(ring.below).of(Check::Shells).count == 1);
    CHECK(splitByPlane(shell, glm::dvec3(0.0), kUp, {.count = 1, .radius = 0.6, .length = 0.5}).pegs == 0);

    const SplitResult plain = splitByPlane(m, glm::dvec3(0, 2, 0), kUp, {.count = 0});
    REQUIRE(plain.ok());
    CHECK(volume(plain.above) == doctest::Approx(300.0));
    CHECK(volume(plain.below) == doctest::Approx(700.0));
    CHECK_FALSE(splitByPlane(m, glm::dvec3(0, 50, 0), kUp, {.count = 0}).ok());
}

TEST_CASE("print: 3MF round trip keeps objects, unit, triangles and colours")
{
    Mesh a = cube();
    Mesh b = box(glm::dvec3(10.0), glm::dvec3(12.0));
    lila::geo::colors::add(b, "colorSet", glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    lila::geo::colors::setCurrent(b, "colorSet");
    const std::string bytes = threemf::write({{"Base", a}, {"Red & Small", b}}, {.unit = "millimeter"});
    REQUIRE(!bytes.empty());
    CHECK(bytes.substr(0, 2) == "PK");

    const threemf::Model model = threemf::read(bytes);
    REQUIRE(model.ok());
    CHECK(model.unit == "millimeter");
    for (const char* entry : {"[Content_Types].xml", "_rels/.rels", "3D/3dmodel.model"})
        CHECK(std::ranges::find(model.entries, entry) != model.entries.end());
    REQUIRE(model.objects.size() == 2);
    CHECK(model.objects[0].name == "Base");
    CHECK(model.objects[1].name == "Red & Small");
    CHECK(model.objects[0].mesh.numPoints() == 8);
    CHECK(model.objects[0].mesh.numFaces() == 12);
    CHECK(volume(model.objects[0].mesh) == doctest::Approx(1000.0));
    CHECK(volume(model.objects[1].mesh) == doctest::Approx(8.0));
    CHECK(model.colors);
    const std::vector<glm::vec4> c = lila::geo::colors::pointColors(model.objects[1].mesh, "colorSet");
    CHECK(c[0].r == doctest::Approx(1.0f));
    CHECK(c[0].g == doctest::Approx(0.0f));
    CHECK(threemf::unitToCm("inch") == doctest::Approx(2.54));
    CHECK_FALSE(threemf::read("not a zip").ok());
}
