#include "retopo/Retopo.h"

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace lila;

namespace
{
bool same(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-9) { return glm::length(a - b) < eps; }
} // namespace

TEST_CASE("angle snap: world, on a ground plane")
{
    const glm::dmat3 world(1.0);
    const glm::dvec3 up(0, 1, 0), anchor(1, 2, 3);

    // Slightly off the Z axis snaps onto it, keeping the length along it.
    retopo::AngleSnap s = retopo::angleSnap(anchor, anchor + glm::dvec3(0.2, 0.0, 10.0), up, world, 15.0);
    CHECK(s.axis == glm::dvec3(0, 1, 0));
    // The XZ plane's first axis is Z, so a line along Z is 0 degrees.
    CHECK(s.degrees == doctest::Approx(0.0));
    CHECK(s.point.x == doctest::Approx(anchor.x));
    CHECK(s.point.z == doctest::Approx(anchor.z + 10.0));

    // Height above the plane is flattened away, and 40 degrees snaps to 45.
    const double a = glm::radians(40.0);
    s = retopo::angleSnap(anchor, anchor + glm::dvec3(std::sin(a), 5.0, std::cos(a)) * 4.0, up, world, 15.0);
    CHECK(s.degrees == doctest::Approx(45.0));
    CHECK(s.point.y == doctest::Approx(anchor.y));
    const glm::dvec3 dir = glm::normalize(s.point - anchor);
    CHECK(same(dir, glm::normalize(glm::dvec3(1, 0, 1))));
    CHECK(glm::length(s.point - anchor) == doctest::Approx(4.0 * std::cos(glm::radians(5.0))));
}

TEST_CASE("angle snap: picks the plane facing the surface normal")
{
    const glm::dmat3 world(1.0);
    // A wall facing +X: snap in YZ, axis X.
    retopo::AngleSnap s = retopo::angleSnap({}, {0.3, 10.0, 0.4}, glm::normalize(glm::dvec3(0.9, 0.1, 0.2)), world, 90.0);
    CHECK(s.axis == glm::dvec3(1, 0, 0));
    CHECK(same(s.point, {0.0, 10.0, 0.0}));
    // Facing -Z picks the XY plane all the same.
    s = retopo::angleSnap({}, {10.0, 0.4, 3.0}, {0, 0, -1}, world, 90.0);
    CHECK(s.axis == glm::dvec3(0, 0, 1));
    CHECK(same(s.point, {10.0, 0.0, 0.0}));
}

TEST_CASE("angle snap: object space follows the reference's rotation")
{
    glm::dmat4 m = glm::rotate(glm::dmat4(1.0), glm::radians(30.0), glm::dvec3(0, 1, 0));
    m = glm::scale(m, glm::dvec3(2.0, 3.0, 0.5));
    const glm::dmat3 frame = retopo::rotationFrame(m);
    const glm::dvec3 objX = glm::normalize(glm::dvec3(m[0]));
    CHECK(same(frame[0], objX));
    CHECK(same(frame[1], {0, 1, 0}));
    CHECK(glm::dot(frame[0], frame[2]) == doctest::Approx(0.0));

    // A cursor 10 degrees off the object's X axis snaps back onto it.
    const glm::dvec3 anchor(5, 0, 5);
    const glm::dvec3 tilted = glm::dvec3(glm::rotate(glm::dmat4(1.0), glm::radians(10.0), glm::dvec3(0, 1, 0)) * glm::dvec4(objX, 0.0));
    retopo::AngleSnap s = retopo::angleSnap(anchor, anchor + tilted * 6.0, {0, 1, 0}, frame, 45.0);
    CHECK(same(glm::normalize(s.point - anchor), objX));
    CHECK(glm::length(s.point - anchor) == doctest::Approx(6.0 * std::cos(glm::radians(10.0))));
}

TEST_CASE("angle snap: no step only flattens; degenerate frames fall back to identity")
{
    retopo::AngleSnap s = retopo::angleSnap({}, {3.0, 2.0, 4.0}, {0, 1, 0}, glm::dmat3(1.0), 0.0);
    CHECK(same(s.point, {3.0, 0.0, 4.0}));
    s = retopo::angleSnap({1, 1, 1}, {1, 1, 1}, {0, 1, 0}, glm::dmat3(1.0), 15.0);
    CHECK(same(s.point, {1, 1, 1}));
    CHECK(retopo::rotationFrame(glm::dmat4(0.0)) == glm::dmat3(1.0));
    // Snapping straight back along an axis lands on 180, never -180.
    s = retopo::angleSnap({}, {-0.01, 0.0, -5.0}, {0, 1, 0}, glm::dmat3(1.0), 15.0);
    CHECK(s.degrees == doctest::Approx(180.0));
}
