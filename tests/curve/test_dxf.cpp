#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "curve/Dxf.h"
#include "curve/Planar.h"
#include "curve/Tessellate.h"
#include "io/FileTranslators.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <glm/gtc/constants.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

using namespace lila;
using json = nlohmann::json;
using D2 = glm::dvec2;
using D3 = glm::dvec3;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

using Pairs = std::vector<std::pair<int, std::string>>;

std::string ascii(const Pairs& entities, const Pairs& blocks = {}, const Pairs& header = {})
{
    std::string s;
    auto add = [&](const Pairs& ps) {
        for (const auto& [c, v] : ps)
            s += std::to_string(c) + "\n" + v + "\n";
    };
    add({{0, "SECTION"}, {2, "HEADER"}});
    add(header);
    add({{0, "ENDSEC"}});
    if (!blocks.empty())
    {
        add({{0, "SECTION"}, {2, "BLOCKS"}});
        add(blocks);
        add({{0, "ENDSEC"}});
    }
    add({{0, "SECTION"}, {2, "ENTITIES"}});
    add(entities);
    add({{0, "ENDSEC"}, {0, "EOF"}});
    return s;
}

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

const curve::DxfLayer* layerNamed(const curve::DxfDocument& d, const std::string& name)
{
    for (const curve::DxfLayer& l : d.layers)
        if (l.name == name)
            return &l;
    return nullptr;
}
} // namespace

TEST_CASE("dxf: curves come in exact")
{
    curve::DxfDocument d = curve::readDxf(ascii(
        {
            {0, "LINE"}, {8, "Lines"}, {10, "0"}, {20, "0"}, {30, "0"}, {11, "10"}, {21, "0"}, {31, "5"},
            {0, "CIRCLE"}, {8, "Round"}, {10, "100"}, {20, "0"}, {30, "0"}, {40, "10"},
            {0, "ARC"}, {8, "Round"}, {10, "0"}, {20, "0"}, {30, "0"}, {40, "5"}, {50, "0"}, {51, "90"},
            {0, "ELLIPSE"}, {8, "Round"}, {10, "0"}, {20, "50"}, {30, "0"}, {11, "20"}, {21, "0"}, {31, "0"}, {40, "0.5"},
            {41, "0"}, {42, "6.283185307179586"},
            // A 10 x 10 slot: two straight sides and two half circles (bulge 1).
            {0, "LWPOLYLINE"}, {8, "Slot"}, {90, "4"}, {70, "1"}, {10, "0"}, {20, "0"}, {10, "10"}, {20, "0"}, {42, "1"},
            {10, "10"}, {20, "10"}, {10, "0"}, {20, "10"}, {42, "1"},
        },
        {}, {{9, "$INSUNITS"}, {70, "4"}}));
    REQUIRE(d.ok());
    CHECK(d.unitScale == doctest::Approx(0.1));
    CHECK(d.warnings.empty());
    REQUIRE(d.layers.size() == 3);

    const curve::DxfLayer* lines = layerNamed(d, "Lines");
    REQUIRE(lines);
    REQUIRE(lines->curves.size() == 1);
    CHECK(lines->curves[0].kind == curve::Kind::Poly);
    CHECK(lines->curves[0].points[1].z == doctest::Approx(5.0));

    const curve::DxfLayer* round = layerNamed(d, "Round");
    REQUIRE(round);
    REQUIRE(round->curves.size() == 3);
    const curve::Spline& c = round->curves[0];
    CHECK(c.kind == curve::Kind::Nurbs);
    CHECK(c.closed);
    for (double u = 0.0; u < curve::spanCount(c); u += 0.37)
        CHECK(glm::length(curve::pointAt(c, u) - D3(100, 0, 0)) == doctest::Approx(10.0).epsilon(1e-9));
    const curve::Spline& a = round->curves[1];
    CHECK(glm::length(curve::pointAt(a, 0.0) - D3(5, 0, 0)) < 1e-9);
    CHECK(glm::length(curve::pointAt(a, curve::spanCount(a)) - D3(0, 5, 0)) < 1e-9);
    CHECK(std::abs(area(round->curves[2])) == doctest::Approx(glm::pi<double>() * 20.0 * 10.0).epsilon(1e-4));

    const curve::DxfLayer* slot = layerNamed(d, "Slot");
    REQUIRE(slot);
    REQUIRE(slot->curves.size() == 1);
    CHECK(slot->curves[0].closed);
    CHECK(std::abs(area(slot->curves[0])) == doctest::Approx(100.0 + glm::pi<double>() * 25.0).epsilon(1e-4));
}

