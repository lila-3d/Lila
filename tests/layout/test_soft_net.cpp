#include "geo/Mesh.h"
#include "layout/SoftNet.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <doctest/doctest.h>

using namespace lila;

namespace softnettest
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

scene::MeshPtr outMesh(const std::string& path)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(path);
    REQUIRE(n);
    return n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

/** The point of m nearest p (the plane's grid has one at every multiple of 2.5). */
glm::dvec3 near(const geo::Mesh& m, const glm::dvec3& p)
{
    glm::dvec3 best(0.0);
    double d = 1e300;
    for (geo::Index i = 0; i < m.pointCapacity(); ++i)
        if (m.pointValid(i) && glm::length(m.position(i) - p) < d)
        {
            d = glm::length(m.position(i) - p);
            best = m.position(i);
        }
    return best;
}

/** How far the point that sat at p moved between a and b. */
glm::dvec3 moved(const geo::Mesh& a, const geo::Mesh& b, const glm::dvec3& p)
{
    for (geo::Index i = 0; i < a.pointCapacity(); ++i)
        if (a.pointValid(i) && glm::length(a.position(i) - p) < 1e-6)
            return b.position(i) - a.position(i);
    FAIL("no point at the spot");
    return {};
}

json line(double x0, double x1, int n = 120)
{
    json pts = json::array();
    for (int i = 0; i <= n; ++i)
        pts.push_back({x0 + (x1 - x0) * double(i) / n, 0.0, 0.0});
    return pts;
}
} // namespace softnettest

using namespace softnettest;

TEST_CASE("softNet: a knot moved along the surface normal carries its line and the surface, fading with distance, without cutting")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    const std::string mesh = softnettest::run("polyPlane", {{"name", "Sheet"}, {"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 40}, {"subdivisionsZ", 40}})[0];
    const std::string layout = softnettest::run("panelLayout", {{"args", {mesh}}, {"mirror", "off"}})[0];
    softnettest::run("panelLayoutLineSet", {{"args", {layout}}, {"set", "Major"}, {"output", "none"}});
    softnettest::run("panelLayoutStroke", {{"args", {layout}}, {"points", line(-30, 30)}, {"tolerance", 1.0}});
    const scene::MeshPtr before = softnettest::outMesh(mesh);
    REQUIRE(before);

    const json net = softnettest::run("softNet", {{"args", {mesh}}, {"reach", 12.0}});
    REQUIRE(net["knots"].size() == 2);
    CHECK(net["samples"].get<int>() > 10);
    // At rest the net changes nothing.
    scene::MeshPtr rest = softnettest::outMesh(mesh);
    CHECK(rest->numPoints() == before->numPoints());
    for (geo::Index i = 0; i < before->pointCapacity(); ++i)
        if (before->pointValid(i))
            CHECK(glm::length(rest->position(i) - before->position(i)) < 1e-9);

    // The knot at x = 30 lifts 5 along its frame's z, the surface normal.
    scene::Graph& g = scene::Graph::current();
    std::string knot;
    for (const json& k : net["knots"])
    {
        scene::Node* n = g.find(k.get<std::string>());
        REQUIRE(n);
        g.evaluate();
        if (glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]).x > 0.0)
            knot = k.get<std::string>();
    }
    REQUIRE(!knot.empty());
    softnettest::run("setAttr", {{"args", {knot + ".translate", 0.0, 0.0, 5.0}}});
    const scene::MeshPtr bent = softnettest::outMesh(mesh);
    CHECK(bent->numPoints() == before->numPoints());
    CHECK(bent->numFaces() == before->numFaces());
    const glm::dvec3 atKnot = moved(*before, *bent, near(*before, {30, 0, 0}));
    const glm::dvec3 middle = moved(*before, *bent, near(*before, {0, 0, 0}));
    const glm::dvec3 otherEnd = moved(*before, *bent, near(*before, {-30, 0, 0}));
    const glm::dvec3 far = moved(*before, *bent, near(*before, {30, 0, 45}));
    MESSAGE("knot ", atKnot.y, " middle ", middle.y, " other end ", otherEnd.y, " far ", far.y);
    CHECK(std::abs(std::abs(atKnot.y) - 5.0) < 0.5);
    CHECK(std::abs(std::abs(middle.y) - 2.5) < 0.5);
    CHECK(std::abs(otherEnd.y) < 0.3);
    CHECK(std::abs(far.y) < 0.15 * std::abs(atKnot.y));
    // Back at rest, the surface is back where it was.
    softnettest::run("setAttr", {{"args", {knot + ".translate", 0.0, 0.0, 0.0}}});
    rest = softnettest::outMesh(mesh);
    for (geo::Index i = 0; i < before->pointCapacity(); ++i)
        if (before->pointValid(i))
            CHECK(glm::length(rest->position(i) - before->position(i)) < 1e-9);
}
