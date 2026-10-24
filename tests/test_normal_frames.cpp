#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <glm/glm.hpp>

using namespace lila;
using json = nlohmann::json;
using scene::ComponentMode;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

geo::Mesh meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return *g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

/** A 2x1 strip of quads in the XZ plane facing +Y: points 0-2 at z=0, 3-5 at z=10. */
geo::Mesh strip()
{
    geo::Mesh m;
    for (double z : {0.0, 10.0})
        for (double x : {0.0, 10.0, 20.0})
            m.addPoint({x, 0.0, z});
    m.addFace({0, 3, 4, 1});
    m.addFace({1, 4, 5, 2});
    return m;
}

/** Sets uv0 so each corner's u is fn(position). */
template <class F> void setUvs(geo::Mesh& m, F&& fn)
{
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c))
            uv[c] = fn(m.position(m.cornerPoint(c)), m.cornerFace(c));
}

bool near(const glm::dvec3& a, const glm::dvec3& b) { return glm::length(a - b) < 1e-9; }

void checkOrthonormal(const glm::dmat3& f)
{
    for (int i = 0; i < 3; ++i)
        CHECK(glm::length(f[i]) == doctest::Approx(1.0));
    CHECK(std::abs(glm::dot(f[0], f[1])) < 1e-9);
    CHECK(near(glm::cross(f[0], f[1]), f[2]));
}
} // namespace

TEST_CASE("componentFrames: normal, uv tangent and fallbacks")
{
    geo::Mesh m = strip();
    const glm::dvec3 up(0, 1, 0), refX(1, 0, 0);

    SUBCASE("no uvs: U is the reference projected into the tangent plane")
    {
        auto f = scene::componentFrames(m, ComponentMode::Vertex, {1}, glm::dvec3(1, 0.5, 0));
        for (geo::Index p = 0; p < 6; ++p)
        {
            checkOrthonormal(f[p]);
            CHECK(near(f[p][2], up));
            CHECK(near(f[p][0], refX));
            CHECK(near(f[p][1], glm::dvec3(0, 0, -1)));
        }
    }
    SUBCASE("uvs: U follows increasing u")
    {
        setUvs(m, [](const glm::dvec3& p, geo::Index) { return glm::vec2(float(p.z) * 0.1f, float(p.x) * 0.1f); });
        auto f = scene::componentFrames(m, ComponentMode::Vertex, {1}, refX);
        for (geo::Index p = 0; p < 6; ++p)
        {
            checkOrthonormal(f[p]);
            CHECK(near(f[p][0], glm::dvec3(0, 0, 1)));
        }
    }
    SUBCASE("islands mirrored across a point cancel and fall back to the reference")
    {
        // Face 1's u runs the opposite way, so points 1 and 4 on the shared edge have no net direction.
        setUvs(m, [](const glm::dvec3& p, geo::Index f) { return glm::vec2(float(f == 0 ? p.x : -p.x) * 0.1f, float(p.z) * 0.1f); });
        auto f = scene::componentFrames(m, ComponentMode::Vertex, {1}, glm::dvec3(0, 0, 1));
        CHECK(near(f[0][0], refX));
        CHECK(near(f[2][0], -refX));
        CHECK(near(f[1][0], glm::dvec3(0, 0, 1)));
        CHECK(near(f[4][0], glm::dvec3(0, 0, 1)));
    }
    SUBCASE("edge mode: points on selected edges take U along them")
    {
        geo::Index e = m.findEdge(1, 4);
        auto f = scene::componentFrames(m, ComponentMode::Edge, {e}, refX);
        CHECK(std::abs(glm::dot(f[1][0], glm::dvec3(0, 0, 1))) == doctest::Approx(1.0));
        CHECK(std::abs(glm::dot(f[4][0], glm::dvec3(0, 0, 1))) == doctest::Approx(1.0));
        CHECK(near(f[0][0], refX));
    }
    SUBCASE("a loose point has no frame")
    {
        geo::Index loose = m.addPoint({0, 50, 0});
        auto f = scene::componentFrames(m, ComponentMode::Vertex, {loose}, refX);
        CHECK(f[loose] == glm::dmat3(0.0));
    }
}

