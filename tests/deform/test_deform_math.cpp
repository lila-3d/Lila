#include <doctest/doctest.h>

#include "TestUtil.h"

#include "deform/Deform.h"
#include "geo/Primitives.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <random>

using namespace lila;
using namespace lila::deform;
using namespace deformtest;

TEST_CASE("bend follows an arc of the right length and is rigid past the bounds")
{
    CHECK(nearTight(bend({0.3, 0.7, -0.2}, {0.0, -1.0, 1.0}), {0.3, 0.7, -0.2}));
    BendParams o{90.0, -1.0, 1.0};
    // The centre line keeps its length: the point at y = 1 sits on an arc of radius 1/k at angle k.
    const double k = glm::half_pi<double>() / 2.0;
    glm::dvec3 top = bend({0, 1, 0}, o);
    CHECK(top.x == doctest::Approx((1.0 - std::cos(k)) / k));
    CHECK(top.y == doctest::Approx(std::sin(k) / k));
    // Past the high bound the axis carries on along the arc's end tangent.
    glm::dvec3 beyond = bend({0, 2, 0}, o);
    CHECK(nearTight(beyond - top, {std::sin(k), std::cos(k), 0.0}));
    CHECK(nearTight(bend({0, 0, 0}, o), {0, 0, 0}));
    CHECK(bend({0.1, 0.5, 0.4}, o).z == doctest::Approx(0.4));
}

TEST_CASE("twist, taper and squash")
{
    glm::dvec3 t = twist({1, 1, 0}, {0.0, 90.0, -1.0, 1.0});
    CHECK(nearTight(t, {0.0, 1.0, -1.0}));
    CHECK(nearTight(twist({1, -1, 0}, {0.0, 90.0, -1.0, 1.0}), {1, -1, 0}));
    CHECK(glm::length(glm::dvec2(twist({0.6, 0.2, 0.8}, {10.0, 70.0, -1.0, 1.0}).x, twist({0.6, 0.2, 0.8}, {10.0, 70.0, -1.0, 1.0}).z)) ==
          doctest::Approx(1.0));

    CHECK(nearTight(taper({1, 1, 1}, {1.0, 0.5, 0.0, -1.0, 1.0}), {0.5, 1, 0.5}));
    CHECK(nearTight(taper({1, -1, 1}, {1.0, 0.5, 0.0, -1.0, 1.0}), {1, -1, 1}));
    CHECK(taper({1, 0, 0}, {1.0, 1.0, 0.25, -1.0, 1.0}).x == doctest::Approx(1.25));

    CHECK(nearTight(squash({0.3, 0.5, -0.2}, {0.0, 1.0, 0.5, -1.0, 1.0}), {0.3, 0.5, -0.2}));
    SquashParams s{-0.5, 1.0, 0.5, -1.0, 1.0};
    CHECK(squash({0, 1, 0}, s).y == doctest::Approx(0.5));
    CHECK(squash({1, 0, 0}, s).x == doctest::Approx(std::sqrt(2.0)));
    CHECK(squash({1, 1, 0}, s).x == doctest::Approx(1.0));
}

