#include <doctest/doctest.h>

#include "gn/Exec.h"
#include "gn/Expression.h"
#include "gn/Gn.h"
#include "geo/Mesh.h"
#include "ops/Command.h"

using namespace lila;
using json = nlohmann::json;

namespace wrangle_tests
{
using N = gn::GNode;
using L = gn::GLink;

glm::dvec4 value(const std::string& code, std::array<double, 3> abc = {0, 0, 0}, glm::dvec3 d = glm::dvec3(0.0))
{
    gn::expr::ProgramPtr p = gn::expr::compile(code, {.allowWrites = false, .needResult = true});
    gn::expr::Bindings b;
    b.count = 1;
    for (size_t k = 0; k < 3; ++k)
        b.inputs[k] = {glm::dvec4(abc[k], 0, 0, 0)};
    b.inputs[3] = {glm::dvec4(d, 0.0)};
    p->run(b);
    return b.result[0];
}

double scalar(const std::string& code, std::array<double, 3> abc = {0, 0, 0}) { return value(code, abc).x; }

gn::expr::Type resultType(const std::string& code) { return gn::expr::compile(code, {.needResult = true})->resultType(); }

int errorLine(const std::string& code, gn::expr::Options o = {})
{
    try
    {
        gn::expr::compile(code, o);
    }
    catch (const gn::expr::CompileError& e)
    {
        return e.line;
    }
    return 0;
}

int attrIndex(const gn::expr::Program& p, const std::string& name)
{
    for (size_t k = 0; k < p.attributes().size(); ++k)
        if (p.attributes()[k].name == name)
            return int(k);
    return -1;
}

gn::GeometryPtr evalNodes(std::vector<gn::GNode> nodes, std::vector<gn::GLink> links)
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    return gn::execute(*plan, {});
}

TEST_CASE("expression arithmetic and types")
{
    CHECK(scalar("1 + 2 * 3") == 7.0);
    CHECK(resultType("1 + 2 * 3") == gn::expr::Type::Int);
    CHECK(scalar("7 / 2") == 3.0);
    CHECK(scalar("7.0 / 2") == doctest::Approx(3.5));
    CHECK(resultType("7.0 / 2") == gn::expr::Type::Float);
    CHECK(scalar("7 % 3") == 1.0);
    CHECK(scalar("-2^2") == -4.0);
    CHECK(scalar("2^3^2") == 512.0);
    CHECK(scalar("1 / 0") == 0.0);
    CHECK(scalar("(1 + 2) * 3") == 9.0);
    CHECK(scalar("3 > 2 && 1 < 0") == 0.0);
    CHECK(scalar("3 > 2 || 1 < 0") == 1.0);
    CHECK(scalar("!0") == 1.0);
    CHECK(scalar("2 == 2.0") == 1.0);
    CHECK(scalar("int(3.9)") == 3.0);
    CHECK(scalar("PI") == doctest::Approx(3.14159265358979));
    CHECK(scalar("// comment\n/* block */ 4") == 4.0);
}

TEST_CASE("expression inputs, statements and loops")
{
    CHECK(scalar("a * 2 + b", {3, 1, 0}) == doctest::Approx(7.0));
    CHECK(scalar("ch(\"c\")", {0, 0, 5}) == doctest::Approx(5.0));
    CHECK(value("d * 2", {}, {1, 2, 3}).z == doctest::Approx(6.0));
    CHECK(scalar("a > 1 ? 10 : 20", {3, 0, 0}) == 10.0);
    CHECK(scalar("x = 0; for (int i = 0; i < 5; i++) x += i; x") == 10.0);
    CHECK(scalar("int i = 0; while (1) { i++; if (i >= 4) break; } i") == 4.0);
    CHECK(scalar("int n = 0; for (int i = 0; i < 6; i++) { if (i % 2 == 0) continue; n += i; } n") == 9.0);
    CHECK(scalar("float y = 1; if (y > 2) y = 5; else y = 7; y") == 7.0);
    CHECK(scalar("float a = 4; a") == 4.0);
    // A local declared in a loop body starts at zero on every pass.
    CHECK(scalar("int t = 0; for (int i = 0; i < 3; i++) { int k; k += 1; t += k; } t") == 3.0);
}

