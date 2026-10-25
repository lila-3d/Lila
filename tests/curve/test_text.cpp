#include <doctest/doctest.h>

#include "curve/BevelPlus.h"
#include "curve/CurveNodes.h"
#include "curve/Planar.h"
#include "curve/Text.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

using namespace lila;
using json = nlohmann::json;
using D2 = glm::dvec2;
using D3 = glm::dvec3;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

void bounds(const curve::Splines& s, D3& lo, D3& hi)
{
    lo = D3(1e300);
    hi = D3(-1e300);
    for (const curve::Spline& sp : s)
    {
        D3 a, b;
        curve::bounds(sp, a, b);
        lo = glm::min(lo, a);
        hi = glm::max(hi, b);
    }
}
} // namespace

TEST_CASE("text: glyph outlines from the bundled font")
{
    CHECK(!curve::findFont("Inter").empty());
    CHECK(!curve::findFont("Inter SemiBold").empty());
    CHECK(curve::findFont("Inter SemiBold") != curve::findFont("Inter"));
    CHECK(curve::findFont("No Such Font Anywhere").empty());

    curve::TextOptions o;
    o.text = "O";
    o.size = 10.0;
    curve::TextResult r = curve::textCurves(o);
    CHECK(r.warnings.empty());
    // An O is an outer loop and a counter.
    REQUIRE(r.curves.size() == 2);
    for (const curve::Spline& s : r.curves)
    {
        CHECK(s.kind == curve::Kind::Bezier);
        CHECK(s.closed);
    }
    curve::PlaneFrame xy = curve::planeFrame(D3(0.0), D3(0, 0, 1));
    curve::Loops2 loops = curve::loopsOf(r.curves, xy, {});
    std::vector<curve::Region2> regions = curve::nestLoops(loops);
    REQUIRE(regions.size() == 1);
    CHECK(regions[0].holes.size() == 1);
    // Outer contours wind counter-clockwise, counters clockwise, as the font means them.
    curve::Splines first = {r.curves[0]}, second = {r.curves[1]};
    double a0 = curve::signedArea(curve::loopsOf(first, xy, {})[0].points), a1 = curve::signedArea(curve::loopsOf(second, xy, {})[0].points);
    CHECK(a0 * a1 < 0.0);
    CHECK(std::max(a0, a1) > std::abs(std::min(a0, a1)));
    D3 lo, hi;
    bounds(r.curves, lo, hi);
    // Cap height of Inter is about 0.73 em.
    CHECK(hi.y - lo.y > 6.0);
    CHECK(hi.y - lo.y < 9.0);
    CHECK(lo.z == doctest::Approx(0.0));

    // Spacing, lines and alignment.
    o.text = "HH";
    double w0 = 0.0;
    {
        bounds(curve::textCurves(o).curves, lo, hi);
        w0 = hi.x - lo.x;
    }
    o.letterSpacing = 2.0;
    bounds(curve::textCurves(o).curves, lo, hi);
    CHECK(hi.x - lo.x == doctest::Approx(w0 + 2.0));
    o.letterSpacing = 0.0;
    o.text = "H\nH";
    bounds(curve::textCurves(o).curves, lo, hi);
    CHECK(lo.y < -5.0);
    o.text = "HHH";
    o.align = curve::TextAlign::Center;
    bounds(curve::textCurves(o).curves, lo, hi);
    CHECK(std::abs(lo.x + hi.x) < 1.0);

    // Unknown fonts fall back with a warning; spaces make no curves.
    o.font = "No Such Font Anywhere";
    o.text = "A B";
    r = curve::textCurves(o);
    CHECK(!r.warnings.empty());
    CHECK(!r.curves.empty());
    o.text = " ";
    CHECK(curve::textCurves(o).curves.empty());
}

TEST_CASE("text: textCurves is live and drives a bevel")
{
    run("newScene");
    json t = run("textCurves", {{"text", "Lo"}, {"size", 8.0}});
    std::string curve = t[0], node = t[1];
    json data = run("curveData", {{"args", {curve}}});
    size_t before = data.size();
    CHECK(before >= 3);
    json b = run("bevelPlus", {{"args", {curve}}, {"extrudeDepth", 1.0}, {"frontWidth", 0.2}, {"frontDepth", 0.2}, {"backWidth", 0.2}, {"backDepth", 0.2}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::MeshPtr m = g.find(b[0].get<std::string>())->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    int open = 0;
    for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        open += m->edgeValid(e) && m->isBoundaryEdge(e) ? 1 : 0;
    CHECK(open == 0);
    size_t faces = m->numFaces();
    run("setAttr", {{"args", {node + ".text", "Lola"}}});
    g.evaluate();
    CHECK(run("curveData", {{"args", {curve}}}).size() > before);
    CHECK(g.find(b[0].get<std::string>())->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->numFaces() > faces);
    run("undo");
    CHECK(run("curveData", {{"args", {curve}}}).size() == before);
    CHECK(!run("fontList").empty());
}
