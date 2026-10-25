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

/** An open dome: valid, outward, one flat boundary loop at height y, every boundary point on exactly two border edges. */
void checkDome(const Mesh& m, double y)
{
    CHECK(isValid(m, kValidateDefault | kValidateDegenerate));
    CHECK(uvsDontOverlap(m));
    CHECK(boundaryEdgeCount(m) > 0);
    std::vector<int> border(m.pointCapacity(), 0);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
        {
            const Index c = m.edgeCorner(e);
            ++border[m.cornerPoint(c)];
            ++border[m.cornerNextPoint(c)];
        }
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && border[p])
        {
            CHECK(border[p] == 2);
            CHECK(m.position(p).y == doctest::Approx(y).epsilon(1e-9).scale(100.0));
        }
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            CHECK(glm::dot(m.faceNormal(f), m.faceCentroid(f)) > 0.0);
    CHECK(m.bounds().min.y == doctest::Approx(y).scale(100.0));
}

bool allQuads(const Mesh& m)
{
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceSize(f) != 4)
            return false;
    return true;
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
    CHECK(cyl.numFaces() == 20 + 2 * (5 * 5 + 20));
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
    CHECK(qs.numFaces() == 6 * 9);
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

TEST_CASE("primitives: quad sphere fraction")
{
    QuadSphereOptions o;
    o.subdiv = 4;
    o.fraction = 0.5;
    Mesh half = makeQuadSphere(o);
    checkDome(half, 0.0);
    CHECK(allQuads(half));
    CHECK(half.numFaces() == 3 * 16);
    // At one half the cut follows the full sphere's equator, so the points are the full sphere's upper ones.
    o.fraction = 1.0;
    Mesh full = makeQuadSphere(o);
    Index upper = 0;
    for (Index p = 0; p < full.pointCapacity(); ++p)
        upper += full.position(p).y > -1e-9;
    CHECK(half.numPoints() == upper);

    for (int n : {1, 3, 8})
        for (double f : {0.05, 0.2, 0.5, 0.7, 0.85, 0.97})
        {
            CAPTURE(n);
            CAPTURE(f);
            QuadSphereOptions q;
            q.subdiv = n;
            q.fraction = f;
            Mesh m = makeQuadSphere(q);
            checkDome(m, 50.0 * (1.0 - 2.0 * f));
            CHECK(allQuads(m));
            for (Index p = 0; p < m.pointCapacity(); ++p)
                CHECK(glm::length(m.position(p)) == doctest::Approx(50.0));
        }
}

TEST_CASE("primitives: sphere and ico sphere fraction")
{
    SphereOptions s;
    s.fraction = 0.5;
    Mesh hemi = makeSphere(s);
    checkDome(hemi, 0.0);
    CHECK(hemi.numFaces() == 20 * 10);
    s.baseAtOrigin = true;
    s.fraction = 0.25;
    Mesh cap = makeSphere(s);
    checkDome(cap, 0.0);
    CHECK(cap.bounds().max.y == doctest::Approx(25.0));

    for (int level : {1, 2, 3, 4})
        for (double f : {0.1, 0.3, 0.5, 0.77})
        {
            CAPTURE(level);
            CAPTURE(f);
            IcoSphereOptions o;
            o.subdivisions = level;
            o.fraction = f;
            Mesh m = makeIcoSphere(o);
            checkDome(m, 50.0 * (1.0 - 2.0 * f));
            for (Index p = 0; p < m.pointCapacity(); ++p)
                CHECK(glm::length(m.position(p)) == doctest::Approx(50.0));
        }
    IcoSphereOptions based;
    based.fraction = 0.5;
    based.baseAtOrigin = true;
    CHECK(makeIcoSphere(based).bounds().min.y == doctest::Approx(0.0).scale(100.0));
}

