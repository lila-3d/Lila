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

TEST_CASE("text: wrapping, justify, anchor, case and styling")
{
    using curve::TextOptions;
    D3 lo, hi;
    auto box = [&](const TextOptions& o) {
        curve::TextResult r = curve::textCurves(o);
        CHECK(r.warnings.empty());
        bounds(r.curves, lo, hi);
        return r;
    };
    TextOptions o;
    o.size = 10.0;

    // Wrapping breaks between words; a long word breaks inside itself.
    o.text = "HH HH HH";
    box(o);
    double oneLine = hi.x - lo.x, top = hi.y;
    o.wrapWidth = oneLine * 0.5;
    box(o);
    CHECK(hi.x - lo.x <= o.wrapWidth + 1e-6);
    CHECK(lo.y < -10.0);
    o.text = "HHHHHHHH";
    box(o);
    CHECK(hi.x - lo.x <= o.wrapWidth + 1e-6);
    CHECK(lo.y < -5.0);

    // Justify stretches every wrapped line but the last to the wrap width.
    o.text = "H HH H HH H";
    o.wrapWidth = oneLine * 0.8;
    o.align = curve::TextAlign::Justify;
    curve::TextResult j = box(o);
    double firstLineRight = -1e300;
    for (size_t c = 0; c < j.curves.size(); ++c)
    {
        D3 a, b;
        curve::bounds(j.curves[c], a, b);
        if (a.y > -1.0)
            firstLineRight = std::max(firstLineRight, b.x);
    }
    CHECK(firstLineRight > o.wrapWidth * 0.97);
    o.align = curve::TextAlign::Left;
    o.wrapWidth = 0.0;

    // Anchors move the block onto y = 0.
    o.text = "H\nH";
    o.anchor = curve::TextAnchor::Top;
    box(o);
    CHECK(hi.y < 1e-6);
    CHECK(hi.y > -3.0);
    o.anchor = curve::TextAnchor::Bottom;
    box(o);
    CHECK(lo.y > -1e-6);
    o.anchor = curve::TextAnchor::Middle;
    box(o);
    CHECK(lo.y < 0.0);
    CHECK(hi.y > 0.0);
    o.anchor = curve::TextAnchor::Baseline;

    // Case changes the glyphs drawn, not the text.
    o.text = "x";
    box(o);
    double lowerTop = hi.y;
    o.textCase = curve::TextCase::Upper;
    box(o);
    CHECK(hi.y > lowerTop + 1.0);
    o.textCase = curve::TextCase::AsTyped;

    // Width scales, slant leans, weight grows the outline.
    o.text = "H";
    box(o);
    double w = hi.x - lo.x, left = lo.x;
    CHECK(top > 0.0);
    o.width = 2.0;
    box(o);
    CHECK(hi.x - lo.x == doctest::Approx(w * 2.0).epsilon(0.02));
    o.width = 1.0;
    o.slant = 15.0;
    box(o);
    CHECK(hi.x - lo.x > w + 1.0);
    o.slant = 0.0;
    o.weight = 0.3;
    box(o);
    CHECK(hi.x - lo.x == doctest::Approx(w + 0.6).epsilon(0.02));
    CHECK(lo.x == doctest::Approx(left - 0.3).epsilon(0.05));
    o.weight = 0.0;

    // Underline and strike join the letters into one outline per line.
    o.text = "HH";
    size_t plain = curve::textCurves(o).curves.size();
    o.underline = true;
    curve::TextResult u = box(o);
    CHECK(lo.y < -0.2);
    CHECK(u.curves.size() == plain + 1);
    o.underline = false;
    o.strikethrough = true;
    curve::TextResult st = box(o);
    curve::PlaneFrame xy = curve::planeFrame(D3(0.0), D3(0, 0, 1));
    CHECK(curve::nestLoops(curve::loopsOf(st.curves, xy, {})).size() == 1);
    CHECK(lo.y > -1e-6);

    // Families and styles for the pickers.
    std::vector<curve::FontFamily> fams = curve::fontFamilies();
    auto inter = std::find_if(fams.begin(), fams.end(), [](const curve::FontFamily& f) { return f.family == "Inter"; });
    REQUIRE(inter != fams.end());
    CHECK(inter->styles.front() == "Regular");
    CHECK(std::find(inter->styles.begin(), inter->styles.end(), "Italic") != inter->styles.end());
}