TEST_CASE("expression vectors and functions")
{
    CHECK(scalar("vector v = set(3, 4, 0); length(v)") == doctest::Approx(5.0));
    CHECK(scalar("dot(set(1, 2, 3), set(4, 5, 6))") == doctest::Approx(32.0));
    CHECK(scalar("cross(set(1, 0, 0), set(0, 1, 0)).z") == doctest::Approx(1.0));
    CHECK(scalar("vector v = 2; v.y") == 2.0);
    CHECK(scalar("vector v = 0; v.y = 3; v.y + v.x") == 3.0);
    CHECK(value("set(1, 2, 3) * 2").y == doctest::Approx(4.0));
    CHECK(value("set(1, 2, 3) * 2").w == 0.0);
    CHECK(value("normalize(set(0, 0, 5))").z == doctest::Approx(1.0));
    CHECK(scalar("distance(set(0, 0, 0), set(0, 3, 4))") == doctest::Approx(5.0));
    CHECK(scalar("fit(5, 0, 10, 0, 100)") == doctest::Approx(50.0));
    CHECK(scalar("fit(20, 0, 10, 0, 100)") == doctest::Approx(100.0));
    CHECK(scalar("smooth(0, 1, 0.5)") == doctest::Approx(0.5));
    CHECK(scalar("clamp(5, 0, 2)") == 2.0);
    CHECK(scalar("lerp(0, 10, 0.25)") == doctest::Approx(2.5));
    CHECK(scalar("min(3, 1.5)") == doctest::Approx(1.5));
    CHECK(scalar("atan2(1, 1)") == doctest::Approx(0.785398163));
    CHECK(scalar("frac(2.75)") == doctest::Approx(0.75));
    CHECK(scalar("degrees(PI)") == doctest::Approx(180.0));
    CHECK(scalar("rand(5) == rand(5)") == 1.0);
    double r = scalar("rand(set(1, 2, 3))");
    CHECK(r >= 0.0);
    CHECK(r < 1.0);
    double n = scalar("noise(set(0.3, 1.7, 2.2))");
    CHECK(n >= 0.0);
    CHECK(n <= 1.0);
    CHECK(value("set(1, 2, 3, 4)").w == 4.0);
    CHECK(resultType("set(1, 2, 3, 4)") == gn::expr::Type::Vector4);
}

TEST_CASE("expression compile errors")
{
    CHECK(errorLine("x = ;") == 1);
    CHECK(errorLine("float y = 1;\nz + 1") == 2);
    CHECK(errorLine("set(1, 2)") == 1);
    CHECK(errorLine("foo(1)") == 1);
    CHECK(errorLine("set(1, 0, 0) < 2") == 1);
    CHECK(errorLine("@N = set(0, 1, 0);") == 1);
    CHECK(errorLine("f@m = 1;\ni@m = 2;") == 2);
    CHECK(errorLine("break;") == 1);
    CHECK(errorLine("float q = 1; float q = 2;") == 1);
    CHECK(errorLine("a = 1;") == 1);
    CHECK(errorLine("@P = 0; 1", {.allowWrites = false, .needResult = true}) == 1);
    CHECK(errorLine("float x = 1;", {.needResult = true}) == 1);
    CHECK(errorLine("float v = 1; v.y") == 1);
    try
    {
        gn::expr::compile("1 +\n  )");
        FAIL("expected a compile error");
    }
    catch (const gn::expr::CompileError& e)
    {
        CHECK(e.line == 2);
        CHECK(e.column == 3);
    }
}

TEST_CASE("expression attributes and selection")
{
    gn::expr::ProgramPtr p = gn::expr::compile("@P.y += 1; i@class = @ptnum * 2; @count = @numelem;");
    int P = attrIndex(*p, "P"), cls = attrIndex(*p, "class"), cnt = attrIndex(*p, "count");
    REQUIRE(P >= 0);
    REQUIRE(cls >= 0);
    REQUIRE(cnt >= 0);
    CHECK(p->attributes()[size_t(P)].written);
    CHECK(p->attributes()[size_t(cls)].type == gn::expr::Type::Int);
    CHECK(p->attributes()[size_t(cnt)].type == gn::expr::Type::Float);
    CHECK(p->attributes()[size_t(attrIndex(*p, "ptnum"))].kind == gn::expr::AttrKind::Index);

    gn::expr::Bindings b;
    b.count = 3;
    b.attrs.resize(p->attributes().size());
    b.attrs[size_t(P)] = {glm::dvec4(0, 0, 0, 0), glm::dvec4(1, 1, 1, 0), glm::dvec4(2, 2, 2, 0)};
    b.selection = {1, 0, 1};
    p->run(b);
    CHECK(b.attrs[size_t(P)][0].y == 1.0);
    CHECK(b.attrs[size_t(P)][1].y == 1.0);
    CHECK(b.attrs[size_t(P)][2].y == 3.0);
    CHECK(b.attrs[size_t(cls)][2].x == 4.0);
    CHECK(b.attrs[size_t(cls)][1].x == 0.0);
    CHECK(b.attrs[size_t(cnt)][0].x == 3.0);

    // A typed reference anywhere types every reference.
    gn::expr::ProgramPtr q = gn::expr::compile("@dir = set(0, 1, 0); v@dir.x = 2;");
    CHECK(q->attributes()[size_t(attrIndex(*q, "dir"))].type == gn::expr::Type::Vector);

    // Enough elements to run in parallel.
    gn::expr::ProgramPtr big = gn::expr::compile("f@h = @ptnum * 0.5;");
    gn::expr::Bindings bb;
    bb.count = 10000;
    big->run(bb);
    CHECK(bb.attrs[0][9999].x == doctest::Approx(4999.5));
}