TEST_CASE("primitives: cap fills")
{
    auto sides = [](const Mesh& m, int n) {
        int count = 0;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            count += m.faceValid(f) && int(m.faceSize(f)) == n;
        return count;
    };
    for (int axis : {3, 4, 5, 6, 7, 12, 20, 21, 32})
        for (int rings : {0, 1, 3})
        {
            CAPTURE(axis);
            CAPTURE(rings);
            CylinderOptions o{.subdivAxis = axis, .subdivHeight = 2, .subdivCaps = rings};
            Mesh quads = makeCylinder(o);
            checkClosedPrimitive(quads);
            CHECK(isValid(quads, kValidateDefault | kValidateDegenerate));
            const int border = axis - axis % 2, a = border / 4, c = border / 2 - a;
            const int extra = 2 * (std::max(1, rings) - 1) * axis;
            if (axis >= 4)
            {
                CHECK(quads.numFaces() == 2 * axis + 2 * (a * c + border + axis % 2) + extra);
                CHECK(sides(quads, 3) == 2 * (axis % 2));
                CHECK(sides(quads, 4) == int(quads.numFaces()) - 2 * (axis % 2));
            }
            else
                CHECK(quads.numFaces() == 2 * axis + 2 + extra);

            o.capFill = CapFill::Fan;
            Mesh fan = makeCylinder(o);
            checkClosedPrimitive(fan);
            CHECK(fan.numFaces() == 2 * axis + 2 * axis + extra);
            o.capFill = CapFill::Ngon;
            Mesh ngon = makeCylinder(o);
            checkClosedPrimitive(ngon);
            CHECK(ngon.numFaces() == 2 * axis + 2 + extra);
            o.capFill = CapFill::None;
            Mesh open = makeCylinder(o);
            CHECK(isValid(open));
            CHECK(open.numFaces() == 2 * axis);
            CHECK(boundaryEdgeCount(open) == 2 * axis);

            ConeOptions k{.subdivAxis = axis, .subdivCap = rings};
            Mesh cone = makeCone(k);
            checkClosedPrimitive(cone);
            if (axis >= 4)
                CHECK(sides(cone, 3) == axis + axis % 2);
            k.capFill = CapFill::None;
            Mesh openCone = makeCone(k);
            CHECK(isValid(openCone));
            CHECK(boundaryEdgeCount(openCone) == axis);
        }
    // Quad caps are flat, inside the rim, and their uvs stay in their island.
    Mesh m = makeCylinder({.radius = 30.0, .height = 80.0, .subdivAxis = 24});
    std::span<const glm::vec2> uv = std::as_const(m).cornerAttrs().get<glm::vec2>(attr::uvName(0));
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && std::abs(m.faceNormal(f).y) > 0.5)
            m.forEachFaceCorner(f, [&](Index c) {
                const glm::dvec3 p = m.position(m.cornerPoint(c));
                CHECK(std::abs(std::abs(p.y) - 40.0) < 1e-9);
                CHECK(std::hypot(p.x, p.z) <= 30.0 + 1e-9);
                CHECK(uv[c].y < 0.5f);
                CHECK(uv[c].x >= 0.0f);
                CHECK(uv[c].x <= 1.0f);
            });
    // Wedges pair their fan into quads.
    Mesh wedge = makeCylinder({.subdivAxis = 9, .fraction = 0.5});
    checkClosedPrimitive(wedge);
    CHECK(sides(wedge, 3) == 2);
}

TEST_CASE("primitives: partial sweeps")
{
    for (double f : {0.25, 0.5, 0.8})
    {
        CAPTURE(f);
        CylinderOptions c;
        c.fraction = f;
        Mesh cyl = makeCylinder(c);
        checkClosedPrimitive(cyl);
        CHECK(signedVolume(cyl) < signedVolume(makeCylinder()));
        c.subdivCaps = 2;
        c.subdivHeight = 3;
        checkClosedPrimitive(makeCylinder(c));
        c.cutCaps = false;
        Mesh open = makeCylinder(c);
        CHECK(isValid(open));
        CHECK(boundaryEdgeCount(open) > 0);
        CHECK(uvsDontOverlap(open));

        ConeOptions k;
        k.fraction = f;
        checkClosedPrimitive(makeCone(k));

        PipeOptions p;
        p.fraction = f;
        p.subdivHeight = 2;
        checkClosedPrimitive(makePipe(p));

        TorusOptions t;
        t.fraction = f;
        checkClosedPrimitive(makeTorus(t));
        t.cutCaps = false;
        Mesh tube = makeTorus(t);
        CHECK(isValid(tube));
        CHECK(boundaryEdgeCount(tube) == 2 * 20);

        DiscOptions d;
        d.fraction = f;
        for (int rings : {0, 1, 3})
        {
            d.subdivRings = rings;
            Mesh disc = makeDisc(d);
            CHECK(isValid(disc));
            CHECK(uvsDontOverlap(disc));
            for (Index fc = 0; fc < disc.faceCapacity(); ++fc)
                CHECK(disc.faceNormal(fc).y == doctest::Approx(1.0));
        }
    }
    // Half a cylinder is cut through its axis, so the cut faces lie in the XY plane.
    CylinderOptions half;
    half.fraction = 0.5;
    Mesh h = makeCylinder(half);
    CHECK(h.bounds().max.z == doctest::Approx(0.0).scale(100.0));
    CHECK(h.bounds().min.z == doctest::Approx(-50.0));
}
