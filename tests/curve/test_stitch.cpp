#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "curve/Stitch.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <cmath>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace stitch_test
{
/** A flat surface: s along X, x along Y, out of it +Z. */
curve::StitchSurface flat()
{
    return [](double s, double x) -> std::optional<curve::SurfacePoint> { return curve::SurfacePoint{D3(s, x, 0.0), D3(0, 0, 1)}; };
}

int facesPerStitch(const curve::StitchOptions& o) { return o.segments * o.sides * o.plies; }
} // namespace stitch_test

TEST_CASE("stitch layout: counts, rows, closed loops and patterns")
{
    using stitch_test::flat;
    curve::StitchOptions o;
    // 10 cm of 0.4 cm stitches with 0.25 gaps: 15 fit, centred.
    curve::StitchResult r = curve::buildStitches(10.0, false, flat(), o);
    CHECK(r.stitches == 15);
    CHECK(r.mesh.numFaces() == 15 * stitch_test::facesPerStitch(o));
    const geo::Bounds b = r.mesh.bounds();
    CHECK(b.min.x + b.max.x == doctest::Approx(10.0).epsilon(1e-3));

    o.rows = 3;
    o.rowSpacing = 0.5;
    CHECK(curve::buildStitches(10.0, false, flat(), o).stitches == 45);
    const geo::Bounds rows = curve::buildStitches(10.0, false, flat(), o).mesh.bounds();
    CHECK(rows.max.y - rows.min.y == doctest::Approx(1.0 + o.diameter).epsilon(0.02));
    o.align = curve::StitchAlign::Left;
    CHECK(curve::buildStitches(10.0, false, flat(), o).mesh.bounds().min.y > -o.diameter);
    o.rows = 1;
    o.align = curve::StitchAlign::Center;

    // A closed path gets a whole number round, the pitch stretched to fit.
    CHECK(curve::buildStitches(10.0, true, flat(), o).stitches == 15);

    // Too short for a stitch: none, no crash.
    CHECK(curve::buildStitches(0.1, false, flat(), o).stitches == 0);

    // Cross makes two per unit, blanket a leg per unit plus a run between.
    o.pattern = curve::StitchPattern::Cross;
    CHECK(curve::buildStitches(10.0, false, flat(), o).stitches == 30);
    o.pattern = curve::StitchPattern::Blanket;
    o.length = 0.3;
    o.gap = 0.2;
    CHECK(curve::buildStitches(10.0, false, flat(), o).stitches == 21 + 20);

    // Plies multiply the tubes.
    o = {};
    o.plies = 3;
    CHECK(curve::buildStitches(10.0, false, flat(), o).mesh.numFaces() == 15 * stitch_test::facesPerStitch(o));
}

TEST_CASE("stitch thread: arches out of the surface and dives into its holes")
{
    curve::StitchOptions o;
    o.diameter = 0.1;
    o.flatten = 0.0;
    o.inset = 0.05;
    o.restHeight = 0.02;
    const curve::StitchResult r = curve::buildStitches(10.0, false, stitch_test::flat(), o);
    const geo::Bounds b = r.mesh.bounds();
    // The top of the thread: its underside at restHeight, plus its thickness.
    CHECK(b.max.z == doctest::Approx(0.02 + 0.1).epsilon(0.02));
    // The ends sink below the surface by the inset (and the thread's radius round it).
    CHECK(b.min.z < -0.05 + 1e-6);
    CHECK(b.min.z > -0.05 - 0.05 - 1e-6);

    // Flattened thread is lower.
    o.flatten = 0.5;
    CHECK(curve::buildStitches(10.0, false, stitch_test::flat(), o).mesh.bounds().max.z < b.max.z - 0.02);

    // UVs run on along the line when continuous: the far end's v is well past one stitch's.
    const auto uv = r.mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    REQUIRE_FALSE(uv.empty());
    float vMax = 0.f;
    for (const glm::vec2& t : uv)
        vMax = std::max(vMax, t.y);
    CHECK(vMax > 20.f);
    o.continuousUv = false;
    const curve::StitchResult own = curve::buildStitches(10.0, false, stitch_test::flat(), o);
    const auto uvOwn = own.mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    vMax = 0.f;
    for (const glm::vec2& t : uvOwn)
        vMax = std::max(vMax, t.y);
    CHECK(vMax < 5.f);

    // Every preset builds.
    for (const std::string& name : curve::stitchPresetNames())
    {
        INFO(name);
        const std::optional<curve::StitchOptions> p = curve::stitchPreset(name);
        REQUIRE(p);
        CHECK(curve::buildStitches(10.0, false, stitch_test::flat(), *p).stitches > 0);
    }
    CHECK_FALSE(curve::stitchPreset("nope"));
}

