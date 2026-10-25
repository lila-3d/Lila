#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <functional>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

json pts(std::initializer_list<D3> list)
{
    json a = json::array();
    for (const D3& p : list)
        a.push_back({p.x, p.y, p.z});
    return a;
}

std::string polyCurve(std::initializer_list<D3> list, const char* name)
{
    return run("curve", {{"type", "poly"}, {"points", pts(list)}, {"name", name}});
}

curve::Spline worldSpline(const std::string& name, size_t k = 0)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    const curve::Splines& s = curve::splinesOf(n->value(curve::CurveShape::kWorldSpace));
    REQUIRE(s.size() > k);
    return s[k];
}

D3 endOf(const std::string& name, bool atEnd)
{
    curve::Spline s = worldSpline(name);
    return curve::pointAt(s, atEnd ? curve::spanCount(s) : 0.0);
}

scene::MeshPtr meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

bool exists(const std::string& name) { return run("objExists", {{"args", {name}}}).get<bool>(); }

json edgesWhere(const std::string& mesh, const std::function<bool(const D3& a, const D3& b)>& keep)
{
    scene::MeshPtr m = meshOf(mesh);
    json out = json::array();
    for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        if (m->edgeValid(e) && keep(m->position(m->edgePoint(e, 0)), m->position(m->edgePoint(e, 1))))
            out.push_back(e);
    return out;
}
} // namespace

TEST_CASE("blendCurve is live and bakes")
{
    run("newScene");
    std::string a = polyCurve({D3(0, 0, 0), D3(10, 0, 0)}, "A");
    std::string b = polyCurve({D3(20, 5, 0), D3(30, 5, 0)}, "B");
    json r = run("blendCurve", {{"args", {a, b}}, {"continuityA", "g2"}, {"continuityB", "g1"}});
    std::string blend = r[0], node = r[1];
    CHECK(glm::distance(endOf(blend, false), D3(10, 0, 0)) < 1e-9);
    CHECK(glm::distance(endOf(blend, true), D3(20, 5, 0)) < 1e-9);
    curve::Spline s = worldSpline(blend);
    CHECK(s.kind == curve::Kind::Nurbs);
    CHECK(s.degree == 4);
    // Moving the second curve moves the blend's end with it.
    run("move", {{"args", {b}}, {"by", {0, 10, 0}}});
    CHECK(glm::distance(endOf(blend, true), D3(20, 15, 0)) < 1e-9);
    run("setAttr", {{"args", {node + ".bulgeA", 2.0}}});
    CHECK(glm::distance(endOf(blend, true), D3(20, 15, 0)) < 1e-9);
    run("undo");
    run("undo");
    run("undo");
    CHECK(!exists(blend));
    // Explicit ends and no history.
    json r2 = run("blendCurve", {{"args", {a, b}}, {"endA", "start"}, {"endB", "end"}, {"continuityA", "g0"}, {"continuityB", "g0"},
                                 {"constructionHistory", false}});
    CHECK(r2[1].is_null());
    CHECK(worldSpline(r2[0]).kind == curve::Kind::Poly);
    CHECK(glm::distance(endOf(r2[0], false), D3(0, 0, 0)) < 1e-9);
    CHECK_THROWS_AS(run("blendCurve", {{"args", {a}}}), CommandError);

    // To a mesh border: leaves square to the edge.
    std::string plane = run("polyPlane", {{"width", 30}, {"depth", 30}, {"subdivisionsX", 3}, {"subdivisionsZ", 3}})[0];
    run("move", {{"args", {plane}}, {"by", {0, 0, 40}}});
    json border = edgesWhere(plane, [](const D3& p, const D3& q) { return std::abs(p.z + 15) < 1e-6 && std::abs(q.z + 15) < 1e-6; });
    REQUIRE(border.size() == 3);
    json rm = run("blendCurve", {{"args", {a}}, {"mesh", plane}, {"edge", border[1]}, {"continuityB", "g1"}});
    curve::Spline sm = worldSpline(rm[0]);
    D3 last = curve::pointAt(sm, curve::spanCount(sm));
    CHECK(last.z == doctest::Approx(25.0));
    CHECK(last.x == doctest::Approx(0.0).epsilon(1e-9));
    curve::Sample e = curve::evaluate(sm, curve::spanCount(sm));
    CHECK(std::abs(glm::normalize(e.d1).x) < 1e-9);
    CHECK(glm::normalize(e.d1).z > 0.999);
}

