#include <doctest/doctest.h>

#include "ops/Command.h"
#include "deform/Deform.h"
#include "deform/Surface.h"
#include "geo/Primitives.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <random>

using namespace lila;
using namespace lila::deform;
using json = nlohmann::json;

namespace
{
bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-9) { return glm::length(a - b) <= eps; }

std::vector<glm::dvec3> randomPoints(size_t n, double lo, double hi, unsigned seed = 11)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> d(lo, hi);
    std::vector<glm::dvec3> out(n);
    for (auto& p : out)
        p = {d(rng), d(rng), d(rng)};
    return out;
}

std::vector<glm::dvec3> pointsOf(const geo::Mesh& m) { return {m.positions().begin(), m.positions().end()}; }

glm::dvec3 apply(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

std::vector<glm::dvec3> transformed(const glm::dmat4& m, std::span<const glm::dvec3> pts)
{
    std::vector<glm::dvec3> out;
    for (const glm::dvec3& p : pts)
        out.push_back(apply(m, p));
    return out;
}

geo::Mesh moved(const geo::Mesh& m, const glm::dmat4& x)
{
    geo::Mesh out = m;
    for (geo::Index p = 0; p < out.pointCapacity(); ++p)
        out.setPosition(p, apply(x, m.position(p)));
    return out;
}

glm::dmat4 someRigid()
{
    glm::dmat4 m = glm::translate(glm::dmat4(1.0), glm::dvec3(3.0, -7.0, 12.5));
    m = glm::rotate(m, 0.7, glm::normalize(glm::dvec3(1.0, 2.0, -0.5)));
    return m;
}

geo::Mesh grid(int n, double size) { return geo::makePlane({.width = size, .depth = size, .subdivX = n, .subdivZ = n}); }
} // namespace

TEST_CASE("rigidFit recovers a rotation and translation exactly and never reflects")
{
    std::vector<glm::dvec3> rest = randomPoints(40, -5.0, 5.0);
    glm::dmat4 m = someRigid();
    RigidFit f = rigidFit(rest, transformed(m, rest));
    for (const glm::dvec3& p : rest)
        CHECK(near(f.apply(p), apply(m, p), 1e-10));
    CHECK(glm::determinant(f.rotation) == doctest::Approx(1.0));

    std::vector<glm::dvec3> mirrored = rest;
    for (glm::dvec3& p : mirrored)
        p.x = -p.x;
    CHECK(glm::determinant(rigidFit(rest, mirrored).rotation) == doctest::Approx(1.0));
}

TEST_CASE("PointGrid nearest and within agree with brute force")
{
    std::vector<glm::dvec3> pts = randomPoints(500, -10.0, 10.0, 3);
    PointGrid grid(pts);
    for (const glm::dvec3& q : randomPoints(50, -14.0, 14.0, 5))
    {
        std::vector<std::pair<double, uint32_t>> brute;
        for (uint32_t i = 0; i < pts.size(); ++i)
            brute.push_back({glm::length(pts[i] - q), i});
        std::sort(brute.begin(), brute.end());
        std::vector<std::pair<double, uint32_t>> got;
        grid.nearest(q, 6, got);
        REQUIRE(got.size() == 6);
        for (int k = 0; k < 6; ++k)
            CHECK(got[size_t(k)].second == brute[size_t(k)].second);
        std::vector<uint32_t> in;
        grid.within(q, 3.0, in);
        size_t count = std::count_if(brute.begin(), brute.end(), [](auto& b) { return b.first <= 3.0; });
        CHECK(in.size() == count);
    }
}