TEST_CASE("dxf: splines keep their knots and weights and rasterize within tolerance")
{
    // A quarter circle as a rational quadratic: the weight is what makes it round.
    curve::DxfDocument d = curve::readDxf(ascii({
        {0, "SPLINE"}, {8, "0"}, {70, "4"}, {71, "2"}, {72, "6"}, {73, "3"},
        {40, "0"}, {40, "0"}, {40, "0"}, {40, "1"}, {40, "1"}, {40, "1"},
        {10, "10"}, {20, "0"}, {30, "0"}, {41, "1"},
        {10, "10"}, {20, "10"}, {30, "0"}, {41, "0.70710678118654757"},
        {10, "0"}, {20, "10"}, {30, "0"}, {41, "1"},
        // Fit points only: the curve passes through every one.
        {0, "SPLINE"}, {8, "0"}, {70, "0"}, {71, "3"}, {72, "0"}, {73, "0"}, {74, "4"},
        {11, "0"}, {21, "0"}, {31, "0"}, {11, "10"}, {21, "5"}, {31, "0"}, {11, "20"}, {21, "0"}, {31, "0"}, {11, "30"}, {21, "5"}, {31, "0"},
    }));
    REQUIRE(d.ok());
    REQUIRE(d.layers.size() == 1);
    REQUIRE(d.layers[0].curves.size() == 2);
    const curve::Spline& q = d.layers[0].curves[0];
    CHECK(q.kind == curve::Kind::Nurbs);
    CHECK(q.degree == 2);
    CHECK(q.knots.size() == 6);
    CHECK(q.weights.size() == 3);
    for (double u = 0.0; u <= 1.0; u += 0.125)
        CHECK(glm::length(curve::pointAt(q, u)) == doctest::Approx(10.0).epsilon(1e-9));

    const double tol = 0.01;
    curve::Spline poly = curve::convert(q, curve::Kind::Poly, tol);
    CHECK(poly.kind == curve::Kind::Poly);
    CHECK(poly.size() > 4);
    for (size_t k = 0; k + 1 < poly.size(); ++k)
    {
        D3 mid = (poly.points[k] + poly.points[k + 1]) * 0.5;
        CHECK(10.0 - glm::length(mid) <= tol * 1.01);
    }

    const curve::Spline& f = d.layers[0].curves[1];
    const int spans = curve::spanCount(f);
    for (D3 p : {D3(0, 0, 0), D3(10, 5, 0), D3(20, 0, 0), D3(30, 5, 0)})
    {
        double best = 1e300;
        for (int k = 0; k <= 4000; ++k)
            best = std::min(best, glm::length(curve::pointAt(f, spans * k / 4000.0) - p));
        CHECK(best < 0.02);
    }
}

