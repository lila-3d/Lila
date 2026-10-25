#include "geo/Bvh.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "retopo/LiveRelax.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

using namespace lila;
using geo::Index;

namespace
{
/** Flat grid in the XZ plane, 100 wide, interior points jittered in the plane by up to `amount` cells. */
geo::Mesh jitteredGrid(int cells, double amount, unsigned seed = 7)
{
    geo::PlaneOptions o;
    o.subdivX = o.subdivZ = cells;
    geo::Mesh m = geo::makePlane(o);
    const double cell = 100.0 / cells;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-amount * cell, amount * cell);
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        const double ox = u(rng), oz = u(rng);
        if (!m.isBoundaryPoint(p))
            m.setPosition(p, m.position(p) + glm::dvec3(ox, 0.0, oz));
    }
    return m;
}

geo::Mesh bigPlane()
{
    geo::PlaneOptions o;
    o.width = o.depth = 400.0;
    o.subdivX = o.subdivZ = 4;
    return geo::makePlane(o);
}

struct Stats
{
    double lengthCv = 0.0;    // edge length standard deviation over mean
    double angleDev = 0.0;    // mean |corner angle - 90 degrees|, in degrees
};

Stats quadStats(const geo::Mesh& m, std::span<const glm::dvec3> x)
{
    double sum = 0.0, sum2 = 0.0;
    Index edges = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        const double l = glm::length(x[m.edgePoint(e, 0)] - x[m.edgePoint(e, 1)]);
        sum += l;
        sum2 += l * l;
        ++edges;
    }
    const double mean = sum / edges;
    Stats s;
    s.lengthCv = std::sqrt(std::max(0.0, sum2 / edges - mean * mean)) / mean;
    double dev = 0.0;
    Index corners = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        m.forEachFaceCorner(f, [&](Index c) {
            const glm::dvec3 p = x[m.cornerPoint(c)];
            const glm::dvec3 a = x[m.cornerPoint(m.cornerPrev(c))] - p, b = x[m.cornerNextPoint(c)] - p;
            const double ang = std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
            dev += std::abs(glm::degrees(ang) - 90.0);
            ++corners;
        });
    }
    s.angleDev = dev / corners;
    return s;
}

/** Per face: whether a corner is turned against `up`; a zero `up` means outward from the origin. */
std::vector<uint8_t> foldFlags(const geo::Mesh& m, std::span<const glm::dvec3> x, const glm::dvec3& up)
{
    std::vector<uint8_t> out(m.faceCapacity(), 0);
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        m.forEachFaceCorner(f, [&](Index c) {
            const glm::dvec3 p = x[m.cornerPoint(c)];
            const glm::dvec3 n = glm::cross(x[m.cornerNextPoint(c)] - p, x[m.cornerPoint(m.cornerPrev(c))] - p);
            out[f] = out[f] || glm::dot(n, up == glm::dvec3(0.0) ? p : up) <= 0.0;
        });
    }
    return out;
}

Index foldedFaces(const geo::Mesh& m, std::span<const glm::dvec3> x, const glm::dvec3& up)
{
    Index count = 0;
    for (uint8_t f : foldFlags(m, x, up))
        count += f;
    return count;
}
} // namespace

TEST_CASE("live relax evens a jittered grid toward squares")
{
    geo::Mesh m = jitteredGrid(16, 0.3);
    geo::MeshBvh surface(bigPlane());
    retopo::LiveRelax relax(m, &surface);
    const Stats before = quadStats(m, relax.positions());
    relax.solve(300);
    const Stats after = quadStats(m, relax.positions());
    MESSAGE("length cv ", before.lengthCv, " -> ", after.lengthCv, ", angle dev ", before.angleDev, " -> ", after.angleDev);
    CHECK(after.lengthCv < 0.5 * before.lengthCv);
    CHECK(after.angleDev < 0.4 * before.angleDev);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        CHECK(relax.positions()[p].y == doctest::Approx(0.0).epsilon(1e-12));
}

TEST_CASE("live relax keeps points on a sphere")
{
    geo::QuadSphereOptions qo;
    qo.subdiv = 3;
    qo.radius = 50.0;
    geo::Mesh m = geo::makeQuadSphere(qo);
    geo::SphereOptions so;
    so.subdivAxis = so.subdivHeight = 96;
    so.radius = 50.0;
    geo::MeshBvh surface(geo::makeSphere(so));
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(-2.0, 2.0);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, m.position(p) + glm::dvec3(u(rng), u(rng), u(rng)));
    retopo::LiveRelaxOptions o;
    o.pinBoundary = false;
    retopo::LiveRelax relax(m, &surface, o);
    for (int i = 0; i < 100; ++i)
        relax.step();
    double worst = 0.0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        geo::BvhClosest c;
        REQUIRE(surface.closestPoint(relax.positions()[p], c));
        worst = std::max(worst, c.distance);
    }
    CHECK(worst < 1e-9);
    CHECK(foldedFaces(m, relax.positions(), glm::dvec3(0.0)) == 0);
}

