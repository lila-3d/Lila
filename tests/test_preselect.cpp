#include <doctest/doctest.h>

#include "core/PrefStore.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "geo/RenderMesh.h"
#include "scene/ComponentPick.h"
#include "scene/Graph.h"
#include "scene/Node.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

using namespace lila;
using namespace lila::scene;
namespace cp = lila::scene::componentPick;

namespace
{
// Unit quad in the xy plane. Through an identity viewProj on a 100x100 widget its corners land at
// (50,50) (100,50) (100,100) (50,100).
geo::Mesh quad()
{
    geo::Mesh m;
    geo::Index a = m.addPoint({0, 0, 0}), b = m.addPoint({1, 0, 0}), c = m.addPoint({1, 1, 0}), d = m.addPoint({0, 1, 0});
    m.addFace({a, b, c, d});
    return m;
}

cp::Screen screen()
{
    return {glm::dmat4(1.0), 100.0, 100.0};
}

uint32_t edgeBetween(const geo::Mesh& m, geo::Index p, geo::Index q)
{
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && std::minmax(m.edgePoint(e, 0), m.edgePoint(e, 1)) == std::minmax(p, q))
            return e;
    FAIL("no edge");
    return 0;
}
} // namespace

TEST_CASE("preselect: screen mapping matches the viewport's")
{
    auto s = screen().project({1, 0, 0});
    REQUIRE(s);
    CHECK(s->x == doctest::Approx(100.0));
    CHECK(s->y == doctest::Approx(50.0));
    glm::dmat4 behind(1.0);
    behind[3][3] = 0.0; // w = 0 for every point
    CHECK_FALSE(cp::Screen{behind, 100, 100}.project({0, 0, 0}));
}

TEST_CASE("preselect: nearest vertex within the click radius")
{
    geo::Mesh m = quad();
    auto c = cp::nearby(m, glm::dmat4(1.0), ComponentMode::Vertex, screen(), {53, 54}, cp::radius(ComponentMode::Vertex));
    REQUIRE(c.size() == 1);
    CHECK(c[0].id == 0);
    CHECK(c[0].px == doctest::Approx(5.0));

    CHECK(cp::nearby(m, glm::dmat4(1.0), ComponentMode::Vertex, screen(), {75, 75}, cp::radius(ComponentMode::Vertex)).empty());
    // A tighter bound (a closer hit already found on another mesh) drops it.
    CHECK(cp::nearby(m, glm::dmat4(1.0), ComponentMode::Vertex, screen(), {53, 54}, 4.0).empty());
}

TEST_CASE("preselect: candidates sort nearest first")
{
    geo::Mesh m = quad();
    // Small enough on screen that every corner is within reach.
    glm::dmat4 world = glm::scale(glm::dmat4(1.0), glm::dvec3(0.1));
    auto c = cp::nearby(m, world, ComponentMode::Vertex, screen(), {51, 51}, 12.0);
    REQUIRE(c.size() == 4);
    CHECK(c[0].id == 0);
    CHECK(c[3].id == 2);
    CHECK(std::is_sorted(c.begin(), c.end()));
}

TEST_CASE("preselect: edges by distance to the projected segment")
{
    geo::Mesh m = quad();
    auto c = cp::nearby(m, glm::dmat4(1.0), ComponentMode::Edge, screen(), {75, 52}, cp::radius(ComponentMode::Edge));
    REQUIRE(c.size() == 1);
    CHECK(c[0].id == edgeBetween(m, 0, 1));
    CHECK(c[0].px == doctest::Approx(2.0));
    glm::dvec3 mid = cp::anchor(m, glm::dmat4(1.0), ComponentMode::Edge, c[0].id);
    CHECK(mid.x == doctest::Approx(0.5));
    CHECK(mid.y == doctest::Approx(0.0));
}

TEST_CASE("preselect: world transform and faces")
{
    geo::Mesh m = quad();
    glm::dmat4 world = glm::translate(glm::dmat4(1.0), glm::dvec3(0.2, 0, 0));
    auto c = cp::nearby(m, world, ComponentMode::Vertex, screen(), {60, 50}, 12.0);
    REQUIRE(!c.empty());
    CHECK(c[0].id == 0);
    CHECK(c[0].px == doctest::Approx(0.0));
    CHECK(cp::anchor(m, world, ComponentMode::Vertex, 0).x == doctest::Approx(0.2));
    CHECK(cp::nearby(m, world, ComponentMode::Face, screen(), {75, 75}, 12.0).empty());
}

TEST_CASE("preselect: deleted components never come back")
{
    geo::Mesh m = quad();
    geo::Index extra = m.addPoint({0.02, 0.02, 0});
    m.removePoint(extra);
    auto c = cp::nearby(m, glm::dmat4(1.0), ComponentMode::Vertex, screen(), {51, 51}, 12.0);
    REQUIRE(c.size() == 1);
    CHECK(c[0].id == 0);
}