TEST_CASE("dxf: blocks expand at their inserts and faces come in as meshes")
{
    curve::DxfDocument d = curve::readDxf(ascii(
        {
            {0, "INSERT"}, {8, "Parts"}, {2, "Tri"}, {10, "100"}, {20, "0"}, {30, "0"}, {41, "2"}, {42, "2"}, {43, "2"}, {50, "90"},
            {0, "INSERT"}, {8, "Parts"}, {2, "Tri"}, {10, "0"}, {20, "0"}, {30, "0"}, {70, "3"}, {44, "50"},
            {0, "POLYLINE"}, {8, "Box"}, {66, "1"}, {70, "64"}, {71, "4"}, {72, "1"},
            {0, "VERTEX"}, {8, "Box"}, {10, "0"}, {20, "0"}, {30, "0"}, {70, "192"},
            {0, "VERTEX"}, {8, "Box"}, {10, "1"}, {20, "0"}, {30, "0"}, {70, "192"},
            {0, "VERTEX"}, {8, "Box"}, {10, "1"}, {20, "1"}, {30, "0"}, {70, "192"},
            {0, "VERTEX"}, {8, "Box"}, {10, "0"}, {20, "1"}, {30, "0"}, {70, "192"},
            {0, "VERTEX"}, {8, "Box"}, {10, "0"}, {20, "0"}, {30, "0"}, {70, "128"}, {71, "1"}, {72, "2"}, {73, "3"}, {74, "4"},
            {0, "SEQEND"}, {8, "Box"},
            {0, "MESH"}, {8, "Box"}, {100, "AcDbSubDMesh"}, {71, "2"}, {72, "0"}, {91, "0"}, {92, "3"},
            {10, "5"}, {20, "0"}, {30, "0"}, {10, "6"}, {20, "0"}, {30, "0"}, {10, "5"}, {20, "1"}, {30, "0"},
            {93, "4"}, {90, "3"}, {90, "0"}, {90, "1"}, {90, "2"},
            {0, "TEXT"}, {8, "Notes"}, {1, "hello"},
        },
        {
            {0, "BLOCK"}, {8, "0"}, {2, "Tri"}, {70, "0"}, {10, "1"}, {20, "0"}, {30, "0"},
            {0, "3DFACE"}, {8, "0"}, {10, "1"}, {20, "0"}, {30, "0"}, {11, "2"}, {21, "0"}, {31, "0"}, {12, "1"}, {22, "1"}, {32, "0"},
            {13, "1"}, {23, "1"}, {33, "0"},
            {0, "ENDBLK"},
        }));
    REQUIRE(d.ok());
    const curve::DxfLayer* parts = layerNamed(d, "Parts");
    REQUIRE(parts);
    // One scaled, rotated insert plus a row of three; layer 0 entities take the insert's layer.
    CHECK(parts->mesh.numFaces() == 4);
    bool rotated = false;
    for (glm::dvec3 p : parts->mesh.positions())
        rotated = rotated || glm::length(p - D3(100, 2, 0)) < 1e-9;
    CHECK(rotated);
    bool third = false;
    for (glm::dvec3 p : parts->mesh.positions())
        third = third || glm::length(p - D3(101, 0, 0)) < 1e-9;
    CHECK(third);

    const curve::DxfLayer* box = layerNamed(d, "Box");
    REQUIRE(box);
    CHECK(box->mesh.numFaces() == 2);
    CHECK(box->mesh.numPoints() == 7);
    CHECK(!layerNamed(d, "Notes"));
    CHECK(d.warnings.size() == 1);
}

TEST_CASE("dxf: what Lila writes reads back")
{
    curve::Spline c = curve::circle(D3(0.0), D3(0, 0, 1), 10.0);
    curve::Spline p;
    p.points = {D3(0, 0, 0), D3(10, 0, 0), D3(10, 10, 3)};
    geo::Mesh m = geo::Mesh::fromPolygons(std::vector<D3>{D3(0, 0, 0), D3(1, 0, 0), D3(1, 1, 0), D3(0.5, 1.5, 0), D3(0, 1, 0)},
                                          std::vector<geo::Index>{5}, std::vector<geo::Index>{0, 1, 2, 3, 4});
    std::vector<curve::DxfObject> objs{{.layer = "Ring", .curves = {c, p}}, {.layer = "Plate", .mesh = &m}};
    for (curve::DxfVersion v : {curve::DxfVersion::R2000, curve::DxfVersion::R12})
    {
        curve::DxfWriteOptions o;
        o.version = v;
        o.unit = "cm";
        o.tolerance = 0.001;
        curve::DxfDocument d = curve::readDxf(curve::writeDxf(objs, o));
        REQUIRE(d.ok());
        CHECK(d.warnings.empty());
        CHECK(d.unitScale == doctest::Approx(1.0));
        const curve::DxfLayer* ring = layerNamed(d, "Ring");
        REQUIRE(ring);
        REQUIRE(ring->curves.size() == 2);
        CHECK(std::abs(area(ring->curves[0])) == doctest::Approx(glm::pi<double>() * 100.0).epsilon(1e-3));
        if (v == curve::DxfVersion::R2000)
            CHECK(ring->curves[0].kind == curve::Kind::Nurbs);
        CHECK(ring->curves[1].points.back().z == doctest::Approx(3.0));
        const curve::DxfLayer* plate = layerNamed(d, "Plate");
        REQUIRE(plate);
        CHECK(plate->mesh.numFaces() == 3);
        CHECK(plate->mesh.numPoints() == 5);
    }
}