TEST_CASE("live relax pins or slides the border")
{
    geo::Mesh m = jitteredGrid(12, 0.3);
    geo::MeshBvh surface(bigPlane());
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> u(-2.5, 2.5);
    // Border points jittered along their own side, so the border is an uneven but straight-sided square.
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        glm::dvec3 x = m.position(p);
        const bool onX = std::abs(std::abs(x.x) - 50.0) < 1e-9, onZ = std::abs(std::abs(x.z) - 50.0) < 1e-9;
        if (onX != onZ)
        {
            (onX ? x.z : x.x) += u(rng);
            m.setPosition(p, x);
        }
    }

    SUBCASE("pinned")
    {
        retopo::LiveRelax relax(m, &surface);
        relax.solve(100);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.isBoundaryPoint(p))
                CHECK(relax.positions()[p] == m.position(p));
    }
    SUBCASE("sliding")
    {
        retopo::LiveRelaxOptions o;
        o.pinBoundary = false;
        retopo::LiveRelax relax(m, &surface, o);
        relax.solve(200);
        double moved = 0.0;
        for (Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (!m.isBoundaryPoint(p))
                continue;
            const glm::dvec3 was = m.position(p), now = relax.positions()[p];
            moved = std::max(moved, glm::length(now - was));
            const bool onX = std::abs(std::abs(was.x) - 50.0) < 1e-9, onZ = std::abs(std::abs(was.z) - 50.0) < 1e-9;
            if (onX && onZ)
                CHECK(glm::length(now - was) < 1e-12);
            else if (onX)
                CHECK(now.x == doctest::Approx(was.x).epsilon(1e-12));
            else
                CHECK(now.z == doctest::Approx(was.z).epsilon(1e-12));
            CHECK(std::abs(now.x) <= 50.0 + 1e-9);
            CHECK(std::abs(now.z) <= 50.0 + 1e-9);
            CHECK(std::abs(now.y) < 1e-12);
        }
        CHECK(moved > 0.5);
    }
}

TEST_CASE("live relax target drags neighbours elastically")
{
    geo::PlaneOptions po;
    po.subdivX = po.subdivZ = 30;
    geo::Mesh m = geo::makePlane(po);
    geo::MeshBvh surface(bigPlane());
    Index centre = geo::kInvalid, near = geo::kInvalid, far = geo::kInvalid;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        const glm::dvec3 x = m.position(p);
        if (glm::length(x) < 1e-9)
            centre = p;
        else if (glm::length(x - glm::dvec3(100.0 / 30.0, 0.0, 0.0)) < 1e-9)
            near = p;
        else if (glm::length(x - glm::dvec3(-40.0, 0.0, 40.0)) < 1e-9)
            far = p;
    }
    REQUIRE(centre != geo::kInvalid);
    REQUIRE(near != geo::kInvalid);
    REQUIRE(far != geo::kInvalid);
    retopo::LiveRelax relax(m, &surface);
    const glm::dvec3 goal(8.0, 0.0, 0.0);
    relax.setTarget(centre, goal);
    relax.solve(300);
    const double nearMove = glm::length(relax.positions()[near] - m.position(near));
    const double farMove = glm::length(relax.positions()[far] - m.position(far));
    MESSAGE("centre off by ", glm::length(relax.positions()[centre] - goal), ", near moved ", nearMove, ", far moved ", farMove);
    CHECK(glm::length(relax.positions()[centre] - goal) < 0.05);
    CHECK(nearMove > 5.0);
    CHECK(farMove < 0.02 * nearMove);
    CHECK(foldedFaces(m, relax.positions(), glm::dvec3(0.0, 1.0, 0.0)) == 0);
}

TEST_CASE("live relax stays mirror exact")
{
    geo::Mesh m = jitteredGrid(14, 0.3);
    geo::PlaneOptions grid;
    grid.subdivX = grid.subdivZ = 14;
    const geo::Mesh flat = geo::makePlane(grid);
    // Make the jitter mirror symmetric across x = 0: the middle column sits exactly on the plane, the -x side copies the
    // reflection of its partner.
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (std::abs(flat.position(p).x) < 1e-6)
            m.setPosition(p, {0.0, 0.0, m.position(p).z});
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (flat.position(p).x >= -1e-6)
            continue;
        for (Index q = 0; q < m.pointCapacity(); ++q)
            if (glm::length(flat.position(q) - flat.position(p) * glm::dvec3(-1.0, 1.0, 1.0)) < 1e-6)
            {
                const glm::dvec3 x = m.position(q);
                m.setPosition(p, {-x.x, x.y, x.z});
            }
    }
    const geo::SymmetryPlane plane = geo::axisPlane(0);
    geo::MeshBvh surface(bigPlane());
    retopo::LiveRelaxOptions o;
    o.symmetry = plane;
    o.pinBoundary = false;
    retopo::LiveRelax relax(m, &surface, o);
    // A drag on one side only; its mirror image must follow exactly.
    Index dragged = geo::kInvalid;
    for (Index p = 0; p < m.pointCapacity() && dragged == geo::kInvalid; ++p)
        if (m.position(p).x > 15.0 && !m.isBoundaryPoint(p))
            dragged = p;
    REQUIRE(dragged != geo::kInvalid);
    relax.setTarget(dragged, m.position(dragged) + glm::dvec3(4.0, 0.0, 3.0));
    for (int i = 0; i < 80; ++i)
        relax.step();

    std::span<const glm::dvec3> x = relax.positions();
    Index pairs = 0, onPlane = 0;
    double worst = 0.0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (std::abs(m.position(p).x) == 0.0)
        {
            CHECK(x[p].x == 0.0);
            ++onPlane;
            continue;
        }
        for (Index q = 0; q < m.pointCapacity(); ++q)
            if (q != p && glm::length(m.position(q) - plane.reflect(m.position(p))) < 1e-9)
            {
                worst = std::max(worst, glm::length(x[q] - plane.reflect(x[p])));
                ++pairs;
            }
    }
    CHECK(onPlane == 15);
    CHECK(pairs == m.numPoints() - 15);
    CHECK(worst < 1e-9);
    CHECK(glm::length(x[dragged] - m.position(dragged)) > 2.0);
}

