#include <doctest/doctest.h>

#include "GroomTestUtil.h"

#include "geo/Attribute.h"
#include "groom/Locks.h"

#include <cmath>

using namespace lila;
using namespace lila::groom;
using namespace groomtest;

TEST_CASE("profiles: closed, counter-clockwise, within the unit box, evenly spaced")
{
    for (int p = 0; p < int(LockProfile::Count); ++p)
    {
        const std::vector<glm::dvec2> ring = profilePoints(LockProfile(p), 12);
        REQUIRE(ring.size() == 12);
        double area = 0.0;
        glm::dvec2 lo(1e9), hi(-1e9);
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const glm::dvec2 &a = ring[i], &b = ring[(i + 1) % ring.size()];
            area += a.x * b.y - b.x * a.y;
            lo = glm::min(lo, a);
            hi = glm::max(hi, a);
        }
        CHECK_MESSAGE(area > 0.0, profileName(LockProfile(p)));
        CHECK(lo.x >= -1.0 - 1e-9);
        CHECK(hi.x <= 1.0 + 1e-9);
        CHECK(lo.y >= -1.0 - 1e-9);
        CHECK(hi.y <= 1.0 + 1e-9);
    }
    LockProfile parsed;
    CHECK(parseProfile("crescent", &parsed));
    CHECK(parsed == LockProfile::Crescent);
    CHECK_FALSE(parseProfile("square", &parsed));
}

TEST_CASE("resolveLocks: widths follow the ramp, roots stay, the lock rests on the scalp, side lies flat to it")
{
    const Surface scalp = sphereScalp(10.0);
    Splines in{lyingGuide(10.0, 0.0)};
    LockOptions o;
    o.width = 2.0;
    o.thickness = 0.25;
    o.widthRamp = [](double t) { return 1.0 - 0.9 * t; };
    const Splines r = resolveLocks(in, &scalp, o);
    REQUIRE(r.size() == 1);
    const Spline& s = r[0];
    CHECK(glm::distance(s.points.front(), in[0].points.front()) < 1e-6);
    CHECK(attr::point(s, attr::kHalfWidth, 0, 0.0) == doctest::Approx(1.0));
    CHECK(attr::point(s, attr::kHalfWidth, s.size() - 1, 0.0) == doctest::Approx(0.1).epsilon(0.02));
    CHECK(attr::point(s, attr::kHalfThick, 0, 0.0) == doctest::Approx(0.25));
    CHECK(attr::point(s, attr::kAlong, s.size() - 1, 0.0) == doctest::Approx(1.0));
    for (size_t i = 1; i < s.size(); ++i)
    {
        // Resting: the centre sits at least half the thickness off the scalp.
        CHECK(glm::length(s.points[i]) >= 10.0 + attr::point(s, attr::kHalfThick, i, 0.0) - 0.02);
        // Flat: the side lies in the scalp's tangent plane.
        const D3 side(attr::point(s, attr::kSideX, i, 0.0), attr::point(s, attr::kSideY, i, 0.0), attr::point(s, attr::kSideZ, i, 0.0));
        CHECK(std::abs(glm::dot(side, glm::normalize(s.points[i]))) < 0.1);
    }
}

TEST_CASE("resolveLocks: per-curve width, tube profile and strays")
{
    Spline a = guide(D3(0, 10, 0), D3(0, 1, 0), 10.0, 8, 1.0);
    attr::set(a, attr::kWidth, 2.0);
    attr::set(a, attr::kProfile, double(int(LockProfile::Tube)));
    Spline st = guide(D3(1, 10, 0), D3(0, 1, 0), 10.0, 8, 2.0);
    attr::set(st, attr::kStray, 1.0);
    LockOptions o;
    o.width = 1.0;
    o.strayWidth = 0.1;
    const Splines r = resolveLocks({a, st}, nullptr, o);
    REQUIRE(r.size() == 2);
    CHECK(attr::point(r[0], attr::kHalfWidth, 0, 0.0) == doctest::Approx(1.0));
    CHECK(attr::point(r[0], attr::kHalfThick, 0, 0.0) == doctest::Approx(1.0));
    CHECK(attr::point(r[1], attr::kHalfWidth, 0, 0.0) == doctest::Approx(0.05));
}

TEST_CASE("lockMesh: closed quads with caps, uvs along the lock, lock ids per face")
{
    LockOptions o;
    o.sides = 8;
    o.segments = 10;
    const Splines r = resolveLocks({guide(D3(0), D3(0, 1, 0), 10.0, 6, 7.0), guide(D3(5, 0, 0), D3(0, 1, 0), 8.0, 6, 9.0)}, nullptr, o);
    const LockMesh lm = lockMesh(r, o);
    CHECK(lm.locks == 2);
    // 11 rings of 8: 10 * 8 quads and 2 caps per lock.
    CHECK(lm.mesh.numFaces() == 2 * (10 * 8 + 2));
    CHECK(lm.mesh.numPoints() == 2 * 11 * 8);
    bool watertight = true;
    for (geo::Index e = 0; e < lm.mesh.edgeCapacity(); ++e)
        if (lm.mesh.edgeValid(e) && lm.mesh.edgeFaceCount(e) != 2)
            watertight = false;
    CHECK(watertight);
    auto ids = lm.mesh.faceAttrs().get<int32_t>("lockId");
    REQUIRE(!ids.empty());
    int sevens = 0, nines = 0;
    for (geo::Index f = 0; f < lm.mesh.faceCapacity(); ++f)
        if (lm.mesh.faceValid(f))
            (ids[f] == 7 ? sevens : nines) += 1;
    CHECK(sevens == 82);
    CHECK(nines == 82);
    auto uv = lm.mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    REQUIRE(!uv.empty());
    float lo = 1e9f, hi = -1e9f;
    for (geo::Index c = 0; c < lm.mesh.cornerCapacity(); ++c)
        if (lm.mesh.cornerValid(c))
        {
            lo = std::min(lo, uv[c].y);
            hi = std::max(hi, uv[c].y);
        }
    CHECK(lo == doctest::Approx(0.0f));
    CHECK(hi == doctest::Approx(1.0f));
}

TEST_CASE("lockFrames: twist turns the side about the tangent by the given angle at the tip")
{
    const Spline s = guide(D3(0), D3(0, 1, 0), 10.0, 11);
    const Frames a = lockFrames(s, nullptr, 0.0);
    const Frames b = lockFrames(s, nullptr, 0.0, glm::radians(90.0));
    CHECK(glm::dot(a.side.front(), b.side.front()) == doctest::Approx(1.0));
    CHECK(std::abs(glm::dot(a.side.back(), b.side.back())) < 1e-6);
    for (size_t i = 0; i < s.size(); ++i)
        CHECK(std::abs(glm::dot(b.side[i], b.tangent[i])) < 1e-9);
}