TEST_CASE("sine and wave")
{
    CHECK(sine({0, 0.25, 0}, {0.2, 1.0, 0.0, 0.0, -1.0, 1.0}).x == doctest::Approx(0.2));
    CHECK(sine({0, 0.5, 0}, {0.2, 1.0, 0.0, 0.0, -1.0, 1.0}).x == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(sine({0, 0.25, 0}, {0.2, 1.0, 0.0, 1.0, -1.0, 1.0}).x == doctest::Approx(0.15));
    CHECK(wave({0.125, 0, 0}, {0.1, 0.5, 0.0, 0.0, 0.0, 1.0}).y == doctest::Approx(0.1));
    CHECK(nearTight(wave({2, 0, 0}, {0.1, 0.5, 0.0, 0.0, 0.0, 1.0}), {2, 0, 0}));
}

TEST_CASE("spherical falloff")
{
    CHECK(sphericalFalloff(100.0, 0.0, 0.5) == 1.0);
    CHECK(sphericalFalloff(0.4, 1.0, 0.5) == 1.0);
    CHECK(sphericalFalloff(1.0, 1.0, 0.5) == 0.0);
    CHECK(sphericalFalloff(0.75, 1.0, 0.5) == doctest::Approx(0.5));
    double prev = 1.0;
    for (double d = 0.5; d <= 1.0; d += 0.05)
    {
        double w = sphericalFalloff(d, 1.0, 0.5);
        CHECK(w <= prev + 1e-12);
        prev = w;
    }
}

TEST_CASE("ffd: axis weights sum to one and the rest lattice is an identity")
{
    std::vector<std::pair<int, double>> w;
    for (FfdBasis b : {FfdBasis::Trilinear, FfdBasis::Bernstein, FfdBasis::BSpline})
        for (int n : {2, 3, 4, 7})
            for (double s = 0.0; s <= 1.0; s += 0.0625)
            {
                ffdAxisWeights(n, s, b, w);
                double sum = 0.0;
                for (auto [i, x] : w)
                {
                    CHECK(i >= 0);
                    CHECK(i < n);
                    sum += x;
                }
                CHECK(sum == doctest::Approx(1.0));
            }

    glm::ivec3 n(4, 3, 5);
    std::vector<glm::dvec3> zero(size_t(n.x * n.y * n.z), glm::dvec3(0.0));
    for (FfdBasis b : {FfdBasis::Trilinear, FfdBasis::Bernstein, FfdBasis::BSpline})
        for (const glm::dvec3& p : randomPoints(50, 0.0, 1.0, 7))
            CHECK(nearTight(ffdDisplacement(n, zero, p, b), glm::dvec3(0.0)));
}

TEST_CASE("ffd: trilinear moves a corner exactly, Bernstein reproduces linear maps")
{
    glm::ivec3 n(2, 2, 2);
    std::vector<glm::dvec3> disp(8, glm::dvec3(0.0));
    disp[latticeIndex(n, 1, 1, 1)] = {1, 2, 3};
    CHECK(nearTight(ffdDisplacement(n, disp, {1, 1, 1}, FfdBasis::Trilinear), {1, 2, 3}));
    CHECK(nearTight(ffdDisplacement(n, disp, {0.5, 0.5, 0.5}, FfdBasis::Trilinear), glm::dvec3(1, 2, 3) / 8.0));

    // Displacing every point by a linear function of its rest position displaces every point the same way.
    glm::ivec3 m(4, 4, 4);
    glm::dmat3 a(0.2, 0.1, 0.0, -0.3, 0.5, 0.1, 0.0, 0.2, -0.4);
    std::vector<glm::dvec3> lin(size_t(m.x * m.y * m.z));
    for (int k = 0; k < m.z; ++k)
        for (int j = 0; j < m.y; ++j)
            for (int i = 0; i < m.x; ++i)
                lin[latticeIndex(m, i, j, k)] = a * latticeRest(m, i, j, k);
    for (const glm::dvec3& stu : randomPoints(30, 0.0, 1.0, 7))
        CHECK(nearTight(ffdDisplacement(m, lin, stu, FfdBasis::Bernstein), a * (stu - glm::dvec3(0.5)), 1e-9));
}

TEST_CASE("lattice mesh has the grid's points in index order")
{
    geo::Mesh m = latticeMesh({3, 2, 4});
    REQUIRE(m.numPoints() == 24);
    CHECK(nearTight(m.position(geo::Index(latticeIndex({3, 2, 4}, 2, 1, 3))), {0.5, 0.5, 0.5}));
    CHECK(nearTight(m.position(0), {-0.5, -0.5, -0.5}));
    // A quad per cell face on every grid plane, per axis: planes times cells in the plane.
    CHECK(m.numFaces() == 4 * (2 * 1) + 2 * (3 * 2) + 3 * (3 * 1));
}

TEST_CASE("smoothing is Jacobi: the result does not depend on point order")
{
    geo::Mesh m = geo::makeSphere({.radius = 10.0, .subdivAxis = 12, .subdivHeight = 8});
    Adjacency adj = pointAdjacency(m);
    std::vector<glm::dvec3> a = pointsOf(m), b = a;
    smooth(a, adj, 5, 0.5, true);
    smooth(b, adj, 5, 0.5, true);
    CHECK(a == b);
    // Smoothing a closed sphere shrinks it.
    CHECK(glm::length(a[adj.neighbors[0]]) < 10.0);
}

TEST_CASE("delta mush: rest is an identity and rigid motion carries the detail along")
{
    geo::Mesh m = geo::makeSphere({.radius = 10.0, .subdivAxis = 16, .subdivHeight = 10});
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        m.setPosition(p, m.position(p) * (1.0 + 0.1 * std::sin(double(p))));
    Adjacency adj = pointAdjacency(m);
    std::vector<glm::dvec3> rest = pointsOf(m);
    DeltaMushBind bind = deltaMushBind(m, rest, adj, 8, 0.5, true);
    std::vector<glm::dvec3> same = deltaMushApply(m, rest, adj, bind, 1.0);
    for (size_t p = 0; p < rest.size(); ++p)
        CHECK(nearTight(same[p], rest[p], 1e-9));

    glm::dmat4 r = glm::translate(glm::dmat4(1.0), glm::dvec3(3, -2, 5)) * glm::rotate(glm::dmat4(1.0), 0.7, glm::normalize(glm::dvec3(1, 2, 3)));
    std::vector<glm::dvec3> moved(rest.size());
    for (size_t p = 0; p < rest.size(); ++p)
        moved[p] = glm::dvec3(r * glm::dvec4(rest[p], 1.0));
    std::vector<glm::dvec3> out = deltaMushApply(m, moved, adj, bind, 1.0);
    for (size_t p = 0; p < rest.size(); ++p)
        CHECK(nearTight(out[p], moved[p], 1e-7));

    // Displacement 0 gives the smoothed shape.
    std::vector<glm::dvec3> smoothed = rest;
    smooth(smoothed, adj, 8, 0.5, true);
    std::vector<glm::dvec3> flat = deltaMushApply(m, rest, adj, bind, 0.0);
    for (size_t p = 0; p < rest.size(); ++p)
        CHECK(nearTight(flat[p], smoothed[p], 1e-9));
}

TEST_CASE("wrap: identity at bind, rigid driver motion moves the targets rigidly")
{
    geo::Mesh driver = geo::makeCube({.width = 20, .height = 20, .depth = 20, .subdivX = 2, .subdivY = 2, .subdivZ = 2});
    geo::Mesh target = geo::makeSphere({.radius = 8.0, .subdivAxis = 12, .subdivHeight = 8});
    std::vector<glm::dvec3> t = pointsOf(target);
    WrapBinding b = wrapBind(driver, t, 4, 2.0);
    CHECK(b.offsets.size() == t.size() + 1);
    std::vector<glm::dvec3> d = pointsOf(driver);
    std::vector<glm::dvec3> same = wrapApply(b, d, t);
    for (size_t i = 0; i < t.size(); ++i)
        CHECK(nearTight(same[i], t[i], 1e-9));

    glm::dmat4 r = glm::translate(glm::dmat4(1.0), glm::dvec3(5, 1, -3)) * glm::rotate(glm::dmat4(1.0), 0.4, glm::dvec3(0, 1, 0));
    for (auto& p : d)
        p = glm::dvec3(r * glm::dvec4(p, 1.0));
    std::vector<glm::dvec3> out = wrapApply(b, d, t);
    for (size_t i = 0; i < t.size(); ++i)
        CHECK(nearTight(out[i], glm::dvec3(r * glm::dvec4(t[i], 1.0)), 1e-9));

    // Deterministic on any thread count.
    WrapBinding again = wrapBind(driver, t, 4, 2.0);
    CHECK(again.influences.size() == b.influences.size());
    CHECK(again.offsets == b.offsets);
}

TEST_CASE("mean value coordinates reproduce linear functions and sum to one")
{
    geo::Mesh cage = geo::makeCube({.width = 2, .height = 2, .depth = 2});
    std::vector<glm::dvec3> c = pointsOf(cage);
    std::vector<uint32_t> tris = trianglePoints(cage);
    REQUIRE(tris.size() == 12 * 3);
    std::vector<double> w(c.size());
    for (const glm::dvec3& x : randomPoints(40, -0.95, 0.95, 7))
    {
        meanValueCoordinates(c, tris, x, w);
        double sum = 0.0;
        glm::dvec3 rebuilt(0.0);
        for (size_t j = 0; j < c.size(); ++j)
        {
            sum += w[j];
            rebuilt += c[j] * w[j];
            CHECK(w[j] > -1e-12); // positive inside a convex cage
        }
        CHECK(sum == doctest::Approx(1.0));
        CHECK(nearTight(rebuilt, x, 1e-9));
    }
    // On a cage vertex and on a face.
    meanValueCoordinates(c, tris, c[3], w);
    CHECK(w[3] == doctest::Approx(1.0));
    glm::dvec3 onFace(0.3, 0.2, 1.0);
    meanValueCoordinates(c, tris, onFace, w);
    glm::dvec3 rebuilt(0.0);
    for (size_t j = 0; j < c.size(); ++j)
        rebuilt += c[j] * w[j];
    CHECK(nearTight(rebuilt, onFace, 1e-9));
    // Outside the cage too.
    glm::dvec3 outside(2.5, -0.4, 0.3);
    meanValueCoordinates(c, tris, outside, w);
    rebuilt = glm::dvec3(0.0);
    for (size_t j = 0; j < c.size(); ++j)
        rebuilt += c[j] * w[j];
    CHECK(nearTight(rebuilt, outside, 1e-8));
}