TEST_CASE("shrinkwrap puts points on the target, or outside it by the offset")
{
    geo::Mesh box = geo::makeCube({.width = 20, .height = 20, .depth = 20});
    SurfaceTarget target(box);
    REQUIRE(target.closed());
    geo::Mesh mesh = geo::makeSphere({.radius = 6.0});
    Adjacency adj = pointAdjacency(mesh);
    std::vector<glm::dvec3> pts = pointsOf(mesh);

    ShrinkwrapParams o;
    std::vector<glm::dvec3> on = shrinkwrap(mesh, adj, pts, target, o);
    for (const glm::dvec3& p : on)
    {
        SurfaceTarget::Hit h;
        REQUIRE(target.closest(p, h));
        CHECK(h.distance < 1e-9);
    }
    // Points already on the target stay.
    CHECK(near(shrinkwrap(mesh, adj, on, target, o)[0], on[0]));

    o.place = ShrinkwrapPlace::Outside;
    o.offset = 0.5;
    for (const glm::dvec3& p : shrinkwrap(mesh, adj, pts, target, o))
    {
        SurfaceTarget::Hit h;
        REQUIRE(target.closest(p, h));
        CHECK(h.distance == doctest::Approx(0.5));
        CHECK_FALSE(target.inside(p));
    }

    // Projecting down -Y from inside the box lands on its bottom face.
    o = {};
    o.mode = ShrinkwrapMode::Project;
    o.axis = {0, -1, 0};
    for (const glm::dvec3& p : shrinkwrap(mesh, adj, pts, target, o))
        CHECK(p.y == doctest::Approx(-10.0));
    o.cull = CullFaces::Back;
    std::vector<glm::dvec3> culled = shrinkwrap(mesh, adj, pts, target, o);
    // From inside, the bottom face faces away from a downward ray, so culling it leaves the points where they were.
    CHECK(near(culled[0], pts[0]));

    o = {};
    o.mode = ShrinkwrapMode::NearestVertex;
    for (const glm::dvec3& p : shrinkwrap(mesh, adj, pts, target, o))
        CHECK(std::abs(std::abs(p.x) - 10.0) + std::abs(std::abs(p.y) - 10.0) + std::abs(std::abs(p.z) - 10.0) < 1e-9);
}

TEST_CASE("shrinkwrap keep detail carries a bump onto the target")
{
    geo::Mesh plane = grid(20, 20.0);
    std::vector<glm::dvec3> pts = pointsOf(plane);
    for (glm::dvec3& p : pts)
        p.y = 3.0 + std::exp(-(p.x * p.x + p.z * p.z) / 2.0);
    SurfaceTarget target(grid(4, 100.0));
    Adjacency adj = pointAdjacency(plane);
    ShrinkwrapParams o;
    o.smoothIterations = 30;
    std::vector<glm::dvec3> flat = shrinkwrap(plane, adj, pts, target, o);
    o.keepDetail = true;
    std::vector<glm::dvec3> kept = shrinkwrap(plane, adj, pts, target, o);
    double flatTop = 0.0, keptTop = 0.0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        flatTop = std::max(flatTop, flat[i].y);
        keptTop = std::max(keptTop, kept[i].y);
    }
    CHECK(flatTop < 1e-9);
    CHECK(keptTop > 0.5);
}

TEST_CASE("surfaceDeform reproduces the driver's rigid motion exactly, by polygons and by points")
{
    geo::Mesh driver = geo::makeSphere({.radius = 10.0});
    std::vector<glm::dvec3> targets = randomPoints(200, -12.0, 12.0);
    glm::dmat4 m = someRigid();
    std::vector<glm::dvec3> drv = pointsOf(driver), drvMoved = transformed(m, drv);

    SurfaceBinding b = surfaceBind(driver, targets, 4, 4.0);
    std::vector<glm::dvec3> rest = surfaceApply(b, drv, targets), out = surfaceApply(b, drvMoved, targets);
    for (size_t i = 0; i < targets.size(); ++i)
    {
        CHECK(near(rest[i], targets[i]));
        CHECK(near(out[i], apply(m, targets[i])));
    }

    PointCapture c = pointCapture(drv, targets, 8, 0.0, 8);
    rest = pointDeform(c, drv, targets);
    out = pointDeform(c, drvMoved, targets);
    for (size_t i = 0; i < targets.size(); ++i)
    {
        CHECK(near(rest[i], targets[i]));
        CHECK(near(out[i], apply(m, targets[i])));
    }
}

