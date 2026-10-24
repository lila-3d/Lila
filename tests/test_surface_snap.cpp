#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/SurfaceSnap.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace lila;
using json = nlohmann::json;
namespace surfaceSnap = lila::ui::surfaceSnap;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

scene::NodeId idOf(const std::string& path) { return scene::Graph::current().find(path)->id(); }

/** World positions of a mesh's points. */
std::vector<glm::dvec3> worldPoints(const std::string& path)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(path);
    const geo::Mesh& m = *n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    std::vector<glm::dvec3> out;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            out.push_back(glm::dvec3(w * glm::dvec4(m.position(p), 1.0)));
    return out;
}

void checkOrthonormal(const glm::dmat3& r)
{
    for (int i = 0; i < 3; ++i)
    {
        CHECK(glm::length(r[i]) == doctest::Approx(1.0));
        CHECK(glm::dot(r[i], r[(i + 1) % 3]) == doctest::Approx(0.0));
    }
}
} // namespace

TEST_CASE("alignUp turns the up axis onto the normal by the shortest arc")
{
    const glm::dvec3 up(0, 1, 0);
    SUBCASE("tilted normal")
    {
        glm::dvec3 n = glm::normalize(glm::dvec3(1, 1, 0.3));
        glm::dmat3 r = surfaceSnap::alignUp(glm::dmat3(1.0), up, n);
        checkOrthonormal(r);
        glm::dvec3 got = r * up;
        CHECK(got.x == doctest::Approx(n.x));
        CHECK(got.y == doctest::Approx(n.y));
        CHECK(got.z == doctest::Approx(n.z));
        // The axis of the turn is perpendicular to both, so it stays put.
        glm::dvec3 axis = glm::normalize(glm::cross(up, n));
        CHECK(glm::length(r * axis - axis) == doctest::Approx(0.0));
    }
    SUBCASE("starts from the object's own rotation")
    {
        glm::dmat3 start = glm::dmat3(glm::rotate(glm::dmat4(1.0), 0.7, glm::dvec3(0, 0, 1)));
        glm::dvec3 n(0, 0, 1);
        glm::dmat3 r = surfaceSnap::alignUp(start, up, n);
        checkOrthonormal(r);
        CHECK(glm::length(r * up - n) == doctest::Approx(0.0));
    }
    SUBCASE("already aligned and opposite")
    {
        CHECK(surfaceSnap::alignUp(glm::dmat3(1.0), up, up) == glm::dmat3(1.0));
        glm::dmat3 r = surfaceSnap::alignUp(glm::dmat3(1.0), up, -up);
        checkOrthonormal(r);
        CHECK(glm::length(r * up + up) == doctest::Approx(0.0));
    }
}

TEST_CASE("moveComponents onto= lands moved points on a surface")
{
    run("newScene");
    std::string ground = run("polyPlane", {{"width", 1000}, {"depth", 1000}})[0];
    std::string cube = run("polyCube", {{"position", {0, 200, 0}}})[0];
    run("selectComponents", {{"args", {cube}}, {"vertices", {0, 1, 2, 3, 4, 5, 6, 7}}});
    run("moveComponents", {{"by", {30, 50, 0}}, {"onto", {ground}}});
    for (const glm::dvec3& p : worldPoints(cube))
        CHECK(p.y == doctest::Approx(0.0));
    run("undo");
    for (const glm::dvec3& p : worldPoints(cube))
        CHECK(p.y > 100.0);
    CHECK_THROWS(run("moveComponents", {{"by", {0, 1, 0}}, {"onto", {"nothing"}}}));
}

TEST_CASE("snap surface designation and queries")
{
    surfaceSnap::registerCommands();
    run("newScene");
    std::string ground = run("polyPlane", {{"width", 1000}, {"depth", 1000}})[0];
    std::string box = run("polyCube", {{"position", {0, 300, 0}}})[0];
    run("select", {{"args", {box}}});

    // Nothing designated: rays hit the nearest visible mesh, minus exclude.
    auto top = surfaceSnap::raycast({0, 1000, 0}, {0, -1, 0});
    REQUIRE(top);
    CHECK(top->node == idOf(box));
    CHECK(top->normal.y == doctest::Approx(1.0));
    auto below = surfaceSnap::raycast({0, 1000, 0}, {0, -1, 0}, {idOf(box)});
    REQUIRE(below);
    CHECK(below->node == idOf(ground));
    CHECK(below->position.y == doctest::Approx(0.0));
    CHECK_FALSE(surfaceSnap::closest({5, 40, 5}));

    CHECK(run("setSnapSurface", {{"args", {ground}}}) == json::array({ground}));
    CHECK(surfaceSnap::active());
    CHECK(run("snapSurface") == json::array({ground}));
    // Only the snap surface is hit now, even with the box in the way.
    auto hit = surfaceSnap::raycast({0, 1000, 0}, {0, -1, 0});
    REQUIRE(hit);
    CHECK(hit->node == idOf(ground));
    auto c = surfaceSnap::closest({5, 40, 5});
    REQUIRE(c);
    CHECK(c->position.y == doctest::Approx(0.0));
    CHECK(c->position.x == doctest::Approx(5.0));

    // The cached BVH follows edits to the surface.
    run("move", {{"args", {ground}}, {"by", {0, -20, 0}}});
    CHECK(surfaceSnap::closest({5, 40, 5})->position.y == doctest::Approx(-20.0));

    run("clearSnapSurface");
    CHECK_FALSE(surfaceSnap::active());
    CHECK(run("snapSurface").empty());
    run("select", {{"clear", true}});
    CHECK_THROWS(run("setSnapSurface"));

    run("setSnapSurface", {{"args", {ground}}});
    run("newScene");
    CHECK_FALSE(surfaceSnap::active());
}
