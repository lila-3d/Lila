#include "TestUtil.h"

#include "geo/Normals.h"
#include "geo/Primitives.h"
#include "geo/RenderMesh.h"
#include "geo/Triangulate.h"

#include <glm/gtc/constants.hpp>

#include <chrono>
#include <cmath>

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
Mesh polygon(std::initializer_list<glm::dvec2> pts)
{
    Mesh m;
    std::vector<Index> ids;
    for (const glm::dvec2& p : pts)
        ids.push_back(m.addPoint({p.x, 0.0, -p.y}));
    m.addFace(ids);
    return m;
}

double triArea(const Mesh& m, const Triangulation& t, Index i, const glm::dvec3& n)
{
    const glm::dvec3 a = m.position(m.cornerPoint(t.corners[i * 3]));
    const glm::dvec3 b = m.position(m.cornerPoint(t.corners[i * 3 + 1]));
    const glm::dvec3 c = m.position(m.cornerPoint(t.corners[i * 3 + 2]));
    return glm::dot(glm::cross(b - a, c - a), n) * 0.5;
}

double seconds(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace

TEST_CASE("triangulate: convex, quads, concave")
{
    Mesh cube = makeCube();
    Triangulation t = triangulate(cube);
    CHECK(t.triangleCount() == 12);

    // L shape, concave at (1,1).
    Mesh l = polygon({{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}});
    t = triangulate(l);
    REQUIRE(t.triangleCount() == 4);
    double total = 0.0;
    for (Index i = 0; i < 4; ++i)
    {
        const double a = triArea(l, t, i, {0, 1, 0});
        CHECK(a > 0.0);
        total += a;
    }
    CHECK(total == doctest::Approx(3.0));

    // Star with 5 reflex points.
    Mesh s;
    std::vector<Index> ids;
    for (int i = 0; i < 10; ++i)
    {
        const double r = i % 2 ? 0.4 : 1.0;
        const double a = glm::two_pi<double>() * i / 10;
        ids.push_back(s.addPoint({r * std::cos(a), r * std::sin(a), 0.0}));
    }
    s.addFace(ids);
    Triangulation ts = triangulate(s);
    REQUIRE(ts.triangleCount() == 8);
    total = 0.0;
    for (Index i = 0; i < 8; ++i)
    {
        const double a = triArea(s, ts, i, {0, 0, 1});
        CHECK(a > 0.0);
        total += a;
    }
    CHECK(total == doctest::Approx(s.faceArea(0)));
    CHECK(triangulate(s).corners == ts.corners);

    // A concave quad must split through its reflex corner.
    Mesh dart = polygon({{0, 0}, {1, 0.3}, {2, 0}, {1, 2}});
    Triangulation td = triangulate(dart);
    for (Index i = 0; i < 2; ++i)
        CHECK(triArea(dart, td, i, {0, 1, 0}) > 0.0);
}

TEST_CASE("normals: hard and smooth")
{
    Mesh cube = makeCube();
    std::vector<glm::vec3> n = cornerNormals(cube);
    for (Index c = 0; c < cube.cornerCapacity(); ++c)
    {
        const glm::dvec3 fn = cube.faceNormal(cube.cornerFace(c));
        CHECK(glm::dot(glm::dvec3(n[c]), fn) == doctest::Approx(1.0));
    }
    setSoftEdges(cube);
    n = cornerNormals(cube);
    for (Index c = 0; c < cube.cornerCapacity(); ++c)
    {
        const glm::dvec3 expect = glm::normalize(cube.position(cube.cornerPoint(c)));
        CHECK(glm::dot(glm::dvec3(n[c]), expect) == doctest::Approx(1.0));
    }

    Mesh sphere = makeSphere({50.0, 32, 16});
    n = cornerNormals(sphere);
    for (Index c = 0; c < sphere.cornerCapacity(); ++c)
    {
        const glm::dvec3 expect = glm::normalize(sphere.position(sphere.cornerPoint(c)));
        CHECK(glm::dot(glm::dvec3(n[c]), expect) > 0.995);
    }

    Mesh cyl = makeCylinder({50.0, 100.0, 24, 2});
    setSoftEdges(cyl);
    setHardEdgesByAngle(cyl, 30.0);
    std::span<const bool> hard = std::as_const(cyl).edgeAttrs().get<bool>(attr::kHard);
    Index hardCount = 0;
    for (Index e = 0; e < cyl.edgeCapacity(); ++e)
        hardCount += hard[e];
    CHECK(hardCount == 48);
}

TEST_CASE("normals: plan matches cornerNormals and holds while only points move")
{
    auto check = [](const Mesh& m, CornerNormalPlan& plan, const CornerNormalOptions& opts) {
        const std::vector<glm::vec3> expect = cornerNormals(m, opts);
        std::vector<glm::vec3> rows(plan.rows());
        plan.compute(m, rows);
        size_t row = 0;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                m.forEachFaceCorner(f, [&](Index c) {
                    CHECK(glm::length(rows[row] - expect[c]) < 1e-5f);
                    ++row;
                });
        CHECK(row == plan.rows());
    };

    Mesh cyl = makeCylinder({50.0, 100.0, 24, 2});
    setSoftEdges(cyl);
    setHardEdgesByAngle(cyl, 30.0);
    Mesh sphere = makeSphere({50.0, 32, 16});
    sphere.removeFace(5);
    for (NormalWeight w : {NormalWeight::Uniform, NormalWeight::Area, NormalWeight::Angle, NormalWeight::AreaAngle})
        for (Mesh* m : {&cyl, &sphere})
        {
            const CornerNormalOptions opts{w};
            CornerNormalPlan plan(*m, opts);
            check(*m, plan, opts);

            Mesh moved = *m;
            CHECK(moved.topologyId() == m->topologyId());
            for (Index p = 0; p < moved.pointCapacity(); ++p)
                moved.setPosition(p, moved.position(p) * glm::dvec3(1.0, 1.5, 0.5) + glm::dvec3(0.0, 0.01 * double(p % 7), 0.0));
            REQUIRE(plan.fits(moved));
            check(moved, plan, opts);
        }

    Mesh changed = cyl;
    CHECK(CornerNormalPlan(cyl).fits(changed));
    setSoftEdges(changed);
    CHECK_FALSE(CornerNormalPlan(cyl).fits(changed));
    changed = cyl;
    changed.removeFace(0);
    CHECK(changed.topologyId() != cyl.topologyId());
    CHECK_FALSE(CornerNormalPlan(cyl).fits(changed));
}

TEST_CASE("normals: mikktspace tangents")
{
    Mesh plane = makePlane();
    REQUIRE(updateTangents(plane));
    std::span<const glm::vec4> t = std::as_const(plane).cornerAttrs().get<glm::vec4>(attr::kTangent);
    for (Index c = 0; c < plane.cornerCapacity(); ++c)
    {
        CHECK(t[c].x == doctest::Approx(1.0f));
        CHECK(t[c].w == doctest::Approx(1.0f));
    }
    Mesh sphere = makeSphere();
    REQUIRE(updateTangents(sphere));
    std::span<const glm::vec4> ts = std::as_const(sphere).cornerAttrs().get<glm::vec4>(attr::kTangent);
    std::vector<glm::vec3> cn = cornerNormals(sphere);
    for (Index c = 0; c < sphere.cornerCapacity(); ++c)
        CHECK(std::abs(glm::dot(glm::vec3(ts[c]), cn[c])) < 1e-3f);
}

TEST_CASE("render mesh: splits")
{
    Mesh cube = makeCube();
    RenderMesh r = buildRenderMesh(cube);
    CHECK(r.vertexCount() == 24);
    CHECK(r.triangleCount() == 12);
    CHECK(r.lineIndices.size() == 24);
    CHECK(r.sections.size() == 1);
    for (Index i : r.indices)
        CHECK(i < r.vertexCount());

    // Soft cube: uv seams still split, normals no longer do.
    setSoftEdges(cube);
    r = buildRenderMesh(cube);
    CHECK(r.vertexCount() == 14);

    Mesh sphere = makeSphere({50.0, 8, 4});
    RenderMeshOptions ro;
    ro.tangents = true;
    r = buildRenderMesh(sphere, ro);
    // Rings duplicate one point on the uv seam; each pole splits per triangle.
    CHECK(r.vertexCount() == 3 * 9 + 2 * 8);
    CHECK(r.tangents.size() == r.vertexCount());

    Mesh two = makeCube();
    std::span<int32_t> mat = two.faceAttrs().getOrAdd<int32_t>(attr::kMaterial);
    mat[1] = 2;
    mat[3] = 2;
    r = buildRenderMesh(two);
    REQUIRE(r.sections.size() == 2);
    CHECK(r.sections[0].material == 0);
    CHECK(r.sections[0].indexCount == 24);
    CHECK(r.sections[1].material == 2);
    CHECK(r.sections[1].firstIndex == 24);
    CHECK(std::as_const(two).faceAttrs().get<int32_t>(attr::kMaterial)[r.triangleFaces[8]] == 2);
}

TEST_CASE("perf: 1M faces")
{
    auto t0 = std::chrono::steady_clock::now();
    Mesh m = makePlane({1000.0, 1000.0, 1000, 1000});
    const double tBuild = seconds(t0);
    REQUIRE(m.numFaces() == 1000000);
    // Bend it so normals are not trivial.
    for (glm::dvec3& p : m.positions())
        p.y = 10.0 * std::sin(p.x * 0.05) * std::cos(p.z * 0.05);

    t0 = std::chrono::steady_clock::now();
    std::vector<glm::dvec3> fn = faceNormals(m);
    const double tFace = seconds(t0);

    t0 = std::chrono::steady_clock::now();
    std::vector<glm::vec3> cn = cornerNormals(m);
    const double tCorner = seconds(t0);

    t0 = std::chrono::steady_clock::now();
    Triangulation tris = triangulate(m);
    const double tTri = seconds(t0);
    CHECK(tris.triangleCount() == 2000000);

    t0 = std::chrono::steady_clock::now();
    std::vector<glm::vec4> tan = cornerTangents(m, tris, cn);
    const double tTan = seconds(t0);

    t0 = std::chrono::steady_clock::now();
    RenderMesh r = buildRenderMesh(m);
    const double tRender = seconds(t0);
    CHECK(r.vertexCount() == m.numPoints());

    t0 = std::chrono::steady_clock::now();
    Mesh copy = m;
    const double tCopy = seconds(t0);

    MESSAGE("1M quads: build " << tBuild << "s, face normals " << tFace << "s, corner normals " << tCorner
                               << "s, triangulate " << tTri << "s, mikktspace " << tTan << "s, render build "
                               << tRender << "s, copy " << tCopy << "s");
}

TEST_CASE("normals: parallel tangents match one call")
{
    Mesh one = makeSphere({50.0, 24, 16});
    Mesh many;
    for (int i = 0; i < 200; ++i)
    {
        Mesh s = one;
        for (glm::dvec3& p : s.positions())
            p.x += 150.0 * i;
        many.append(s);
    }
    const Triangulation tm = triangulate(many);
    REQUIRE(tm.triangleCount() > (1u << 17));
    const std::vector<glm::vec3> n = cornerNormals(many);
    const std::vector<glm::vec4> par = cornerTangents(many, tm, n);
    const std::vector<glm::vec4> one1 = cornerTangents(many, tm, n, "uv0", false);
    CHECK(par == one1);
}

TEST_CASE("normals: parallel tangents on one connected mesh match one call")
{
    Mesh m = makePlane({100.0, 100.0, 300, 300});
    // A second plane edge to edge, its own points on the same positions, so MikkTSpace welds across the seam.
    Mesh side = makePlane({100.0, 100.0, 300, 300});
    for (glm::dvec3& p : side.positions())
        p.x += 100.0;
    m.append(side);
    for (glm::dvec3& p : m.positions())
        p.y = 3.0 * std::sin(p.x * 0.2) * std::cos(p.z * 0.3);
    std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uv.empty());
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        uv[c] *= glm::vec2(1.0f, c % 7 == 0 ? -1.0f : 1.0f); // mirrored corners: mixed bitangent signs

    const Triangulation tris = triangulate(m);
    REQUIRE(tris.triangleCount() > (1u << 17));
    const std::vector<glm::vec3> n = cornerNormals(m);
    const std::vector<glm::vec4> par = cornerTangents(m, tris, n);
    const std::vector<glm::vec4> one = cornerTangents(m, tris, n, "uv0", false);
    REQUIRE(par.size() == one.size());
    size_t differ = 0;
    for (size_t c = 0; c < par.size(); ++c)
        differ += par[c] != one[c];
    CHECK(differ == 0);
}