TEST_CASE("surfaceDeform carries its binding to a new driver topology by uv")
{
    geo::Mesh coarse = grid(4, 40.0), fine = grid(8, 40.0);
    std::vector<glm::dvec3> targets = randomPoints(100, -15.0, 15.0, 9);
    SurfaceBinding b = surfaceBind(coarse, targets, 2, 4.0);
    REQUIRE(b.hasUv);
    SurfaceBinding again = surfaceRebindByUv(b, fine);
    CHECK(again.driverPoints == fine.pointCapacity());
    std::vector<glm::dvec3> rest = surfaceApply(again, pointsOf(fine), targets);
    for (size_t i = 0; i < targets.size(); ++i)
        CHECK(near(rest[i], targets[i], 1e-8));
    glm::dmat4 up = glm::translate(glm::dmat4(1.0), glm::dvec3(0, 5, 0));
    std::vector<glm::dvec3> out = surfaceApply(again, transformed(up, pointsOf(fine)), targets);
    CHECK(near(out[7], targets[7] + glm::dvec3(0, 5, 0), 1e-8));
}

TEST_CASE("proximity wrap: rigid, scale compensation, snap and several drivers")
{
    geo::Mesh a = geo::makeCube({.width = 10, .height = 10, .depth = 10});
    geo::Mesh b = moved(a, glm::translate(glm::dmat4(1.0), glm::dvec3(40, 0, 0)));
    std::vector<glm::dvec3> targets = randomPoints(60, -6.0, 6.0, 2);
    std::vector<glm::dvec3> nearB = transformed(glm::translate(glm::dmat4(1.0), glm::dvec3(40, 0, 0)), randomPoints(60, -6.0, 6.0, 4));
    targets.insert(targets.end(), nearB.begin(), nearB.end());
    const geo::Mesh* drivers[] = {&a, &b};
    ProximityBinding bind = proximityBind(drivers, targets, {});
    REQUIRE(bind.driverPoints.size() == 2);
    std::vector<glm::dvec3> pa = pointsOf(a), pb = pointsOf(b);

    std::span<const glm::dvec3> rest[] = {pa, pb};
    std::vector<glm::dvec3> same = proximityApply(bind, rest, targets, WrapMode::Offset, 0.0);
    for (size_t i = 0; i < targets.size(); ++i)
        CHECK(near(same[i], targets[i]));

    // Moving one driver moves only the points bound to it.
    glm::dmat4 m = someRigid();
    std::vector<glm::dvec3> pbMoved = transformed(m, pb);
    std::span<const glm::dvec3> one[] = {pa, pbMoved};
    std::vector<glm::dvec3> out = proximityApply(bind, one, targets, WrapMode::Offset, 0.0);
    for (size_t i = 0; i < 60; ++i)
        CHECK(near(out[i], targets[i]));
    for (size_t i = 60; i < targets.size(); ++i)
        CHECK(near(out[i], apply(m, targets[i])));

    // Scaling a driver about the origin scales its points with full compensation; without it the offsets keep their size.
    glm::dmat4 s = glm::scale(glm::dmat4(1.0), glm::dvec3(2.0));
    std::vector<glm::dvec3> paScaled = transformed(s, pa);
    std::span<const glm::dvec3> scaled[] = {paScaled, pb};
    out = proximityApply(bind, scaled, targets, WrapMode::Offset, 1.0);
    for (size_t i = 0; i < 60; ++i)
        CHECK(near(out[i], targets[i] * 2.0));

    std::vector<glm::dvec3> pbUp = transformed(glm::translate(glm::dmat4(1.0), glm::dvec3(0, 0, 3)), pb);
    std::span<const glm::dvec3> up[] = {pa, pbUp};
    out = proximityApply(bind, up, targets, WrapMode::Surface, 0.0);
    CHECK(near(out[0], targets[0]));
    CHECK(near(out[60], targets[60] + glm::dvec3(0, 0, 3)));
    ProximityBinding single = proximityBind(std::span<const geo::Mesh* const>(drivers, 1), std::span<const glm::dvec3>(targets.data(), 1), {.maxDrivers = 1});
    std::span<const glm::dvec3> onlyA[] = {pa};
    out = proximityApply(single, onlyA, std::span<const glm::dvec3>(targets.data(), 1), WrapMode::Snap, 0.0);
    SurfaceTarget ta(a);
    SurfaceTarget::Hit h;
    REQUIRE(ta.closest(out[0], h));
    CHECK(h.distance < 1e-9);

    // Smoothed influences still reproduce the rest and rigid motion exactly.
    geo::Mesh sphere = geo::makeSphere({.radius = 5.5});
    Adjacency adj = pointAdjacency(sphere);
    ProximityBinding smooth = proximityBind(std::span<const geo::Mesh* const>(drivers, 1), pointsOf(sphere), {.smoothIterations = 3}, &adj);
    std::vector<glm::dvec3> paMoved = transformed(m, pa);
    std::span<const glm::dvec3> movedA[] = {paMoved};
    std::vector<glm::dvec3> sp = pointsOf(sphere);
    out = proximityApply(smooth, movedA, sp, WrapMode::Offset, 0.0);
    for (size_t i = 0; i < sp.size(); ++i)
        CHECK(near(out[i], apply(m, sp[i]), 1e-8));
}

