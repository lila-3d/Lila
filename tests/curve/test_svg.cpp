#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "curve/Planar.h"
#include "curve/Svg.h"
#include "io/FileTranslators.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <glm/gtc/constants.hpp>

#include <filesystem>
#include <fstream>

using namespace lila;
using json = nlohmann::json;
using D2 = glm::dvec2;
using D3 = glm::dvec3;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

double area(const curve::Spline& s)
{
    curve::TessOptions o;
    o.chordError = 0.0005;
    curve::Polyline pl = curve::tessellate(s, o);
    std::vector<D2> p;
    for (const D3& q : pl.points)
        p.push_back(D2(q.x, q.y));
    return curve::signedArea(p);
}

const char* kDoc = R"SVG(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 100" width="400" height="200">
  <rect id="box" x="10" y="10" width="40" height="20"/>
  <circle id="dot" cx="100" cy="50" r="20"/>
  <g transform="translate(150 50) rotate(45)">
    <path id="tri" d="M0 -10 L10 10 H-10 Z"/>
  </g>
  <path id="arc" d="M10 80 A 20 10 0 0 1 50 80 Q 70 60 90 80 C 100 90 110 70 120 80 S 140 90 150 80 T 190 80"/>
  <polyline id="zig" points="10,95 20,90 30,95"/>
  <ellipse id="egg" cx="170" cy="20" rx="15" ry="8"/>
</svg>)SVG";
} // namespace

TEST_CASE("svg: shapes, paths and transforms come in as Beziers")
{
    curve::SvgImportOptions o;
    o.scale = 1.0;
    o.center = false;
    curve::SvgDocument doc = curve::readSvg(kDoc, o);
    CHECK(doc.warnings.empty());
    REQUIRE(doc.shapes.size() == 6);
    std::map<std::string, const curve::SvgShape*> by;
    for (const curve::SvgShape& s : doc.shapes)
        by[s.id] = &s;
    REQUIRE(by.count("box"));
    const curve::Spline& box = by["box"]->curves[0];
    CHECK(box.closed);
    CHECK(box.kind == curve::Kind::Bezier);
    CHECK(box.size() == 4);
    // Straight sides keep vector handles; y flips so the drawing reads upright. The viewBox maps 200 x 100 units onto
    // 400 x 200 px, so every length doubles.
    for (curve::HandleType t : box.typeR)
        CHECK(t == curve::HandleType::Vector);
    CHECK(std::abs(area(box)) == doctest::Approx(4 * 800.0).epsilon(1e-6));
    D3 lo, hi;
    curve::bounds(box, lo, hi);
    CHECK(hi.y == doctest::Approx(-20.0));
    CHECK(lo.y == doctest::Approx(-60.0));
    // The circle is close to exact (nanosvg's cubic arcs).
    CHECK(std::abs(area(by["dot"]->curves[0])) == doctest::Approx(4 * glm::pi<double>() * 400.0).epsilon(1e-3));
    // The transformed triangle keeps its area.
    CHECK(std::abs(area(by["tri"]->curves[0])) == doctest::Approx(4 * 200.0).epsilon(1e-4));
    CHECK(!by["arc"]->curves[0].closed);
    CHECK(!by["zig"]->curves[0].closed);
    CHECK(std::abs(area(by["egg"]->curves[0])) == doctest::Approx(4 * glm::pi<double>() * 120.0).epsilon(1e-3));

    // On the ground plane, centred.
    o.plane = curve::SvgPlane::XZ;
    o.center = true;
    doc = curve::readSvg(kDoc, o);
    D3 a(1e300), b(-1e300);
    for (const curve::SvgShape& s : doc.shapes)
        for (const curve::Spline& sp : s.curves)
            for (const D3& p : curve::tessellate(sp).points)
            {
                a = glm::min(a, p);
                b = glm::max(b, p);
            }
    CHECK(a.y == doctest::Approx(0.0));
    CHECK(b.y == doctest::Approx(0.0));
    CHECK(std::abs(a.x + b.x) < 1.0);
    CHECK(std::abs(a.z + b.z) < 1.0);

    CHECK(!curve::readSvg("not svg at all").warnings.empty());
}

TEST_CASE("svg: import and export through the translator round trip")
{
    run("newScene");
    auto dir = std::filesystem::temp_directory_path() / "lila_curve_tests";
    std::filesystem::create_directories(dir);
    std::string in = (dir / "shapes.svg").string(), out = (dir / "out.svg").string();
    {
        std::ofstream f(in);
        f << kDoc;
    }
    CHECK(io::translators::forPath(io::translators::Op::Import, in) != nullptr);
    json names = io::translators::run(io::translators::Op::Import, in, {{"scale", 0.5}});
    REQUIRE(names.size() == 6);
    json box = run("curveData", {{"args", {names[0]}}, {"worldSpace", true}});
    REQUIRE(box.size() == 1);
    run("undo");
    CHECK(!run("objExists", {{"args", {names[0]}}}).get<bool>());
    run("redo");
    run("exportSvg", {{"args", {out, names[0], names[1]}}, {"scale", 0.5}});
    curve::SvgImportOptions o;
    o.scale = 0.5;
    o.center = false;
    std::ifstream f(out);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    curve::SvgDocument back = curve::readSvg(text, o);
    REQUIRE(back.shapes.size() == 2);
    CHECK(std::abs(area(back.shapes[0].curves[0])) == doctest::Approx(800.0).epsilon(1e-4));
    CHECK(std::abs(area(back.shapes[1].curves[0])) == doctest::Approx(glm::pi<double>() * 400.0).epsilon(1e-3));
}
