#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

scene::MeshPtr meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

json curveJson(const std::string& name) { return run("curveData", {{"args", {name}}, {"worldSpace", true}}); }

json pts(std::initializer_list<D3> list)
{
    json a = json::array();
    for (const D3& p : list)
        a.push_back({p.x, p.y, p.z});
    return a;
}

std::string square(double x0, double z0, double size, const char* name)
{
    return run("curve", {{"type", "poly"},
                         {"closed", true},
                         {"points", pts({D3(x0, 0, z0), D3(x0 + size, 0, z0), D3(x0 + size, 0, z0 + size), D3(x0, 0, z0 + size)})},
                         {"name", name}});
}

int openEdges(const geo::Mesh& m)
{
    int open = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            ++open;
    return open;
}
} // namespace

TEST_CASE("curveBoolean is live, undoable and bakes")
{
    run("newScene");
    std::string a = square(0, 0, 10, "A"), b = square(5, 5, 10, "B");
    json r = run("curveBoolean", {{"args", {a, b}}, {"operation", "union"}});
    std::string res = r[0];
    REQUIRE(curveJson(res).size() == 1);
    CHECK(curveJson(res)[0]["points"].size() == 8);
    // Moving a tool away from the target leaves two loops.
    run("move", {{"args", {b}}, {"by", {30, 0, 0}}});
    CHECK(curveJson(res).size() == 2);
    run("undo");
    CHECK(curveJson(res).size() == 1);
    run("setAttr", {{"args", {std::string(r[1]) + ".operation", 1}}});
    REQUIRE(curveJson(res).size() == 1);
    CHECK(curveJson(res)[0]["points"].size() == 6);
    json baked = run("curveBoolean", {{"args", {a, b}}, {"operation", "intersect"}, {"constructionHistory", false}});
    CHECK(baked[1].is_null());
    CHECK(curveJson(baked[0])[0]["points"].size() == 4);
    CHECK_THROWS_AS(run("curveBoolean", {{"args", {a}}, {"operation", "subtract"}}), CommandError);
}

TEST_CASE("offsetRegion grows and merges live")
{
    run("newScene");
    std::string a = square(0, 0, 4, "A"), b = square(5, 0, 4, "B");
    json r = run("offsetRegion", {{"args", {a, b}}, {"distance", 0.25}, {"join", "miter"}});
    CHECK(curveJson(r[0]).size() == 2);
    run("setAttr", {{"args", {std::string(r[1]) + ".distance", 0.75}}});
    CHECK(curveJson(r[0]).size() == 1);
}

TEST_CASE("bevelPlus follows its curves and saves")
{
    run("newScene");
    std::string outer = run("circle", {{"radius", 10.0}, {"name", "Outer"}});
    std::string inner = run("circle", {{"radius", 5.0}, {"name", "Inner"}});
    json r = run("bevelPlus", {{"args", {outer, inner}}, {"extrudeDepth", 3.0}, {"frontProfile", "convex"}, {"frontSegments", 3}});
    std::string mesh = r[0], node = r[1];
    scene::MeshPtr m = meshOf(mesh);
    REQUIRE(m);
    CHECK(m->numFaces() > 50);
    CHECK(openEdges(*m) == 0);
    CHECK(m->bounds().max.y == doctest::Approx(3.5));
    // The hole is a hole: nothing near the centre.
    bool centre = false;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p) && glm::length(glm::dvec2(m->position(p).x, m->position(p).z)) < 4.0)
            centre = true;
    CHECK(!centre);
    // Scaling the hole curve rebuilds the solid.
    run("scale", {{"args", {inner}}, {"by", {0.5, 0.5, 0.5}}});
    m = meshOf(mesh);
    centre = false;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p) && glm::length(glm::dvec2(m->position(p).x, m->position(p).z)) < 4.0)
            centre = true;
    CHECK(centre);
    CHECK(openEdges(*m) == 0);
    run("setAttr", {{"args", {node + ".extrudeDepth", 6.0}}});
    CHECK(meshOf(mesh)->bounds().max.y == doctest::Approx(6.5));
    run("undo");
    CHECK(meshOf(mesh)->bounds().max.y == doctest::Approx(3.5));

    auto dir = std::filesystem::temp_directory_path() / "lila_curve_tests";
    std::string path = (dir / "bevel.lila").string();
    size_t faces = meshOf(mesh)->numFaces();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(meshOf(mesh)->numFaces() == faces);

    // A custom profile from a curve.
    std::string prof = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(0.2, 0.8, 0), D3(1, 1, 0)})}, {"name", "Prof"}});
    json c = run("bevelPlus", {{"args", {outer}}, {"profileCurve", prof}, {"frontWidth", 1.0}, {"frontDepth", 1.0}});
    CHECK(openEdges(*meshOf(c[0])) == 0);
    CHECK(run("getAttr", {{"args", {std::string(c[1]) + ".warning"}}}).get<std::string>().empty());
}