TEST_CASE("collision leaves no point inside a collider")
{
    geo::Mesh ball = geo::makeSphere({.radius = 5.0});
    SurfaceTarget collider(ball);
    REQUIRE(collider.closed());
    geo::Mesh sheet = grid(30, 20.0);
    std::vector<glm::dvec3> pts = pointsOf(sheet);
    const SurfaceTarget* meshes[] = {&collider};
    CollisionParams o;
    o.offset = 0.25;
    std::vector<glm::dvec3> out = collide(sheet, pts, meshes, {}, o);
    size_t pushed = 0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        CHECK_FALSE(collider.inside(out[i]));
        SurfaceTarget::Hit h;
        REQUIRE(collider.closest(out[i], h));
        CHECK(h.distance >= 0.25 - 1e-9);
        pushed += near(out[i], pts[i]) ? 0 : 1;
    }
    CHECK(pushed > 0);

    // Volume keep bulges the points around the contact, still never inside.
    o.volumeKeep = 1.0;
    std::vector<glm::dvec3> bulged = collide(sheet, pts, meshes, {}, o);
    for (const glm::dvec3& p : bulged)
        CHECK_FALSE(collider.inside(p));

    // A capsule along Y: points inside end up exactly at the radius plus offset; points clear of the falloff stay.
    Capsule cap{{0, -5, 0}, {0, 5, 0}, 2.0};
    o = {};
    o.offset = 0.5;
    o.falloff = 0.5;
    std::vector<glm::dvec3> probe = {{1, 0, 0}, {0, 3, 0.2}, {4, 0, 0}};
    out = collide(sheet, probe, {}, std::span<const Capsule>(&cap, 1), o);
    CHECK(glm::length(glm::dvec2(out[0].x, out[0].z)) == doctest::Approx(2.5));
    CHECK(glm::length(glm::dvec2(out[1].x, out[1].z)) >= 2.5 - 1e-9);
    CHECK(near(out[2], probe[2]));
}

TEST_CASE("rigidify regions match a rigid transform")
{
    geo::Mesh m = geo::makeCube({.width = 4, .height = 4, .depth = 4, .subdivX = 3, .subdivY = 3, .subdivZ = 3});
    geo::Mesh other = moved(m, glm::translate(glm::dmat4(1.0), glm::dvec3(10, 0, 0)));
    m.append(other);
    Adjacency adj = pointAdjacency(m);
    std::vector<int32_t> regions = connectedRegions(adj);
    CHECK(*std::max_element(regions.begin(), regions.end()) == 1);
    std::vector<glm::dvec3> rest = pointsOf(m), deformed = rest;
    for (glm::dvec3& p : deformed)
        p += glm::dvec3(std::sin(p.x), 0.3 * p.y * p.x, std::cos(p.z * 0.5));
    std::vector<glm::dvec3> out = rigidify(adj, rest, deformed, regions, 0);
    for (size_t i = 0; i < rest.size(); ++i)
        for (size_t j = i + 1; j < rest.size(); j += 7)
            if (regions[i] == regions[j])
                CHECK(glm::length(out[i] - out[j]) == doctest::Approx(glm::length(rest[i] - rest[j])).epsilon(1e-12));
    CHECK(near(rigidify(adj, rest, rest, regions, 2)[5], rest[5], 1e-12));
}