TEST_CASE("dxf: binary files read")
{
    std::string b("AutoCAD Binary DXF\r\n\x1a\0", 22);
    auto code = [&](int c) {
        int16_t v = int16_t(c);
        b.append(reinterpret_cast<const char*>(&v), 2);
    };
    auto str = [&](int c, const char* s) {
        code(c);
        b.append(s, std::strlen(s) + 1);
    };
    auto dbl = [&](int c, double v) {
        code(c);
        b.append(reinterpret_cast<const char*>(&v), 8);
    };
    str(0, "SECTION");
    str(2, "ENTITIES");
    str(0, "LINE");
    str(8, "0");
    code(62);
    b.append("\x01\x00", 2);
    dbl(10, 1.0);
    dbl(20, 2.0);
    dbl(30, 0.0);
    dbl(11, 4.0);
    dbl(21, 6.0);
    dbl(31, 0.0);
    str(0, "ENDSEC");
    str(0, "EOF");
    curve::DxfDocument d = curve::readDxf(b);
    REQUIRE(d.ok());
    REQUIRE(d.layers.size() == 1);
    REQUIRE(d.layers[0].curves.size() == 1);
    CHECK(glm::length(d.layers[0].curves[0].points[1] - D3(4, 6, 0)) < 1e-12);
    CHECK(!curve::readDxf("not a dxf").ok());
}

TEST_CASE("dxf: import as polylines and export through the translators")
{
    run("newScene");
    auto dir = std::filesystem::temp_directory_path() / "lila_curve_tests";
    std::filesystem::create_directories(dir);
    std::string in = (dir / "plan.dxf").string(), out = (dir / "plan_out.dxf").string();
    {
        std::ofstream f(in, std::ios::binary);
        f << ascii({{0, "CIRCLE"}, {8, "Holes"}, {10, "0"}, {20, "0"}, {30, "0"}, {40, "100"},
                    {0, "LINE"}, {8, "Edges"}, {10, "0"}, {20, "0"}, {30, "0"}, {11, "100"}, {21, "0"}, {31, "0"}},
                   {}, {{9, "$INSUNITS"}, {70, "4"}});
    }
    CHECK(io::translators::forPath(io::translators::Op::Import, in) != nullptr);
    json names = run("importDxf", {{"args", {in}}, {"polylines", true}, {"tolerance", 0.001}});
    REQUIRE(names.size() == 2);
    json hole = run("curveData", {{"args", {names[0]}}, {"worldSpace", true}});
    REQUIRE(hole.size() == 1);
    CHECK(hole[0]["type"] == "poly");
    CHECK(hole[0]["points"].size() > 16);
    // 100 mm in the file is 10 cm in the scene.
    for (const json& q : hole[0]["points"])
        CHECK(glm::length(D3(q[0].get<double>(), q[1].get<double>(), q[2].get<double>())) == doctest::Approx(10.0).epsilon(1e-3));
    run("undo");
    CHECK(!run("objExists", {{"args", {names[0]}}}).get<bool>());
    run("redo");

    run("exportDxf", {{"args", {out, names[0], names[1]}}});
    std::ifstream f(out, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    curve::DxfDocument back = curve::readDxf(text);
    REQUIRE(back.ok());
    CHECK(back.unitScale == doctest::Approx(0.1));
    REQUIRE(back.layers.size() == 2);
    D3 lo, hi;
    curve::bounds(back.layers[0].curves[0], lo, hi);
    CHECK(hi.x == doctest::Approx(100.0).epsilon(1e-3));
}