TEST_CASE("wrangle node")
{
    // The cube is 100 across, centred on the origin.
    auto moved = evalNodes({N{"C", "MeshCube", {}, {}}, N{"W", "Wrangle", {}, {{"code", "@P.y += a;"}, {"a", 2.0}}}},
                           {L{"C", "mesh", "W", "geometry"}, L{"W", "geometry", "GroupOutput", "geometry"}});
    CHECK(moved->mesh->bounds().max.y == doctest::Approx(52.0));
    CHECK(moved->mesh->bounds().min.y == doctest::Approx(-48.0));

    // Only the top points move.
    auto top = evalNodes({N{"C", "MeshCube", {}, {}}, N{"W", "Wrangle", {}, {{"code", "if (@P.y > 0) @P.y *= 2;"}}}},
                         {L{"C", "mesh", "W", "geometry"}, L{"W", "geometry", "GroupOutput", "geometry"}});
    CHECK(top->mesh->bounds().max.y == doctest::Approx(100.0));
    CHECK(top->mesh->bounds().min.y == doctest::Approx(-50.0));

    auto stored = evalNodes({N{"C", "MeshCube", {}, {}}, N{"W", "Wrangle", {}, {{"domain", "face"}, {"code", "i@side = @elemnum + 10;"}}}},
                            {L{"C", "mesh", "W", "geometry"}, L{"W", "geometry", "GroupOutput", "geometry"}});
    auto side = gn::evaluateAll<int64_t>(gn::attributeField("side", gn::SocketType::Int), gn::FieldContext::forMesh(*stored->mesh, gn::Domain::Face));
    REQUIRE(side.size() == 6);
    CHECK(side[0] == 10);
    CHECK(side[5] == 15);

    CHECK_THROWS(evalNodes({N{"C", "MeshCube", {}, {}}, N{"W", "Wrangle", {}, {{"domain", "face"}, {"code", "@P = 0;"}}}},
                           {L{"C", "mesh", "W", "geometry"}, L{"W", "geometry", "GroupOutput", "geometry"}}));
    CHECK_THROWS(evalNodes({N{"C", "MeshCube", {}, {}}, N{"W", "Wrangle", {}, {{"code", "@P.y += ;"}}}},
                           {L{"C", "mesh", "W", "geometry"}, L{"W", "geometry", "GroupOutput", "geometry"}}));
}

TEST_CASE("expression node")
{
    auto lifted = evalNodes({N{"C", "MeshCube", {}, {}}, N{"E", "Expression", {}, {{"dataType", "vector"}, {"code", "set(0, a, 0)"}, {"a", 3.0}}},
                             N{"S", "SetPosition", {}, {}}},
                            {L{"C", "mesh", "S", "geometry"}, L{"E", "value", "S", "offset"}, L{"S", "geometry", "GroupOutput", "geometry"}});
    CHECK(lifted->mesh->bounds().max.y == doctest::Approx(53.0));

    auto culled = evalNodes({N{"C", "MeshCube", {}, {}}, N{"E", "Expression", {}, {{"dataType", "bool"}, {"code", "@P.y > 0"}}},
                             N{"D", "DeleteGeometry", {}, {}}},
                            {L{"C", "mesh", "D", "geometry"}, L{"E", "value", "D", "selection"}, L{"D", "geometry", "GroupOutput", "geometry"}});
    CHECK(culled->mesh->numPoints() == 4);
    CHECK(culled->mesh->bounds().max.y == doctest::Approx(-50.0));
}

TEST_CASE("code props ask for a code editor")
{
    json types = ops::execute("gnNodeTypes", {{"detail", true}});
    auto editorOf = [&](const char* type, const char* prop) {
        for (const json& t : types)
            if (t["type"] == type)
                for (const json& p : t["props"])
                    if (p["id"] == prop)
                        return p.value("editor", std::string());
        return std::string("missing");
    };
    CHECK(editorOf("Wrangle", "code") == "code");
    CHECK(editorOf("Expression", "code") == "code");
    CHECK(editorOf("StringToCurves", "font") == "font");
    CHECK(editorOf("Wrangle", "domain").empty());
}
} // namespace wrangle_tests