TEST_CASE("skinSlide keeps points on the rest surface and is identity at rest")
{
    geo::Mesh plane = grid(12, 24.0);
    std::vector<glm::dvec3> rest = pointsOf(plane);
    SurfaceTarget surface(plane);
    Adjacency adj = pointAdjacency(plane);
    std::vector<glm::dvec3> same = skinSlide(adj, rest, surface, rest, {});
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(near(same[i], rest[i]));
    std::vector<glm::dvec3> pulled = rest;
    for (glm::dvec3& p : pulled)
        p += glm::dvec3(0.0, 2.0 * std::exp(-(p.x * p.x + p.z * p.z) / 20.0), 0.5);
    std::vector<glm::dvec3> out = skinSlide(adj, rest, surface, pulled, {});
    for (const glm::dvec3& p : out)
        CHECK(std::abs(p.y) < 1e-9);
    // Interior points carry the slide along the surface.
    CHECK(out[6 * 13 + 6].z - rest[6 * 13 + 6].z == doctest::Approx(0.5).epsilon(1e-6));
    SlideParams keep;
    keep.normalAmount = 1.0;
    out = skinSlide(adj, rest, surface, pulled, keep);
    CHECK(out[6 * 13 + 6].y == doctest::Approx(pulled[6 * 13 + 6].y));
}

TEST_CASE("surface pins find a uv and frame it along u")
{
    geo::Mesh plane = grid(4, 10.0);
    SurfacePoint sp;
    REQUIRE(locateUv(plane, "uv0", {0.25, 0.75}, sp));
    std::vector<glm::dvec3> pts = pointsOf(plane);
    glm::dmat4 f = surfaceFrame(pts, sp);
    CHECK(std::abs(std::abs(glm::dvec3(f[2]).y) - 1.0) < 1e-12);
    CHECK(std::abs(glm::length(glm::dvec3(f[0])) - 1.0) < 1e-12);
    SurfacePoint again;
    REQUIRE(locateNearest(plane, glm::dvec3(f[3]) + glm::dvec3(0, 3, 0), "uv0", again));
    CHECK(near(glm::dvec3(surfaceFrame(pts, again)[3]), glm::dvec3(f[3])));
    CHECK_FALSE(locateUv(plane, "uv0", {3.0, 3.0}, sp));
}

// ------------------------------------------------------------------------------------------------ scene

namespace
{
using namespace lila::scene;

json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

std::vector<glm::dvec3> scenePoints(const std::string& mesh, bool world = false)
{
    Graph& g = Graph::current();
    g.evaluate();
    Node* n = g.find(mesh);
    REQUIRE(n);
    MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
    std::vector<glm::dvec3> out(m->positions().begin(), m->positions().end());
    if (world)
        out = transformed(n->as<glm::dmat4>(Transform::kWorldMatrix), out);
    return out;
}

void editPoints(const std::string& mesh, const std::function<glm::dvec3(const glm::dvec3&)>& offset)
{
    Graph& g = Graph::current();
    PlugRef plug = modifiers::editPlug(g, g.find(mesh)->id());
    auto m = std::make_shared<geo::Mesh>(*std::get<MeshPtr>(g.value(plug)));
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        m->setPosition(p, m->position(p) + offset(m->position(p)));
    g.setValue(plug, MeshPtr(m));
}

std::string tempPath(const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_deform_tests";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

bool samePoints(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b, double eps = 1e-6)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!near(a[i], b[i], eps))
            return false;
    return true;
}
} // namespace

