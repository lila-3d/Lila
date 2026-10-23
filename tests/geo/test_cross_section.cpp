#include "TestUtil.h"

#include "geo/CrossSection.h"
#include "geo/Primitives.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace lila::geo;

namespace
{
void checkNear(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6)
{
    INFO("got (" << a.x << ", " << a.y << ", " << a.z << ") want (" << b.x << ", " << b.y << ", " << b.z << ")");
    CHECK(glm::length(a - b) < eps);
}

std::optional<SectionPick> center(const Mesh& m, const glm::dvec3& p, const glm::dvec3& dir, const glm::dmat4& world = glm::dmat4(1.0))
{
    SectionMesh sm{&m, world};
    return sectionCenter(std::span<const SectionMesh>(&sm, 1), p, dir);
}
} // namespace

TEST_CASE("cross section of a cylinder centres an off-axis point")
{
    Mesh cyl = makeCylinder({.radius = 10.0, .height = 100.0, .subdivAxis = 16, .subdivHeight = 4});
    Bounds b = cyl.bounds();
    REQUIRE(b.max.y - b.min.y == doctest::Approx(100.0));

    auto pick = center(cyl, {3.0, 12.0, -4.0}, {0, 1, 0});
    REQUIRE(pick);
    CHECK(pick->inside);
    checkNear(pick->center, {0.0, 12.0, 0.0});

    // Tilted direction: the oblique cut is an ellipse, still centred on the axis.
    pick = center(cyl, {2.0, 5.0, 1.0}, glm::normalize(glm::dvec3(0.3, 1.0, 0.0)));
    REQUIRE(pick);
    CHECK(pick->inside);
    CHECK(glm::length(glm::dvec2(pick->center.x, pick->center.z)) < 1e-6);

    // Placed in the world: the mesh's transform applies.
    glm::dmat4 w = glm::translate(glm::dmat4(1.0), glm::dvec3(50, 0, 0));
    pick = center(cyl, {52.0, 0.0, 3.0}, {0, 1, 0}, w);
    REQUIRE(pick);
    checkNear(pick->center, {50.0, 0.0, 0.0});
}

TEST_CASE("cut along the axis goes through n-gon caps")
{
    Mesh cyl = makeCylinder({.radius = 10.0, .height = 40.0, .subdivAxis = 12});
    // Plane x = 2 cuts the side and both 12-sided caps into a rectangle centred at y = 0, z = 0.
    auto pick = center(cyl, {2.0, 7.0, 1.0}, {1, 0, 0});
    REQUIRE(pick);
    CHECK(pick->inside);
    checkNear(pick->center, {2.0, 0.0, 0.0}, 1e-6);
    CHECK(pick->area > 40.0 * 15.0);
}

TEST_CASE("bent tube: the torus cut picks the side of the ring holding the point")
{
    Mesh torus = makeTorus({.radius = 30.0, .sectionRadius = 5.0, .subdivAxis = 32, .subdivHeight = 16});
    Bounds b = torus.bounds();
    REQUIRE(b.max.y - b.min.y == doctest::Approx(10.0).epsilon(0.01));
    // The ring lies in XZ; at (30, 0, 0) the tube runs along Z, and the plane z = 0 cuts two loops at x = +-30.
    auto pick = center(torus, {31.5, 2.0, 0.0}, {0, 0, 1});
    REQUIRE(pick);
    CHECK(pick->inside);
    CHECK(pick->center.x == doctest::Approx(30.0).epsilon(0.02));
    CHECK(std::abs(pick->center.y) < 1e-6);

    pick = center(torus, {-28.0, -1.0, 0.0}, {0, 0, 1});
    REQUIRE(pick);
    CHECK(pick->center.x == doctest::Approx(-30.0).epsilon(0.02));
    // Outside both tubes: the nearest loop wins.
    pick = center(torus, {-40.0, 0.0, 0.0}, {0, 0, 1});
    REQUIRE(pick);
    CHECK_FALSE(pick->inside);
    CHECK(pick->center.x < -25.0);
}

