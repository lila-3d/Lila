#include "clay/Clay.h"
#include "geo/ColorSets.h"

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <random>

using namespace lila;
using namespace lila::clay;

namespace
{
Item ballAt(glm::dvec3 at, double r, Op op = Op::Add, double blend = 0.0)
{
    return {.op = op, .blend = blend, .matrix = glm::translate(glm::dmat4(1.0), at), .shape = Primitive{.shape = Shape::Sphere, .radius = r}};
}

Item tree(std::vector<Item> children) { return Item{.children = std::move(children)}; }

double area(const geo::Mesh& m)
{
    double a = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            a += m.faceArea(f);
    return a;
}

double volume(const geo::Mesh& m)
{
    double v = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            const std::vector<geo::Index> pts = m.facePoints(f);
            for (size_t i = 1; i + 1 < pts.size(); ++i)
                v += glm::dot(m.position(pts[0]), glm::cross(m.position(pts[i]), m.position(pts[i + 1]))) / 6.0;
        }
    return v;
}

/** Every edge has exactly two faces and the mesh's structure, winding and manifoldness check out. */
void checkClosed(const geo::Mesh& m)
{
    CHECK(m.validate().empty());
    size_t open = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
            ++open;
    CHECK(open == 0);
}

std::vector<glm::dvec3> sortedPoints(const geo::Mesh& m)
{
    std::vector<glm::dvec3> out;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        out.push_back(m.position(p));
    std::ranges::sort(out, [](const glm::dvec3& a, const glm::dvec3& b) {
        return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
    });
    return out;
}
} // namespace

TEST_CASE("clay mesher: a sphere is closed and within 1% of its area and volume")
{
    const geo::Mesh m = mesh(Field(tree({ballAt({3.3, -1.2, 0.7}, 10)})), {.voxelSize = 0.5});
    REQUIRE(m.faceCapacity() > 0);
    checkClosed(m);
    const double pi = 3.14159265358979;
    CHECK(area(m) == doctest::Approx(4.0 * pi * 100.0).epsilon(0.01));
    CHECK(volume(m) == doctest::Approx(4.0 / 3.0 * pi * 1000.0).epsilon(0.01));
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        CHECK(glm::length(m.position(p) - glm::dvec3(3.3, -1.2, 0.7)) == doctest::Approx(10.0).epsilon(0.002));
}

TEST_CASE("clay mesher: joins, cuts and groups stay closed")
{
    Item g{.blend = 1.0, .children = {ballAt({20, 0, 0}, 6), ballAt({24, 0, 0}, 3, Op::Subtract, 1.0)}};
    Item capsule{.blend = 3.0, .matrix = glm::rotate(glm::dmat4(1.0), 1.0, glm::dvec3(0, 0, 1)),
                 .shape = Primitive{.shape = Shape::Capsule, .radius = 3, .endRadius = 2, .length = 20}};
    Item box{.op = Op::Subtract, .blend = 0.5, .matrix = glm::translate(glm::dmat4(1.0), glm::dvec3(0, 8, 0)),
             .shape = Primitive{.shape = Shape::Box, .extents = {4, 4, 4}, .rounding = 1}};
    const geo::Mesh m = mesh(Field(tree({ballAt({0, 0, 0}, 8), capsule, box, g})), {.voxelSize = 0.5});
    REQUIRE(m.faceCapacity() > 0);
    checkClosed(m);
    CHECK(volume(m) > 0.0);
}

TEST_CASE("clay mesher: symmetry gives a mirror-exact mesh")
{
    const geo::Mesh m = mesh(Field(tree({ballAt({6, 2, 0}, 4, Op::Add), ballAt({0, 0, 0}, 5, Op::Add, 2.0)}), true), {.voxelSize = 0.5});
    checkClosed(m);
    // Each point's mirror image is another point of the mesh.
    double worst = 0.0;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        const glm::dvec3 q = m.position(p) * glm::dvec3(-1, 1, 1);
        double nearest = 1e30;
        for (geo::Index o = 0; o < m.pointCapacity(); ++o)
            nearest = std::min(nearest, glm::length(m.position(o) - q));
        worst = std::max(worst, nearest);
    }
    CHECK(worst < 1e-6);
}