TEST_CASE("shrinkwrapDeformer wraps live, saves and undoes")
{
    run("newScene");
    std::string ball = run("polySphere", {{"name", "Ball"}, {"radius", 10}})[0];
    std::string box = run("polyCube", {{"name", "Box"}, {"width", 30}, {"height", 30}, {"depth", 30}})[0];
    std::vector<glm::dvec3> rest = scenePoints(ball);
    std::string d = run("shrinkwrapDeformer", {{"args", {ball, box}}, {"offset", 1.0}, {"placement", "outsideSurface"}})[0];
    for (const glm::dvec3& p : scenePoints(ball, true))
        CHECK(std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) == doctest::Approx(16.0));
    run("move", {{"args", {box}}, {"by", v3(0, 5, 0)}});
    double top = -1e9;
    for (const glm::dvec3& p : scenePoints(ball, true))
        top = std::max(top, p.y);
    CHECK(top == doctest::Approx(21.0));
    run("setAttr", {{"args", {d + ".envelope", 0.0}}});
    CHECK(samePoints(scenePoints(ball), rest));
    run("setAttr", {{"args", {d + ".envelope", 1.0}}});
    std::vector<glm::dvec3> wrapped = scenePoints(ball);
    std::string path = tempPath("shrinkwrap.lilaa");
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(samePoints(scenePoints(ball), wrapped));
    CHECK_THROWS(run("shrinkwrapDeformer", {{"args", {ball, box}}, {"mode", "sideways"}}));
}

TEST_CASE("surfaceDeform, wrap with several drivers, and old wrap bindings")
{
    run("newScene");
    std::string ball = run("polySphere", {{"name", "Ball"}, {"radius", 10}})[0];
    std::string proxy = run("polyCube", {{"name", "Proxy"}, {"width", 30}, {"height", 30}, {"depth", 30}})[0];
    std::vector<glm::dvec3> rest = scenePoints(ball);
    std::string sd = run("surfaceDeform", {{"args", {ball, proxy}}})[0];
    CHECK(samePoints(scenePoints(ball), rest));
    run("move", {{"args", {proxy}}, {"by", v3(3, 0, -2)}});
    std::vector<glm::dvec3> out = scenePoints(ball);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(near(out[i], rest[i] + glm::dvec3(3, 0, -2), 1e-6));
    run("setAttr", {{"args", {sd + ".mode", 1}}});
    run("rebindDeformer", {{"args", {sd}}});
    CHECK(samePoints(scenePoints(ball), rest));
    run("move", {{"args", {proxy}}, {"by", v3(0, 2, 0)}});
    out = scenePoints(ball);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(near(out[i], rest[i] + glm::dvec3(0, 2, 0), 1e-6));

    run("newScene");
    std::string left = run("polySphere", {{"name", "Left"}, {"radius", 5}})[0];
    std::string right = run("polySphere", {{"name", "Right"}, {"radius", 5}})[0];
    run("move", {{"args", {right}}, {"by", v3(30, 0, 0)}});
    std::string a = run("polyCube", {{"name", "A"}, {"width", 14}, {"height", 14}, {"depth", 14}})[0];
    std::string b = run("polyCube", {{"name", "B"}, {"width", 14}, {"height", 14}, {"depth", 14}})[0];
    run("move", {{"args", {b}}, {"by", v3(30, 0, 0)}});
    std::vector<glm::dvec3> leftRest = scenePoints(left, true), rightRest = scenePoints(right, true);
    std::string w = run("wrap", {{"args", {left, a}}, {"drivers", {b}}})[0];
    std::string w2 = run("wrap", {{"args", {right, a}}})[0];
    run("addWrapDriver", {{"args", {w2, b}}});
    CHECK(samePoints(scenePoints(left, true), leftRest));
    CHECK(samePoints(scenePoints(right, true), rightRest));
    run("move", {{"args", {b}}, {"by", v3(0, 4, 0)}});
    CHECK(samePoints(scenePoints(left, true), leftRest));
    std::vector<glm::dvec3> r = scenePoints(right, true);
    CHECK(near(r[0], rightRest[0] + glm::dvec3(0, 4, 0), 1e-6));
    run("undo");
    run("undo");
    CHECK(samePoints(scenePoints(right, true), rightRest));

    // A binding saved before wrap 2 still loads and rides by offset exactly as it did.
    Graph& g = Graph::current();
    Node* wn = g.find(w);
    geo::Mesh driverWorld = *g.find(a)->as<MeshPtr>(MeshNode::kOutMesh);
    WrapBinding old = wrapBind(driverWorld, leftRest, 4, 2.0);
    json j = {{"kind", "wrapBinding"}, {"offsets", old.offsets}, {"driverPoints", old.driverPoints}};
    for (const auto& inf : old.influences)
    {
        for (int k = 0; k < 3; ++k)
        {
            j["tris"].push_back(inf.tri[k]);
            j["bary"].push_back(inf.bary[k]);
            j["offset"].push_back(inf.offset[k]);
        }
        j["weight"].push_back(inf.weight);
    }
    auto* load = data::kinds().find("wrapBinding");
    REQUIRE(load);
    g.setValue({wn->id(), wn->type().attrIndex("binding"), -1}, (*load)(j));
    run("move", {{"args", {a}}, {"by", v3(0, 0, 2)}});
    std::vector<glm::dvec3> drv = scenePoints(a, true);
    std::vector<glm::dvec3> expect = wrapApply(old, drv, leftRest);
    CHECK(samePoints(scenePoints(left, true), expect));
}