TEST_CASE("preselect: hover equality drives redraws")
{
    cp::Hover a{ComponentMode::Edge, NodeId(3), 7}, b = a;
    CHECK(a == b);
    b.id = 8;
    CHECK(a != b);
    CHECK(cp::Hover{}.empty());
    CHECK_FALSE(a.empty());
}

namespace
{
double area2d(const std::vector<glm::dvec3>& pts)
{
    double a = 0.0;
    for (size_t i = 0; i < pts.size(); ++i)
        a += pts[i].x * pts[(i + 1) % pts.size()].y - pts[(i + 1) % pts.size()].x * pts[i].y;
    return std::abs(a) * 0.5;
}
} // namespace

TEST_CASE("preselect: face fill covers a concave n-gon exactly")
{
    // A square with a notch cut into its right side; a fan from the first corner covers the notch and folds over.
    std::vector<glm::dvec3> pts = {{2, 2, 0}, {0, 2, 0}, {0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}};
    geo::Mesh m;
    std::vector<geo::Index> ids;
    for (const glm::dvec3& p : pts)
        ids.push_back(m.addPoint(p));
    geo::Index f = m.addFace(ids);
    std::vector<uint32_t> tris = cp::fillTriangles(m, f);
    REQUIRE(tris.size() == 3 * (pts.size() - 2));
    double covered = 0.0;
    for (size_t t = 0; t < tris.size(); t += 3)
    {
        glm::dvec3 a = m.position(tris[t]), b = m.position(tris[t + 1]), c = m.position(tris[t + 2]);
        double signedArea = 0.5 * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
        CHECK(signedArea > -1e-12);
        covered += std::abs(signedArea);
    }
    CHECK(covered == doctest::Approx(area2d(pts)));
}

TEST_CASE("preselect: topology callouts")
{
    geo::Mesh tri;
    tri.addFace({tri.addPoint({0, 0, 0}), tri.addPoint({1, 0, 0}), tri.addPoint({0, 1, 0})});
    CHECK(cp::faceTopology(tri, 0) == cp::Topology::Triangle);
    CHECK(cp::faceTopology(quad(), 0) == cp::Topology::Plain);

    // A fan of five triangles: the hub has five edges and is a pole, the rim points have three.
    geo::Mesh fan;
    geo::Index hub = fan.addPoint({0, 0, 0});
    std::vector<geo::Index> rim;
    for (int i = 0; i < 5; ++i)
        rim.push_back(fan.addPoint({std::cos(i * 1.2566), std::sin(i * 1.2566), 0.0}));
    for (int i = 0; i < 5; ++i)
        fan.addFace({hub, rim[i], rim[(i + 1) % 5]});
    CHECK(cp::pointTopology(fan, hub) == cp::Topology::Pole);
    CHECK(cp::pointTopology(fan, rim[0]) == cp::Topology::Plain);

    geo::Mesh penta;
    std::vector<geo::Index> ring;
    for (int i = 0; i < 5; ++i)
        ring.push_back(penta.addPoint({std::cos(i * 1.2566), std::sin(i * 1.2566), 0.0}));
    penta.addFace(ring);
    CHECK(cp::faceTopology(penta, 0) == cp::Topology::Ngon);
    CHECK(cp::topologyEnabledKey(cp::Topology::Ngon) != cp::topologyEnabledKey(cp::Topology::Pole));
}

TEST_CASE("selection masks gate object kinds")
{
    namespace sm = lila::scene::selectionMask;
    for (sm::Kind k : sm::kAll)
        sm::setEnabled(k, true);
    Graph& g = Graph::current();
    Node* mesh = g.createNode(*types::find("mesh"), "maskMesh");
    Node* loc = g.createNode(*types::find("locator"), "maskLoc");
    Node* cam = g.createNode(*types::find("camera"), "maskCam");
    CHECK(sm::kindOf(*mesh) == sm::Kind::Mesh);
    CHECK(sm::kindOf(*loc) == sm::Kind::Locator);
    CHECK(sm::kindOf(*cam) == sm::Kind::Other);
    sm::setEnabled(sm::Kind::Locator, false);
    CHECK_FALSE(sm::pickable(*loc));
    CHECK(sm::pickable(*cam));
    CHECK(sm::pickable(*mesh));
    for (sm::Kind k : sm::kAll)
        sm::setEnabled(k, true);
}

TEST_CASE("preselect: the triangles of a primitive call out through the pick data")
{
    geo::Mesh cone = geo::makeCone();
    geo::RenderMesh rm = geo::buildRenderMesh(cone);
    int triangles = 0;
    for (geo::Index f = 0; f < cone.faceCapacity(); ++f)
        if (cone.faceValid(f) && cp::faceTopology(cone, f) == cp::Topology::Triangle)
            ++triangles;
    CHECK(triangles == 20);
    int hits = 0;
    for (geo::Index t = 0; t < rm.triangleCount(); ++t)
        if (cp::faceTopology(cone, rm.triangleFaces[t]) == cp::Topology::Triangle)
            ++hits;
    CHECK(hits == 20);
}