TEST_CASE("clay mesher: an update re-meshes only what changed and matches a full build")
{
    std::vector<Item> items;
    for (int i = 0; i < 40; ++i)
        items.push_back(ballAt({i * 9.0, 0, 0}, 5, Op::Add, 2.0));
    Mesher mesher;
    mesher.update(Field(tree(items)), {.voxelSize = 0.5});
    const size_t all = mesher.brickCount();

    items[3].matrix = glm::translate(glm::dmat4(1.0), glm::dvec3(27, 2, 1));
    const Field moved(tree(items));
    const geo::Mesh live = *mesher.update(moved, {.voxelSize = 0.5});
    CHECK(mesher.lastRebuilt() < all / 2);
    const geo::Mesh fresh = mesh(moved, {.voxelSize = 0.5});
    checkClosed(live);

    const std::vector<glm::dvec3> a = sortedPoints(live), b = sortedPoints(fresh);
    REQUIRE(a.size() == b.size());
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, glm::length(a[i] - b[i]));
    CHECK(worst < 1e-9);
    CHECK(live.faceCapacity() == fresh.faceCapacity());

    // Nothing changed: nothing re-meshed.
    mesher.update(moved, {.voxelSize = 0.5});
    CHECK(mesher.lastRebuilt() == 0);
}

TEST_CASE("clay mesher: colours, an empty field, and a runaway voxel size")
{
    Item red = ballAt({0, 0, 0}, 5);
    red.shape->color = {1, 0, 0};
    const geo::Mesh m = mesh(Field(tree({red})), {.voxelSize = 0.5});
    REQUIRE(geo::colors::has(m, "clay"));
    CHECK(geo::colors::pointColors(m, "clay")[0].x == doctest::Approx(1.0));
    CHECK(geo::colors::pointColors(m, "clay")[0].y == doctest::Approx(0.0));
    CHECK_FALSE(geo::colors::has(mesh(Field(tree({ballAt({0, 0, 0}, 5)})), {.voxelSize = 0.5}), "clay"));

    CHECK(mesh(Field(tree({})), {.voxelSize = 0.5}).faceCapacity() == 0);
    CHECK_THROWS(mesh(Field(tree({ballAt({0, 0, 0}, 1000)})), {.voxelSize = 0.01}));
}

TEST_CASE("clay mesher: 200 shapes mesh at an effective 256^3 in reasonable time")
{
    std::vector<Item> items;
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(-50.0, 50.0);
    for (int i = 0; i < 200; ++i)
        items.push_back(ballAt({u(rng), u(rng), u(rng)}, 3.0 + std::abs(u(rng)) * 0.08, i % 6 == 5 ? Op::Subtract : Op::Add, 1.5));
    const Field f(tree(items));
    const auto t0 = std::chrono::steady_clock::now();
    Mesher mesher;
    const auto built = mesher.update(f, {.voxelSize = 0.45});
    const geo::Mesh& m = *built;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    items[10].matrix = glm::translate(items[10].matrix, glm::dvec3(1.0, 0.5, 0.0));
    const auto t1 = std::chrono::steady_clock::now();
    mesher.update(Field(tree(items)), {.voxelSize = 0.45});
    MESSAGE("moving one shape: " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() << " ms, "
                                 << mesher.lastRebuilt() << " bricks");
    size_t open = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
            ++open;
    MESSAGE("200 shapes at 0.45 cm: " << ms << " ms, " << m.faceCapacity() << " quads, " << open << " edges not shared by two faces");
    CHECK(m.faceCapacity() > 0);
}

TEST_CASE("clay mesher: sharp edges keep a box's corners, closed")
{
    Item box{.matrix = glm::translate(glm::dmat4(1.0), glm::dvec3(0.13, 0.27, -0.31)), .shape = Primitive{.shape = Shape::Box, .extents = {5, 4, 3}}};
    const Field f(tree({box}));
    auto corner = [](const geo::Mesh& m) {
        double best = 0.0;
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            best = std::max(best, glm::dot(m.position(p) - glm::dvec3(0.13, 0.27, -0.31), glm::dvec3(1, 1, 1)));
        return best;
    };
    const geo::Mesh soft = mesh(f, {.voxelSize = 0.5});
    const geo::Mesh hard = mesh(f, {.voxelSize = 0.5, .sharp = true});
    checkClosed(hard);
    CHECK(hard.validate(geo::kValidateAll).empty());
    CHECK(corner(hard) == doctest::Approx(12.0).epsilon(0.01));
    CHECK(corner(soft) < corner(hard) - 0.3);
    CHECK(volume(hard) == doctest::Approx(480.0).epsilon(0.01));
}