TEST_CASE("componentFrames: face selections use only the selected faces")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    geo::Mesh m = meshOf(cube);
    geo::Index top = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceNormal(f).y > 0.9)
            top = f;
    auto frames = scene::componentFrames(m, ComponentMode::Face, {top}, glm::dvec3(1, 0, 0));
    for (geo::Index p : m.facePoints(top))
    {
        checkOrthonormal(frames[p]);
        CHECK(near(frames[p][2], glm::dvec3(0, 1, 0)));
    }

    run("selectComponents", {{"args", {cube}}, {"faces", {top}}});
    glm::dmat3 frame = scene::selectionNormalFrame();
    checkOrthonormal(frame);
    CHECK(near(frame[2], glm::dvec3(0, 1, 0)));
    // The selection's points move along their own U, which the manipulator's U shows.
    CHECK(near(frame[0], frames[m.facePoints(top)[0]][0]));
}

TEST_CASE("moveComponents uvn= moves each point along its own frame")
{
    run("newScene");
    std::string sphere = run("polySphere")[0];
    geo::Mesh before = meshOf(sphere);
    scene::components::Ids pts = {10, 11, 30, 31};
    run("selectComponents", {{"args", {sphere}}, {"vertices", pts}});
    const glm::dmat3 manip = scene::selectionNormalFrame();
    const glm::dvec3 uvn(3.0, -2.0, 1.5);
    run("moveComponents", {{"uvn", {uvn.x, uvn.y, uvn.z}}});
    geo::Mesh after = meshOf(sphere);

    // Same reference the command and manipulator use: the world axis least aligned with the selection normal.
    glm::dvec3 ref(0.0);
    int axis = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(manip[2][i]) < std::abs(manip[2][axis]) - 1e-9)
            axis = i;
    ref[axis] = 1.0;
    auto frames = scene::componentFrames(before, ComponentMode::Vertex, pts, ref);
    for (geo::Index p = 0; p < before.pointCapacity(); ++p)
    {
        if (!before.pointValid(p))
            continue;
        bool selected = std::find(pts.begin(), pts.end(), p) != pts.end();
        glm::dvec3 expect = before.position(p) + (selected ? frames[p] * uvn : glm::dvec3(0.0));
        CHECK(glm::length(after.position(p) - expect) < 1e-6);
    }
    // Each point keeps its own frame, so the four do not all move the same way.
    CHECK(glm::length((after.position(10) - before.position(10)) - (after.position(31) - before.position(31))) > 1e-3);
    CHECK(glm::length(after.position(10) - before.position(10)) == doctest::Approx(glm::length(uvn)));

    run("undo");
    CHECK(near(meshOf(sphere).position(10), before.position(10)));
    CHECK_THROWS(run("moveComponents", {{"uvn", {1, 0, 0}}, {"normal", 1}}));
}

TEST_CASE("moveComponents uvn= keeps symmetry")
{
    run("newScene");
    std::string cube = run("polyCube", {{"subdivisionsX", 4}, {"subdivisionsY", 2}})[0];
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    geo::Mesh m = meshOf(cube);
    scene::components::Ids pts;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.position(p).y > 49.0 && m.position(p).x > 1.0)
            pts.push_back(p);
    run("selectComponents", {{"args", {cube}}, {"vertices", pts}});
    run("moveComponents", {{"uvn", {4, 3, 2}}});
    geo::Mesh after = meshOf(cube);
    int moved = 0;
    for (geo::Index p = 0; p < after.pointCapacity(); ++p)
    {
        glm::dvec3 at = after.position(p);
        moved += glm::length(at - m.position(p)) > 1e-6;
        double best = 1e30;
        for (geo::Index q = 0; q < after.pointCapacity(); ++q)
            best = std::min(best, glm::length(after.position(q) - glm::dvec3(-at.x, at.y, at.z)));
        CHECK(best < 1e-9);
    }
    CHECK(moved == 2 * int(pts.size()));
}