TEST_CASE("stitch command: live on a sphere, presets, baked")
{
    ops::execute("newScene", json::object());
    scene::Graph& g = scene::Graph::current();
    const double radius = 20.0;
    scene::Node* ball = g.createNode(*scene::types::find("mesh"), "Ball", scene::kNoNode, true);
    g.setValue({ball->id(), scene::MeshNode::kInMesh, -1},
               scene::MeshPtr(std::make_shared<geo::Mesh>(geo::makeSphere({.radius = radius, .subdivAxis = 64, .subdivHeight = 32}))));
    curve::Spline ring;
    ring.kind = curve::Kind::Poly;
    for (int k = 0; k < 48; ++k)
    {
        const double t = 6.283185307179586 * k / 48;
        ring.points.emplace_back(radius * std::cos(t), 0.0, radius * std::sin(t));
    }
    ring.closed = true;
    ring.normalize();
    scene::Node* curveNode = g.createNode(*scene::types::find("curve"), "Seam", scene::kNoNode, true);
    g.setValue({curveNode->id(), curve::CurveShape::kCreate, -1}, curve::makeCurveData({ring}));

    const json made = ops::execute("stitch", {{"args", {"Seam"}}, {"surfaces", json::array({"Ball"})}, {"preset", "heavyLeather"}, {"rows", 2}});
    REQUIRE(made.size() == 1);
    REQUIRE(made[0][1].is_string());
    g.evaluate();
    scene::Node* mesh = g.find(made[0][0].get<std::string>());
    REQUIRE(mesh);
    scene::MeshPtr m = mesh->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    REQUIRE(m->numFaces() > 0);
    // On the ball: every point within the thread's reach of its surface (the facets sit a little inside the sphere).
    double lo = 1e9, hi = 0.0;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
    {
        const double d = glm::length(m->position(p));
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    CHECK(lo > radius - 0.6);
    CHECK(hi < radius + 0.4);
    // Two rows straddle the equator.
    const geo::Bounds b = m->bounds();
    CHECK(b.max.y > 0.2);
    CHECK(b.min.y < -0.2);

    scene::Node* op = g.find(made[0][1].get<std::string>());
    REQUIRE(op);
    const int64_t before = op->as<int64_t>(op->type().attrIndex("stitchCount"));
    CHECK(before > 0);
    // Live: longer stitches mean fewer of them.
    g.setValue({op->id(), op->type().attrIndex("stitchLength"), -1}, 1.5);
    g.evaluate();
    CHECK(op->as<int64_t>(op->type().attrIndex("stitchCount")) < before);

    // No history: the mesh keeps its stitches with no node behind it.
    const json baked = ops::execute("stitch", {{"args", {"Seam"}}, {"surfaces", json::array({"Ball"})}, {"constructionHistory", false}});
    CHECK(baked[0][1].is_null());
    g.evaluate();
    scene::Node* bakedMesh = g.find(baked[0][0].get<std::string>());
    REQUIRE(bakedMesh);
    CHECK(bakedMesh->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->numFaces() > 0);

    CHECK_THROWS(ops::execute("stitch", {{"args", {"Seam"}}, {"preset", "nope"}}));
}