TEST_CASE("live relax never folds a face")
{
    geo::Mesh m = jitteredGrid(20, 0.45, 5);
    geo::MeshBvh surface(bigPlane());
    const glm::dvec3 up(0.0, 1.0, 0.0);
    const Index foldedBefore = foldedFaces(m, m.positions(), up);
    retopo::LiveRelaxOptions o;
    o.stiffness = 1.0;
    o.squareness = 1.0;
    o.evenSize = 1.0;
    o.targetLength = 9.0;
    retopo::LiveRelax relax(m, &surface, o);
    Index centre = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (glm::length(m.position(p)) < glm::length(m.position(centre)))
            centre = p;
    relax.setTarget(centre, glm::dvec3(35.0, 0.0, 30.0));
    Index newlyFolded = 0, worst = 0;
    for (int i = 0; i < 60; ++i)
    {
        const std::vector<uint8_t> was = foldFlags(m, relax.positions(), up);
        relax.step();
        const std::vector<uint8_t> now = foldFlags(m, relax.positions(), up);
        Index count = 0;
        for (size_t f = 0; f < now.size(); ++f)
        {
            newlyFolded += now[f] && !was[f];
            count += now[f];
        }
        worst = std::max(worst, count);
    }
    MESSAGE("folded or concave before ", foldedBefore, ", most during ", worst);
    CHECK(newlyFolded == 0);
    CHECK(worst <= foldedBefore);
}

TEST_CASE("live relax settles")
{
    geo::Mesh m = jitteredGrid(10, 0.25);
    geo::MeshBvh surface(bigPlane());
    retopo::LiveRelax relax(m, &surface);
    CHECK_FALSE(relax.settled());
    const int steps = relax.solve(2000);
    MESSAGE("settled in ", steps, " steps");
    CHECK(relax.settled());
    CHECK(steps < 2000);
    CHECK(relax.step() < relax.options().tolerance * 2.0);
}

TEST_CASE("live relax is deterministic")
{
    geo::Mesh m = jitteredGrid(24, 0.35, 9);
    geo::MeshBvh surface(bigPlane());
    retopo::LiveRelaxOptions o;
    o.pinBoundary = false;
    retopo::LiveRelax a(m, &surface, o), b(m, &surface, o);
    a.setTarget(30, glm::dvec3(10.0, 0.0, 10.0));
    b.setTarget(30, glm::dvec3(10.0, 0.0, 10.0));
    for (int i = 0; i < 40; ++i)
    {
        a.step();
        b.step();
    }
    bool same = true;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        same = same && a.positions()[p] == b.positions()[p];
    CHECK(same);
}

TEST_CASE("settleMesh honours active points")
{
    geo::Mesh m = jitteredGrid(10, 0.3);
    const geo::Mesh original = m;
    geo::MeshBvh surface(bigPlane());
    std::vector<uint8_t> active(m.pointCapacity(), 0);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        active[p] = m.position(p).x > 0.0;
    retopo::settleMesh(m, &surface, {}, 100, active);
    bool frozen = true, moved = false;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!active[p])
            frozen = frozen && m.position(p) == original.position(p);
        else
            moved = moved || m.position(p) != original.position(p);
    }
    CHECK(frozen);
    CHECK(moved);
}

TEST_CASE("live relax step timing at 5k points")
{
    geo::Mesh m = jitteredGrid(70, 0.3);
    geo::PlaneOptions po;
    po.width = po.depth = 200.0;
    po.subdivX = po.subdivZ = 100;
    geo::MeshBvh surface(geo::makePlane(po));
    retopo::LiveRelaxOptions o;
    o.pinBoundary = false;
    retopo::LiveRelax relax(m, &surface, o);
    relax.step();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i)
        relax.step();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 100.0;
    std::printf("live relax: %u points, %.3f ms per step\n", unsigned(m.numPoints()), ms);
    CHECK(ms < 50.0);
}
