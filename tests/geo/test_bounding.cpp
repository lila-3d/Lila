#include "TestUtil.h"

#include "geo/BoundingShapes.h"
#include "geo/Primitives.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
std::vector<glm::dvec3> pointsOf(const Mesh& m, const glm::dmat4& xf = glm::dmat4(1.0))
{
    std::vector<glm::dvec3> out;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            out.push_back(glm::dvec3(xf * glm::dvec4(m.position(p), 1.0)));
    return out;
}

double worstOutsideBox(const std::vector<glm::dvec3>& pts, const OrientedBox& b)
{
    double worst = -1e300;
    for (const glm::dvec3& p : pts)
        for (int i = 0; i < 3; ++i)
            worst = std::max(worst, std::abs(glm::dot(p - b.center, b.axes[i])) - b.size[i] * 0.5);
    return worst;
}

glm::dmat4 tilted()
{
    glm::dmat4 m = glm::translate(glm::dmat4(1.0), glm::dvec3(30.0, -12.0, 5.0));
    m = glm::rotate(m, 0.7, glm::normalize(glm::dvec3(1.0, 2.0, 0.5)));
    return glm::rotate(m, 0.3, glm::dvec3(0.0, 0.0, 1.0));
}
} // namespace

TEST_CASE("bounding: a rotated slab recovers its exact size")
{
    std::vector<glm::dvec3> pts = pointsOf(makeCube({240.0, 20.0, 90.0, 4, 2, 3}), tilted());
    std::optional<OrientedBox> b = minimumVolumeBox(pts);
    REQUIRE(b);
    glm::dvec3 s = b->size;
    std::array<double, 3> sorted{s.x, s.y, s.z};
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted[0] == doctest::Approx(20.0).epsilon(1e-6));
    CHECK(sorted[1] == doctest::Approx(90.0).epsilon(1e-6));
    CHECK(sorted[2] == doctest::Approx(240.0).epsilon(1e-6));
    CHECK(worstOutsideBox(pts, *b) < 1e-6);
    CHECK(glm::determinant(b->axes) == doctest::Approx(1.0));
}

TEST_CASE("bounding: a near-aligned cloud keeps near-identity axes")
{
    std::vector<glm::dvec3> pts = pointsOf(makeCube({100.0, 50.0, 30.0}), glm::rotate(glm::dmat4(1.0), 0.05, glm::dvec3(0, 1, 0)));
    std::optional<OrientedBox> b = minimumVolumeBox(pts);
    REQUIRE(b);
    for (int i = 0; i < 3; ++i)
        CHECK(b->axes[i][i] > 0.99);
    CHECK(b->size.x == doctest::Approx(100.0).epsilon(1e-6));
    CHECK(b->size.y == doctest::Approx(50.0).epsilon(1e-6));
}

TEST_CASE("bounding: sphere, cylinder and capsule contain every point")
{
    std::vector<glm::dvec3> sphere = pointsOf(makeIcoSphere({40.0, 3}), glm::translate(glm::dmat4(1.0), glm::dvec3(5, 6, 7)));
    RoundShape s = minimumSphere(sphere);
    CHECK(s.radius == doctest::Approx(40.0).epsilon(1e-6));
    CHECK(glm::length(s.center - glm::dvec3(5, 6, 7)) < 1e-6);

    glm::dmat4 xf = tilted();
    std::vector<glm::dvec3> rod = pointsOf(makeCylinder({10.0, 200.0, 24}), xf);
    OrientedBox frame = *minimumVolumeBox(rod);
    RoundShape c = fitCylinder(rod, frame.axes);
    CHECK(c.radius == doctest::Approx(10.0).epsilon(1e-3));
    CHECK(c.height == doctest::Approx(200.0).epsilon(1e-6));
    CHECK(std::abs(glm::dot(c.axes[1], glm::dvec3(xf[1]))) == doctest::Approx(1.0).epsilon(1e-6));

    std::vector<glm::dvec3> pill = pointsOf(makeCapsule({20.0, 160.0, 24, 1, 8}), xf);
    RoundShape p = fitCapsule(pill, minimumVolumeBox(pill)->axes);
    CHECK(p.radius == doctest::Approx(20.0).epsilon(1e-3));
    CHECK(p.height == doctest::Approx(160.0).epsilon(1e-3));
    double spine = 0.5 * (p.height - 2.0 * p.radius);
    for (const glm::dvec3& q : pill)
    {
        double t = std::clamp(glm::dot(q - p.center, p.axes[1]), -spine, spine);
        CHECK(glm::length(q - (p.center + p.axes[1] * t)) <= p.radius + 1e-6);
    }
}

TEST_CASE("bounding: hull mesh is closed, outward and capped")
{
    std::vector<glm::dvec3> pts = pointsOf(makeSphere({50.0, 32, 16}));
    Mesh hull = convexHullMesh(pts, 32);
    CHECK(isValid(hull));
    CHECK(boundaryEdgeCount(hull) == 0);
    CHECK(signedVolume(hull) > 0.0);
    CHECK(hull.numPoints() <= 32);

    Mesh box = convexHullMesh(pointsOf(makeCube({10.0, 10.0, 10.0, 3, 3, 3})), 32, 1.0);
    CHECK(box.numPoints() == 8);
    CHECK(box.bounds().size().x == doctest::Approx(12.0));
}

TEST_CASE("primitives: capsule")
{
    Mesh m = makeCapsule({25.0, 150.0, 16, 2, 5});
    CHECK(isValid(m));
    CHECK(boundaryEdgeCount(m) == 0);
    CHECK(euler(m) == 2);
    CHECK(signedVolume(m) > 0.0);
    CHECK(m.bounds().size().y == doctest::Approx(150.0));
    CHECK(m.bounds().size().x == doctest::Approx(50.0));
    Mesh ball = makeCapsule({25.0, 10.0});
    CHECK(boundaryEdgeCount(ball) == 0);
    CHECK(ball.bounds().size().y == doctest::Approx(50.0));
}