TEST_CASE("matchCurve, trimCurve, extendCurve, splitCurves and fairCurve")
{
    run("newScene");
    std::string a = run("curveEP", {{"points", pts({D3(0), D3(5, 3, 0), D3(10, -1, 0), D3(15, 2, 0)})}, {"name", "A"}});
    std::string b = run("curveEP", {{"points", pts({D3(18, 4, 0), D3(24, 6, 0), D3(30, 3, 0)})}, {"name", "B"}});
    D3 startA = endOf(a, false);
    run("matchCurve", {{"args", {a, b}}, {"continuity", "g2"}});
    CHECK(glm::distance(endOf(a, true), D3(18, 4, 0)) < 1e-9);
    CHECK(glm::distance(endOf(a, false), startA) < 1e-9);
    CHECK(glm::distance(endOf(b, false), D3(18, 4, 0)) < 1e-9);
    run("undo");
    run("matchCurve", {{"args", {a, b}}, {"average", true}});
    CHECK(glm::distance(endOf(a, true), endOf(b, false)) < 1e-9);
    CHECK(glm::distance(endOf(a, true), D3(16.5, 3, 0)) < 1e-6);

    run("newScene");
    std::string line = polyCurve({D3(0, 0, 0), D3(10, 0, 0)}, "Line");
    polyCurve({D3(3, -2, 0), D3(3, 2, 0)}, "Cut1");
    polyCurve({D3(7, -2, 0), D3(7, 2, 0)}, "Cut2");
    json left = run("trimCurve", {{"args", {line}}, {"point", {5, 0.1, 0}}});
    REQUIRE(left.size() == 2);
    CHECK(curve::length(worldSpline(left[0])) == doctest::Approx(3.0));
    CHECK(curve::length(worldSpline(left[1])) == doctest::Approx(3.0));
    run("undo");
    CHECK(curve::length(worldSpline(line)) == doctest::Approx(10.0));
    // Seen from above, a curve floating over the cutters still trims.
    std::string high = polyCurve({D3(0, 0, 5), D3(10, 0, 5)}, "High");
    CHECK(run("trimCurve", {{"args", {high}}, {"point", {9, 0, 5}}, {"cutters", {"Cut1", "Cut2"}}}).empty());
    run("undo");
    json view = run("trimCurve", {{"args", {high}}, {"point", {9, 0, 5}}, {"cutters", {"Cut1", "Cut2"}}, {"direction", {0, 0, 1}}});
    REQUIRE(view.size() == 1);
    CHECK(curve::length(worldSpline(view[0])) == doctest::Approx(7.0));

    run("newScene");
    std::string shortLine = polyCurve({D3(0, 0, 0), D3(5, 0, 0)}, "Short");
    std::string wall = polyCurve({D3(12, -5, 0), D3(12, 5, 0)}, "Wall");
    run("extendCurve", {{"args", {shortLine}}, {"mode", "line"}});
    CHECK(glm::distance(endOf(shortLine, true), D3(12, 0, 0)) < 1e-6);
    run("undo");
    run("extendCurve", {{"args", {shortLine}}, {"length", 2.0}, {"end", "start"}});
    CHECK(glm::distance(endOf(shortLine, false), D3(-2, 0, 0)) < 1e-9);
    CHECK_THROWS_AS(run("extendCurve", {{"args", {shortLine}}, {"boundaries", {wall}}, {"end", "start"}}), CommandError);

    run("newScene");
    std::string h = polyCurve({D3(0, 0, 0), D3(10, 0, 0)}, "H");
    std::string v = polyCurve({D3(5, -5, 0), D3(5, 5, 0)}, "V");
    json pieces = run("splitCurves", {{"args", {h, v}}});
    CHECK(pieces.size() == 4);
    for (const json& p : pieces)
        CHECK(curve::length(worldSpline(p)) == doctest::Approx(5.0));

    run("newScene");
    std::string wiggle = run("curveEP", {{"points", pts({D3(0), D3(4, 1.2, 0), D3(8, 0.7, 0), D3(12, 1.6, 0), D3(16, 0.4, 0), D3(20, 1.5, 0), D3(24, 0, 0)})}});
    json fr = run("fairCurve", {{"args", {wiggle}}, {"tolerance", 0.3}});
    REQUIRE(fr.size() == 1);
    CHECK(fr[0]["maxDeviation"].get<double>() <= 0.3 + 1e-9);
    CHECK(fr[0]["variationAfter"].get<double>() < fr[0]["variationBefore"].get<double>());
    run("undo");
}

