#include "TestUtil.h"

#include "geo/Bvh.h"
#include "geo/Primitives.h"

#include <tbb/parallel_for.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <random>

using namespace lila::geo;

namespace
{
bool bruteRay(const Mesh& m, const Triangulation& tris, const glm::dvec3& o, const glm::dvec3& d, double& best)
{
    best = std::numeric_limits<double>::max();
    bool any = false;
    for (Index t = 0; t < tris.triangleCount(); ++t)
    {
        const glm::dvec3 a = m.position(m.cornerPoint(tris.corners[t * 3]));
        const glm::dvec3 b = m.position(m.cornerPoint(tris.corners[t * 3 + 1]));
        const glm::dvec3 c = m.position(m.cornerPoint(tris.corners[t * 3 + 2]));
        const glm::dvec3 e1 = b - a, e2 = c - a, p = glm::cross(d, e2);
        const double det = glm::dot(e1, p);
        if (det == 0.0)
            continue;
        const glm::dvec3 s = o - a;
        const double u = glm::dot(s, p) / det;
        const glm::dvec3 q = glm::cross(s, e1);
        const double v = glm::dot(d, q) / det;
        const double tt = glm::dot(e2, q) / det;
        if (u >= 0 && v >= 0 && u + v <= 1 && tt >= 0 && tt < best)
        {
            best = tt;
            any = true;
        }
    }
    return any;
}
} // namespace

TEST_CASE("bvh: raycast and closest point match brute force")
{
    Mesh m = makeTorus({50.0, 20.0, 24, 16});
    m.append(makeCube({30, 30, 30}));
    MeshBvh bvh(m);
    CHECK(bvh.triangleCount() == 24 * 16 * 2 + 12);
    CHECK(bvh.bounds().size().x == doctest::Approx(140.0).epsilon(1e-4));

    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    int hits = 0;
    for (int i = 0; i < 300; ++i)
    {
        const glm::dvec3 o(u(rng) * 120, u(rng) * 120, u(rng) * 120);
        const glm::dvec3 d = glm::normalize(glm::dvec3(u(rng), u(rng), u(rng)));
        double brute;
        const bool expect = bruteRay(m, bvh.triangles(), o, d, brute);
        BvhHit hit;
        const bool got = bvh.raycast(o, d, hit);
        CHECK(got == expect);
        if (got && expect)
        {
            CHECK(hit.t == doctest::Approx(brute));
            ++hits;
            CHECK(glm::distance(hit.point, o + d * brute) < 1e-6);
            CHECK(hit.bary.x + hit.bary.y + hit.bary.z == doctest::Approx(1.0));
        }
        CHECK(bvh.occluded(o, d, 1e9) == expect);
    }
    CHECK(hits > 20);

    for (int i = 0; i < 100; ++i)
    {
        const glm::dvec3 p(u(rng) * 100, u(rng) * 100, u(rng) * 100);
        BvhClosest c;
        REQUIRE(bvh.closestPoint(p, c));
        // No vertex may be closer than the reported surface distance.
        for (Index v = 0; v < m.pointCapacity(); ++v)
            CHECK(glm::distance(m.position(v), p) >= c.distance - 1e-9);
        CHECK(glm::distance(c.point, p) == doctest::Approx(c.distance));
    }

    // Uv under a ray through the cube's top.
    Mesh cube = makeCube();
    MeshBvh cb(cube);
    BvhHit hit;
    REQUIRE(cb.raycast({0, 100, 0}, {0, -1, 0}, hit));
    CHECK(hit.t == doctest::Approx(50.0));
    CHECK(cube.faceNormal(hit.face).y == doctest::Approx(1.0));
    CHECK(hit.frontFacing);
    const glm::vec2 uv = cb.interpolate(std::as_const(cube).cornerAttrs().get<glm::vec2>("uv0"), hit.triangle, hit.bary);
    CHECK(uv.x == doctest::Approx(0.5f));
    CHECK(uv.y == doctest::Approx(0.375f));

    // Refit after moving points.
    for (glm::dvec3& p : cube.positions())
        p.y += 10.0;
    cb.refit(cube);
    REQUIRE(cb.raycast({0, 100, 0}, {0, -1, 0}, hit));
    CHECK(hit.t == doctest::Approx(40.0));
    CHECK(cb.overlapping(cube.bounds()).size() == 12);
}

TEST_CASE("perf: bvh 1M faces")
{
    Mesh m = makePlane({1000.0, 1000.0, 1000, 1000});
    for (glm::dvec3& p : m.positions())
        p.y = 10.0 * std::sin(p.x * 0.05) * std::cos(p.z * 0.05);
    auto t0 = std::chrono::steady_clock::now();
    MeshBvh bvh(m);
    const double tBuild = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const int rays = 1000000;
    std::atomic<int> hits{0};
    t0 = std::chrono::steady_clock::now();
    tbb::parallel_for(0, rays, [&](int i) {
        const double x = (i % 1000) - 499.5, z = (i / 1000) - 499.5;
        BvhHit h;
        if (bvh.raycast({x, 100.0, z}, {0.1, -1.0, 0.05}, h))
            ++hits;
    });
    const double tRays = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(hits.load() > rays * 9 / 10);
    t0 = std::chrono::steady_clock::now();
    std::atomic<int> found{0};
    tbb::parallel_for(0, rays, [&](int i) {
        BvhClosest c;
        if (bvh.closestPoint({(i % 1000) - 499.5, 30.0, (i / 1000) - 499.5}, c))
            ++found;
    });
    const double tClosest = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("bvh 2M tris: build " << tBuild << "s (incl. triangulation), 1M rays " << tRays << "s, 1M closest " << tClosest << "s");
}