TEST_CASE("collision, rigidify, skinSlide and pins from commands")
{
    run("newScene");
    std::string sheet = run("polyPlane", {{"name", "Sheet"}, {"width", 40}, {"depth", 40}, {"subdivisionsX", 20}, {"subdivisionsZ", 20}})[0];
    std::string ball = run("polySphere", {{"name", "Ball"}, {"radius", 8}})[0];
    std::string c = run("collision", {{"args", {sheet, ball}}, {"offset", 0.5}})[0];
    Graph& g = Graph::current();
    g.evaluate();
    MeshPtr ballMesh = g.find(ball)->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(ballMesh);
    SurfaceTarget collider(*ballMesh);
    for (const glm::dvec3& p : scenePoints(sheet, true))
        CHECK_FALSE(collider.inside(p));
    std::string loc = run("spaceLocator", {{"name", "Bone"}}).get<std::string>();
    run("addCollider", {{"args", {c, loc}}, {"capsuleRadius", 3.0}});
    CHECK(g.find(c)->as<bool>(g.find(c)->type().attrIndex("capsuleOn0")));

    run("newScene");
    std::string cube = run("polyCube", {{"name", "Plate"}, {"width", 4}, {"height", 4}, {"depth", 4}, {"subdivisionsX", 3}})[0];
    std::vector<glm::dvec3> restCube = scenePoints(cube);
    json twist = run("nonlinear", {{"args", {cube}}, {"type", "twist"}, {"params", {{"endAngle", 0.0}}}});
    std::string rig = run("rigidify", {{"args", {cube}}, {"regions", "islands"}})[0];
    run("setAttr", {{"args", {twist[1].get<std::string>() + ".endAngle", 60.0}}});
    std::vector<glm::dvec3> rigid = scenePoints(cube);
    CHECK_FALSE(samePoints(rigid, restCube));
    for (size_t i = 1; i < rigid.size(); ++i)
        CHECK(glm::length(rigid[0] - rigid[i]) == doctest::Approx(glm::length(restCube[0] - restCube[i])));

    std::string slide = run("skinSlide", {{"args", {cube}}})[0];
    CHECK(samePoints(scenePoints(cube), rigid, 1e-6));
    run("undo");

    run("newScene");
    Graph& g3 = Graph::current();
    std::string skin = run("polyPlane", {{"name", "Skin"}, {"width", 10}, {"depth", 10}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    std::string prop = run("spaceLocator", {{"name", "Button"}}).get<std::string>();
    run("move", {{"args", {prop}}, {"by", v3(1, 2, 1)}});
    json pins = run("surfacePin", {{"args", {prop, skin}}, {"mode", "uv"}});
    REQUIRE(pins.size() == 1);
    glm::dvec3 before = glm::dvec3(g3.find(prop)->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
    CHECK(near(before, glm::dvec3(1, 2, 1), 1e-6));
    editPoints(skin, [](const glm::dvec3&) { return glm::dvec3(0, 3, 0); });
    g3.evaluate();
    glm::dvec3 after = glm::dvec3(g3.find(prop)->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
    CHECK(near(after, before + glm::dvec3(0, 3, 0), 1e-6));
    json near2 = run("surfacePin", {{"args", {skin}}, {"mode", "nearest"}, {"position", v3(2, 0, 2)}});
    g3.evaluate();
    CHECK(near(glm::dvec3(g3.find(near2[0].get<std::string>())->as<glm::dmat4>(Transform::kWorldMatrix)[3]), glm::dvec3(2, 3, 2), 1e-6));
}