TEST_CASE("textMesh makes live text as a mesh in one step")
{
    run("newScene");
    json plain = run("textMesh", {{"text", "Lo"}});
    REQUIRE(plain.size() == 3);
    for (const char* attr : {"frontBevel", "backBevel", "backCap"})
        CHECK(run("getAttr", {{"args", {std::string(plain[2]) + "." + attr}}}) == false);
    CHECK(run("getAttr", {{"args", {std::string(plain[2]) + ".extrudeDepth"}}}).get<double>() == doctest::Approx(0.5));

    json r = run("textMesh", {{"text", "Lo"}, {"size", 8.0}, {"extrudeDepth", 1.0}, {"frontBevel", true}, {"frontWidth", 0.2}, {"frontDepth", 0.2},
                              {"backCap", true}});
    REQUIRE(r.size() == 3);
    std::string mesh = r[0], text = r[1], bevel = r[2];
    CHECK(run("getAttr", {{"args", {bevel + ".backBevel"}}}) == false);
    scene::MeshPtr m = meshOf(mesh);
    REQUIRE(m);
    int open = 0;
    for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        open += m->edgeValid(e) && m->isBoundaryEdge(e) ? 1 : 0;
    CHECK(open == 0);
    size_t faces = m->numFaces();
    run("setAttr", {{"args", {text + ".text", "Lola"}}});
    CHECK(meshOf(mesh)->numFaces() > faces);

    json baked = run("textMesh", {{"text", "A"}, {"constructionHistory", false}});
    CHECK(baked[1].is_null());
    CHECK(baked[2].is_null());
    CHECK(meshOf(baked[0])->numFaces() > 0);
    CHECK(scene::Graph::current().find(text));
}

TEST_CASE("bevelPlus sits on the curve's transform and extrudes out of the text's front")
{
    run("newScene");
    scene::Graph& g = scene::Graph::current();
    json t = run("textCurves", {{"text", "H"}, {"size", 10.0}});
    std::string curve = t[0];
    // Text on a wall facing -X: the fitted plane would otherwise point +X, into the wall.
    run("setAttr", {{"args", {curve + ".translate", {50, 20, 5}}}});
    run("setAttr", {{"args", {curve + ".rotate", {0, -90, 0}}}});
    run("select", {{"args", {curve}}});
    json r = run("textMesh", {{"extrudeDepth", 2.0}, {"frontBevel", false}, {"backBevel", false}});
    std::string mesh = r[0];
    CHECK(r[1] == t[1]);
    CHECK(run("getAttr", {{"args", {r[1].get<std::string>() + ".text"}}}) == "H");
    g.evaluate();
    glm::dmat4 cw = g.find(curve)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    glm::dmat4 mw = g.find(mesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k)
            CHECK(mw[c][k] == doctest::Approx(cw[c][k]).epsilon(1e-9));
    geo::Bounds b = meshOf(mesh)->bounds();
    CHECK(b.min.z == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(b.max.z == doctest::Approx(2.0).epsilon(1e-6));
    CHECK(b.min.x > -1.0);
    CHECK(b.max.x < 12.0);

    run("setAttr", {{"args", {r[2].get<std::string>() + ".extrudeDepth", -2.0}}});
    b = meshOf(mesh)->bounds();
    CHECK(b.min.z == doctest::Approx(-2.0).epsilon(1e-6));
    CHECK(b.max.z == doctest::Approx(0.0).epsilon(1e-6));

    // Moving the mesh carries its geometry.
    run("move", {{"args", {mesh}}, {"by", {-10, 0, 0}}});
    g.evaluate();
    CHECK(g.find(mesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3].x == doctest::Approx(40.0));
    CHECK(meshOf(mesh)->bounds().min.z == doctest::Approx(-2.0).epsilon(1e-6));
}