TEST_CASE("curveNetwork stays live; borderBlend builds and merges")
{
    run("newScene");
    json names = json::array();
    // A tilted plane's grid lines: y = z / 10.
    for (double z : {0.0, 10.0, 20.0})
        names.push_back(polyCurve({D3(-2, z * 0.1, z), D3(22, z * 0.1, z)}, "U"));
    for (double x : {0.0, 10.0, 20.0})
        names.push_back(polyCurve({D3(x, -0.2, -2), D3(x, 2.2, 22)}, "V"));
    json r = run("curveNetwork", {{"args", names}, {"spansU", 2}, {"spansV", 2}, {"tolerance", 0.2}});
    std::string mesh = r[0], node = r[1];
    scene::MeshPtr m = meshOf(mesh);
    REQUIRE(m);
    CHECK(m->numFaces() == 16);
    // Moving every curve together moves the surface: it follows its inputs.
    for (const json& n : names)
        run("move", {{"args", {n}}, {"by", {0, 7, 0}}});
    CHECK(meshOf(mesh)->bounds().min.y > 6.0);
    run("setAttr", {{"args", {node + ".spansU", 3}}});
    CHECK(meshOf(mesh)->numFaces() == 24);
    CHECK_THROWS_AS(run("curveNetwork", {{"args", {names[0], names[1]}}}), CommandError);

    run("newScene");
    std::string pa = run("polyPlane", {{"width", 30}, {"depth", 30}, {"subdivisionsX", 3}, {"subdivisionsZ", 3}})[0];
    std::string pb = run("polyPlane", {{"width", 30}, {"depth", 30}, {"subdivisionsX", 3}, {"subdivisionsZ", 2}})[0];
    run("move", {{"args", {pb}}, {"by", {50, 10, 0}}});
    json ea = edgesWhere(pa, [](const D3& p, const D3& q) { return std::abs(p.x - 15) < 1e-6 && std::abs(q.x - 15) < 1e-6; });
    json eb = edgesWhere(pb, [](const D3& p, const D3& q) { return std::abs(p.x + 15) < 1e-6 && std::abs(q.x + 15) < 1e-6; });
    REQUIRE(ea.size() == 3);
    REQUIRE(eb.size() == 2);
    json bb = run("borderBlend", {{"meshA", pa}, {"edgesA", ea}, {"meshB", pb}, {"edgesB", eb}, {"segments", 5}});
    scene::MeshPtr strip = meshOf(bb[0]);
    REQUIRE(strip);
    CHECK(strip->numFaces() == 5 * 3);
    CHECK(strip->validate(geo::kValidateAll).empty());
    // Live: moving side B moves the strip's far edge.
    run("move", {{"args", {pb}}, {"by", {0, 5, 0}}});
    CHECK(meshOf(bb[0])->bounds().max.y == doctest::Approx(15.0));
    run("undo");
    // Merged into one mesh (both sides on it): the strip welds on, splitting the side with fewer edges.
    std::string one = run("polyPlane", {{"width", 30}, {"depth", 30}, {"subdivisionsX", 3}, {"subdivisionsZ", 3}})[0];
    run("move", {{"args", {one}}, {"by", {0, 0, 80}}});
    std::string two = run("polyPlane", {{"width", 30}, {"depth", 30}, {"subdivisionsX", 3}, {"subdivisionsZ", 2}})[0];
    run("move", {{"args", {two}}, {"by", {50, 10, 80}}});
    std::string joinedName = run("polyCombine", {{"args", {one, two}}, {"name", "Joined"}});
    scene::MeshPtr joined = meshOf(joinedName);
    REQUIRE(joined);
    geo::Index before = joined->numFaces();
    json sideA = edgesWhere(joinedName, [](const D3& p, const D3& q) { return std::abs(p.x - 15) < 1e-6 && std::abs(q.x - 15) < 1e-6; });
    json sideB = edgesWhere(joinedName, [](const D3& p, const D3& q) { return std::abs(p.x - 35) < 1e-6 && std::abs(q.x - 35) < 1e-6; });
    json all = sideA;
    for (const json& e : sideB)
        all.push_back(e);
    run("borderBlend", {{"meshA", joinedName}, {"edgesA", all}, {"segments", 4}, {"merge", true}});
    scene::MeshPtr merged = meshOf(joinedName);
    CHECK(merged->numFaces() == before + 4 * 3);
    CHECK(merged->validate(geo::kValidateAll).empty());
    int borderEdges = 0;
    for (geo::Index e = 0; e < merged->edgeCapacity(); ++e)
        if (merged->edgeValid(e) && merged->isBoundaryEdge(e))
            ++borderEdges;
    // The plates' borders less the bridged sides (12 - 3 and 10 - 2), plus the strip's two open ends.
    CHECK(borderEdges == 9 + 8 + 4 * 2);
}
