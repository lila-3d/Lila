#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "geo/Slide.h"

using namespace lila;
using namespace testutil;
using geo::Index;

namespace
{
/** Plane point id at column i, row j (rows run toward -Z). */
Index gridId(int sx, int i, int j) { return Index(j * (sx + 1) + i); }

/** The edges of column i of a 4x4 plane, end to end. */
std::vector<Index> column(const geo::Mesh& m, int i)
{
    std::vector<Index> out;
    for (int j = 0; j < 4; ++j)
        out.push_back(m.findEdge(gridId(4, i, j), gridId(4, i, j + 1)));
    return out;
}
} // namespace

TEST_CASE("edge slide moves a loop along the edges beside it and keeps the surface")
{
    geo::Mesh m = geo::makePlane({.width = 100.0, .depth = 100.0, .subdivX = 4, .subdivZ = 4});
    const glm::dvec3 plusX(1.0, 0.0, 0.0);
    geo::SlideRails rails = geo::edgeSlideRails(m, column(m, 2), &plusX);
    REQUIRE(rails.points.size() == 5);
    std::vector<glm::dvec3> to = geo::slidePositions(m, rails, 0.5, false);
    for (size_t k = 0; k < rails.points.size(); ++k)
    {
        const glm::dvec3 from = m.position(rails.points[k]);
        CHECK(to[k].x == doctest::Approx(12.5));
        CHECK(to[k].y == doctest::Approx(0.0));
        CHECK(to[k].z == doctest::Approx(from.z));
    }
    to = geo::slidePositions(m, rails, -1.0, false);
    for (const glm::dvec3& p : to)
        CHECK(p.x == doctest::Approx(-25.0));

    // Uneven neighbours: a fraction follows each rail, even moves every point the shortest rail's share.
    m.setPosition(gridId(4, 3, 2), m.position(gridId(4, 3, 2)) + glm::dvec3(25.0, 0.0, 0.0));
    rails = geo::edgeSlideRails(m, column(m, 2), &plusX);
    to = geo::slidePositions(m, rails, 0.5, false);
    const auto mid = std::ranges::find(rails.points, gridId(4, 2, 2)) - rails.points.begin();
    CHECK(to[size_t(mid)].x == doctest::Approx(25.0));
    to = geo::slidePositions(m, rails, 0.5, true);
    for (const glm::dvec3& p : to)
        CHECK(p.x == doctest::Approx(12.5));
}

TEST_CASE("edge slide on a border loop only slides inward")
{
    geo::Mesh m = geo::makePlane({.width = 100.0, .depth = 100.0, .subdivX = 4, .subdivZ = 4});
    const glm::dvec3 plusX(1.0, 0.0, 0.0);
    const geo::SlideRails rails = geo::edgeSlideRails(m, column(m, 0), &plusX);
    REQUIRE(rails.points.size() == 5);
    for (const glm::dvec3& p : geo::slidePositions(m, rails, 1.0, false))
        CHECK(p.x == doctest::Approx(-25.0));
    for (const glm::dvec3& p : geo::slidePositions(m, rails, -1.0, false))
        CHECK(p.x == doctest::Approx(-50.0));
}

TEST_CASE("vertex slide follows the edge most along the direction")
{
    geo::Mesh m = geo::makePlane({.width = 100.0, .depth = 100.0, .subdivX = 4, .subdivZ = 4});
    const Index v = gridId(4, 2, 2);
    const geo::SlideRails rails = geo::vertexSlideRails(m, std::vector<Index>{v}, glm::dvec3(0.2, 0.0, -1.0));
    REQUIRE(rails.points.size() == 1);
    CHECK(rails.ends[0][0] == gridId(4, 2, 3));
    CHECK(rails.ends[0][1] == gridId(4, 2, 1));
    CHECK(geo::slidePositions(m, rails, 1.0, false)[0].z == doctest::Approx(m.position(gridId(4, 2, 3)).z));
    CHECK(geo::slidePositions(m, rails, -0.5, false)[0].z == doctest::Approx(m.position(v).z + 12.5));
}

TEST_CASE("polySlide slides the selected edges as one undo step")
{
    run("newScene");
    const std::string plane = run("polyPlane", {{"width", 100}, {"depth", 100}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    const geo::Mesh before = meshOf(plane);
    run("selectComponents", {{"args", {plane}}, {"edges", column(before, 2)}});
    run("polySlide", {{"amount", -0.5}, {"direction", {1, 0, 0}}});
    for (int j = 0; j <= 4; ++j)
    {
        CHECK(meshOf(plane).position(gridId(4, 2, j)).x == doctest::Approx(-12.5));
        CHECK(meshOf(plane).position(gridId(4, 2, j)).z == doctest::Approx(before.position(gridId(4, 2, j)).z));
    }
    run("undo");
    CHECK(meshOf(plane).position(gridId(4, 2, 2)).x == doctest::Approx(0.0));

    run("selectComponents", {{"args", {plane}}, {"vertices", {gridId(4, 2, 2)}}});
    CHECK_THROWS_AS(run("polySlide", {{"amount", 0.5}}), CommandError);
    run("polySlide", {{"amount", 0.5}, {"direction", {0, 0, -1}}});
    CHECK(meshOf(plane).position(gridId(4, 2, 2)).z == doctest::Approx(before.position(gridId(4, 2, 2)).z - 12.5));
}