TEST_CASE("two limbs: the cut picks the limb under the point, across one mesh or two")
{
    Mesh left = makeCylinder({.radius = 6.0, .height = 80.0, .subdivAxis = 12, .subdivHeight = 3});
    Mesh right = left;
    for (Index p = 0; p < left.pointCapacity(); ++p)
    {
        left.setPosition(p, left.position(p) + glm::dvec3(-15, 0, 0));
        right.setPosition(p, right.position(p) + glm::dvec3(15, 0, 0));
    }
    Mesh legs = left;
    legs.append(right);

    auto pick = center(legs, {13.0, -10.0, 2.0}, {0, 1, 0});
    REQUIRE(pick);
    CHECK(pick->inside);
    checkNear(pick->center, {15.0, -10.0, 0.0});
    pick = center(legs, {-17.0, 20.0, -1.0}, {0, 1, 0});
    REQUIRE(pick);
    checkNear(pick->center, {-15.0, 20.0, 0.0});

    SectionMesh both[] = {{&left}, {&right}};
    pick = sectionCenter(both, {16.0, 0.0, 0.0}, {0, 1, 0});
    REQUIRE(pick);
    checkNear(pick->center, {15.0, 0.0, 0.0});
}

TEST_CASE("open and non-manifold meshes still give closed loops")
{
    // A cylinder with a column of side faces removed: the cut is an open arc that gets closed across the gap.
    Mesh cyl = makeCylinder({.radius = 10.0, .height = 20.0, .subdivAxis = 16, .subdivHeight = 1, .caps = false});
    for (Index f = 0; f < cyl.faceCapacity(); ++f)
        if (cyl.faceValid(f) && cyl.faceCentroid(f).x > 9.0)
        {
            cyl.removeFace(f);
            break;
        }
    std::vector<SectionContour> loops;
    SectionMesh sm{&cyl};
    loops = crossSections(std::span<const SectionMesh>(&sm, 1), {0, 0, 0}, {0, 1, 0});
    REQUIRE(loops.size() == 1);
    CHECK(loops[0].bridged);
    auto pick = center(cyl, {1.0, 0.0, 1.0}, {0, 1, 0});
    REQUIRE(pick);
    CHECK(pick->inside);
    CHECK(glm::length(pick->center) < 1.0);

    // A fin face sharing an edge with the tube (three faces on one edge) must not break the loop.
    Mesh fin = makeCylinder({.radius = 10.0, .height = 20.0, .subdivAxis = 8, .subdivHeight = 1, .caps = false});
    Index e = kInvalid;
    for (Index i = 0; i < fin.edgeCapacity(); ++i)
        if (fin.edgeValid(i))
        {
            glm::dvec3 a = fin.position(fin.edgePoint(i, 0)), b = fin.position(fin.edgePoint(i, 1));
            if (std::abs(a.y - b.y) > 1.0)
            {
                e = i;
                break;
            }
        }
    REQUIRE(e != kInvalid);
    Index a = fin.edgePoint(e, 0), b = fin.edgePoint(e, 1);
    glm::dvec3 out = glm::normalize(glm::dvec3(fin.position(a).x, 0, fin.position(a).z)) * 10.0;
    Index c = fin.addPoint(fin.position(b) + out), d = fin.addPoint(fin.position(a) + out);
    REQUIRE(fin.addFace({a, b, c, d}) != kInvalid);
    auto finPick = center(fin, {0.5, 0.0, -0.5}, {0, 1, 0});
    REQUIRE(finPick);
    CHECK(finPick->inside);
    CHECK(glm::length(finPick->center) < 2.0);

    // Nothing to cut.
    CHECK_FALSE(center(cyl, {0, 500, 0}, {0, 1, 0}));
}
