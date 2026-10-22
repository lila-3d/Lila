#include "TestUtil.h"

#include "geo/Primitives.h"

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
void checkClosedPrimitive(const Mesh& m, int expectedEuler = 2)
{
    CHECK(isValid(m));
    CHECK(boundaryEdgeCount(m) == 0);
    CHECK(euler(m) == expectedEuler);
    CHECK(signedVolume(m) > 0.0);
    CHECK(uvsDontOverlap(m));
}
} // namespace

TEST_CASE("primitives: cube")
{
    Mesh m = makeCube();
    CHECK(m.numPoints() == 8);
    CHECK(m.numFaces() == 6);
    checkClosedPrimitive(m);
    CHECK(signedVolume(m) == doctest::Approx(1e6));
    std::span<const bool> hard = std::as_const(m).edgeAttrs().get<bool>(attr::kHard);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        CHECK(hard[e]);

    Mesh s = makeCube({200.0, 100.0, 50.0, 3, 2, 4, true});
    checkClosedPrimitive(s);
    CHECK(s.numFaces() == 2 * (3 * 2 + 2 * 4 + 3 * 4));
    CHECK(s.bounds().min.y == doctest::Approx(0.0));
    CHECK(s.bounds().size().x == doctest::Approx(200.0));
    Index hardCount = 0;
    std::span<const bool> h2 = std::as_const(s).edgeAttrs().get<bool>(attr::kHard);
    for (Index e = 0; e < s.edgeCapacity(); ++e)
        hardCount += h2[e];
    CHECK(hardCount == 4 * (3 + 2 + 4));
}

TEST_CASE("primitives: plane and disc")
{
    Mesh p = makePlane({100.0, 50.0, 4, 2});
    CHECK(isValid(p));
    CHECK(p.numFaces() == 8);
    CHECK(p.faceNormal(0).y == doctest::Approx(1.0));
    CHECK(uvsDontOverlap(p));
    CHECK(boundaryEdgeCount(p) == 12);

    Mesh d = makeDisc();
    CHECK(isValid(d));
    CHECK(d.faceNormal(0).y == doctest::Approx(1.0));
    CHECK(uvsDontOverlap(d));
    Mesh n = makeDisc({50.0, 12, 0});
    CHECK(isValid(n));
    CHECK(n.numFaces() == 1);
    CHECK(n.faceSize(0) == 12);
    CHECK(n.faceNormal(0).y == doctest::Approx(1.0));
}

TEST_CASE("primitives: round")
{
    Mesh sphere = makeSphere();
    checkClosedPrimitive(sphere);
    CHECK(sphere.numPoints() == 20 * 19 + 2);
    CHECK(sphere.numFaces() == 400);
    CHECK(sphere.bounds().size().y == doctest::Approx(100.0));

    Mesh cyl = makeCylinder();
    checkClosedPrimitive(cyl);
    CHECK(cyl.numFaces() == 22);
    Mesh cyl2 = makeCylinder({50.0, 100.0, 16, 3, 2, true, true});
    checkClosedPrimitive(cyl2);
    CHECK(cyl2.bounds().min.y == doctest::Approx(0.0));

    Mesh cone = makeCone();
    checkClosedPrimitive(cone);
    Mesh cone2 = makeCone({50.0, 100.0, 12, 2, 2});
    checkClosedPrimitive(cone2);

    Mesh torus = makeTorus();
    checkClosedPrimitive(torus, 0);
    CHECK(torus.numFaces() == 400);

    Mesh pipe = makePipe();
    checkClosedPrimitive(pipe, 0);

    Mesh qs = makeQuadSphere();
    checkClosedPrimitive(qs);
    CHECK(qs.numFaces() == 6 * 64);
    for (Index p = 0; p < qs.pointCapacity(); ++p)
        CHECK(glm::length(qs.position(p)) == doctest::Approx(50.0));
}

TEST_CASE("primitives: overlap check catches overlap")
{
    Mesh p = makePlane({100.0, 100.0, 2, 1});
    std::span<glm::vec2> uv = p.cornerAttrs().get<glm::vec2>("uv0");
    for (Index c = 0; c < p.cornerCapacity(); ++c)
        uv[c] = glm::vec2(c % 4 == 1 || c % 4 == 2 ? 1.0f : 0.0f, c % 4 >= 2 ? 1.0f : 0.0f);
    CHECK_FALSE(uvsDontOverlap(p));
}
