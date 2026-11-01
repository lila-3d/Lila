#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "curve/Strap.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace strap_test
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

json with(json base, const json& extra)
{
    base.update(extra);
    return base;
}

geo::Mesh cubeAt(double size, const D3& at)
{
    geo::Mesh m = geo::makeCube({.width = size, .height = size, .depth = size});
    for (D3& p : m.positions())
        p += at;
    return m;
}

/** A wall 100 high round a U: a 100 square with a notch 40 wide and 50 deep in its +z side. */
geo::Mesh notchedPrism()
{
    const std::vector<glm::dvec2> outline = {{-50, -50}, {50, -50}, {50, 50}, {20, 50}, {20, 0}, {-20, 0}, {-20, 50}, {-50, 50}};
    std::vector<D3> points;
    std::vector<geo::Index> sizes, idx;
    for (const glm::dvec2& p : outline)
    {
        points.emplace_back(p.x, -50, p.y);
        points.emplace_back(p.x, 50, p.y);
    }
    for (geo::Index k = 0; k < outline.size(); ++k)
    {
        const geo::Index j = (k + 1) % geo::Index(outline.size());
        sizes.push_back(4);
        for (geo::Index c : {2 * k, 2 * j, 2 * j + 1, 2 * k + 1})
            idx.push_back(c);
    }
    return geo::Mesh::fromPolygons(points, sizes, idx);
}

/** A level slash across the view from the front, at height y: its plane is horizontal. */
curve::StrapSlash frontSlash(double y = 0.0)
{
    return {.start = D3(-80, y, 300), .startDir = D3(0, 0, -1), .end = D3(80, y, 300), .endDir = D3(0, 0, -1)};
}

geo::Bounds meshBounds(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    REQUIRE(m->numFaces() > 0);
    return m->bounds();
}
} // namespace strap_test

using strap_test::cubeAt;
using strap_test::frontSlash;
using strap_test::meshBounds;
using strap_test::notchedPrism;
using strap_test::with;

TEST_CASE("strap path: a slash round a box, bundles, bridges and open cuts")
{
    geo::Mesh box = cubeAt(100.0, D3(0.0));
    std::vector<geo::SectionMesh> one = {{&box}};
    curve::StrapPathResult r = curve::strapPath(one, frontSlash());
    REQUIRE(r.path);
    CHECK(r.loops == 1);
    CHECK(r.path->closed);
    CHECK(curve::tessellate(*r.path).length == doctest::Approx(400.08).epsilon(1e-6));
    CHECK(std::abs(glm::dot(r.up, D3(0, 1, 0))) == doctest::Approx(1.0));

    // A gap lifts it off the surface all round.
    r = curve::strapPath(one, frontSlash(), {.gap = 1.0});
    REQUIRE(r.path);
    for (const D3& p : curve::tessellate(*r.path).points)
        CHECK(glm::length(glm::dvec2(std::max(std::abs(p.x) - 50.0, 0.0), std::max(std::abs(p.z) - 50.0, 0.0))) >= 1.0 - 0.02);

    // The slash misses: no path, and why.
    r = curve::strapPath(one, frontSlash(200.0));
    CHECK_FALSE(r.path);
    CHECK_FALSE(r.warning.empty());

    // Two boxes crossed by one slash are bundled: the strap goes round both.
    geo::Mesh a = cubeAt(40.0, D3(-30, 0, 0)), b = cubeAt(40.0, D3(30, 0, 0));
    std::vector<geo::SectionMesh> two = {{&a}, {&b}};
    r = curve::strapPath(two, frontSlash());
    REQUIRE(r.path);
    CHECK(r.loops == 2);
    CHECK(curve::tessellate(*r.path).length == doctest::Approx(280.08).epsilon(1e-6));
    // A slash over only one of them straps that one.
    r = curve::strapPath(two, {.start = D3(5, 0, 300), .startDir = D3(0, 0, -1), .end = D3(80, 0, 300), .endDir = D3(0, 0, -1)});
    REQUIRE(r.path);
    CHECK(r.loops == 1);
    CHECK(curve::tessellate(*r.path).length == doctest::Approx(160.08).epsilon(1e-6));

    // A notch narrower than twice the bridge is spanned straight across; a wider one is followed, and taut spans any.
    geo::Mesh notched = notchedPrism();
    std::vector<geo::SectionMesh> u = {{&notched}};
    auto lengthWith = [&](const curve::StrapPathOptions& o) {
        curve::StrapPathResult res = curve::strapPath(u, frontSlash(), o);
        INFO(res.warning);
        REQUIRE(res.path);
        return curve::tessellate(*res.path).length;
    };
    CHECK(lengthWith({}) == doctest::Approx(500.08).epsilon(1e-6));
    CHECK(lengthWith({.bridge = 15.0}) == doctest::Approx(500.08).epsilon(1e-6));
    CHECK(lengthWith({.bridge = 25.0}) == doctest::Approx(400.08).epsilon(1e-6));
    CHECK(lengthWith({.taut = true}) == doctest::Approx(400.08).epsilon(1e-6));

    // Through an open mesh the strap follows the open cut, outside toward the eye.
    geo::Mesh plane = geo::makePlane({.width = 100.0, .depth = 100.0});
    std::vector<geo::SectionMesh> flat = {{&plane}};
    curve::StrapSlash fromAbove{.start = D3(-80, 300, 0), .startDir = D3(0, -1, 0), .end = D3(80, 300, 0), .endDir = D3(0, -1, 0)};
    r = curve::strapPath(flat, fromAbove);
    REQUIRE(r.path);
    CHECK_FALSE(r.path->closed);
    curve::StrapOptions o;
    o.up = r.up;
    geo::Mesh m = curve::makeStrap(*r.path, o).mesh;
    CHECK(m.bounds().min.y == doctest::Approx(0.01).epsilon(1e-6));
    CHECK(m.bounds().max.y == doctest::Approx(0.01 + o.thickness).epsilon(1e-6));
}

TEST_CASE("strap path clears what rises under its width")
{
    // A lump on the box's side, just above the slash's plane: the centre misses it, the strap's edge would cut through it.
    geo::Mesh box = cubeAt(100.0, D3(0.0)), lump = cubeAt(10.0, D3(52, 6, 0));
    std::vector<geo::SectionMesh> both = {{&box}, {&lump}};
    auto maxX = [](const curve::StrapPathResult& r) {
        REQUIRE(r.path);
        double x = -1e9;
        for (const D3& p : r.path->points)
            x = std::max(x, p.x);
        return x;
    };
    CHECK(maxX(curve::strapPath(both, frontSlash())) == doctest::Approx(50.01));
    CHECK(maxX(curve::strapPath(both, frontSlash(), {.width = 4.0})) == doctest::Approx(57.01));
    // Narrower than the gap to it, it still runs flat on the box.
    CHECK(maxX(curve::strapPath(both, frontSlash(), {.width = 1.5})) == doctest::Approx(50.01));
}

TEST_CASE("thin tape slanted round a faceted cylinder stays outside it")
{
    geo::Mesh cyl = geo::makeCylinder({.radius = 30.0, .height = 100.0, .subdivAxis = 24});
    std::vector<geo::SectionMesh> one = {{&cyl}};
    const curve::StrapSlash slant{.start = D3(-80, -20, 300), .startDir = D3(0, 0, -1), .end = D3(80, 20, 300), .endDir = D3(0, 0, -1)};
    curve::StrapPathResult r = curve::strapPath(one, slant, {.gap = 0.0, .width = 5.0});
    REQUIRE(r.path);
    curve::StrapOptions o;
    o.width = 5.0;
    o.thickness = 0.04;
    geo::Mesh tape = curve::makeStrap(*r.path, o).mesh;
    REQUIRE(tape.numPoints() > 0);
    // Outside a convex mesh: in front of at least one of its faces.
    int inside = 0;
    for (const D3& p : tape.positions())
    {
        bool out = false;
        for (geo::Index f = 0; f < cyl.faceCapacity() && !out; ++f)
            if (cyl.faceValid(f))
                out = glm::dot(p - cyl.faceCentroid(f), glm::normalize(cyl.faceAreaVector(f))) >= -1e-6;
        inside += out ? 0 : 1;
    }
    CHECK(inside == 0);
}

TEST_CASE("rope keeps rings along long straight runs")
{
    geo::Mesh box = cubeAt(100.0, D3(0.0));
    std::vector<geo::SectionMesh> one = {{&box}};
    curve::StrapPathResult r = curve::strapPath(one, frontSlash());
    REQUIRE(r.path);
    curve::StrapOptions o;
    o.style = curve::StrapStyle::Rope;
    o.width = 2.5;
    o.lay = 7.0;
    geo::Mesh m = curve::makeStrap(*r.path, o).mesh;
    // Rings no further apart than a twelfth of a turn of the strands.
    const double ringsAround = 400.0 / (7.0 / 12.0);
    CHECK(double(m.numPoints()) >= ringsAround * o.strands * o.strandSides * 0.95);
}

TEST_CASE("strap mesh: band on the surface, rope, buckle")
{
    geo::Mesh box = cubeAt(100.0, D3(0.0));
    std::vector<geo::SectionMesh> one = {{&box}};
    curve::StrapPathResult r = curve::strapPath(one, frontSlash());
    REQUIRE(r.path);

    curve::StrapOptions o;
    o.width = 4.0;
    o.thickness = 0.5;
    curve::StrapResult band = curve::makeStrap(*r.path, o);
    CHECK(band.warnings.empty());
    geo::Bounds bb = band.mesh.bounds();
    // The underside sits on the box, the band stands off it by its thickness and spans its width.
    CHECK(bb.max.x == doctest::Approx(50.51).epsilon(1e-6));
    CHECK(bb.min.z == doctest::Approx(-50.51).epsilon(1e-6));
    CHECK(bb.max.y - bb.min.y == doctest::Approx(4.0).epsilon(1e-6));
    for (const D3& p : band.mesh.positions())
        CHECK(std::max(std::abs(p.x), std::abs(p.z)) >= 50.01 - 1e-6);

    o.flip = true;
    CHECK(curve::makeStrap(*r.path, o).mesh.bounds().max.x == doctest::Approx(50.01).epsilon(1e-6));
    o.flip = false;

    o.style = curve::StrapStyle::Rope;
    o.width = 3.0;
    curve::StrapResult rope = curve::makeStrap(*r.path, o);
    geo::Bounds rb = rope.mesh.bounds();
    CHECK(rb.max.x <= 53.0 + 0.05);
    CHECK(rb.max.x > 52.3);
    CHECK(rope.mesh.numFaces() > band.mesh.numFaces());

    o.style = curve::StrapStyle::Band;
    o.width = 4.0;
    o.buckle = true;
    o.buckleAt = 0.125;
    curve::StrapResult buckled = curve::makeStrap(*r.path, o);
    CHECK(buckled.mesh.numFaces() == band.mesh.numFaces() + 6);
    auto part = buckled.mesh.faceAttrs().get<int32_t>("strapPart");
    int buckleFaces = 0;
    for (geo::Index f = 0; f < buckled.mesh.faceCapacity(); ++f)
        if (buckled.mesh.faceValid(f) && part[f] == int32_t(curve::StrapPart::Buckle))
            ++buckleFaces;
    CHECK(buckleFaces == 6);
}

TEST_CASE("strap command: live history, presets, baked")
{
    strap_test::run("newScene");
    std::string box = strap_test::run("polyCube", {{"width", 100}, {"height", 100}, {"depth", 100}})[0];
    const json slash = {{"args", {box}}, {"start", {-80, 0, 300}}, {"end", {80, 0, 300}}, {"view", {0, 0, -1}}};

    json made = strap_test::run("strap", with(slash, {{"preset", "strap"}, {"buckle", false}}));
    REQUIRE(made.size() == 4);
    std::string mesh = made[0], curve = made[1], op = made[2], cut = made[3];
    geo::Bounds b = meshBounds(mesh);
    CHECK(b.max.x == doctest::Approx(50.36).epsilon(1e-4));
    CHECK(b.max.y - b.min.y == doctest::Approx(4.0).epsilon(1e-4));

    // The strap follows the box.
    strap_test::run("move", {{"args", {box}}, {"by", {0, 0, 1000}}});
    CHECK(meshBounds(mesh).center().z == doctest::Approx(1000.0).epsilon(1e-3));
    strap_test::run("move", {{"args", {box}}, {"by", {0, 0, -1000}}});

    strap_test::run("setAttr", {{"args", {cut + ".gap", 2.0}}});
    CHECK(meshBounds(mesh).max.x == doctest::Approx(52.36).epsilon(1e-4));
    strap_test::run("setAttr", {{"args", {cut + ".gap", 0.0}}});

    strap_test::run("strapPreset", {{"args", {mesh}}, {"preset", "rope"}});
    scene::Graph& g = scene::Graph::current();
    CHECK(g.find(op)->as<int64_t>(g.find(op)->type().attrIndex("style")) == 1);
    CHECK(meshBounds(mesh).max.x == doctest::Approx(52.5).epsilon(0.01));
    // Tape lifts off a hair so its underside doesn't flicker into the surface; switching away drops the gap.
    run("strapPreset", {{"args", {mesh}}, {"preset", "tape"}});
    CHECK(g.find(cut)->as<double>(g.find(cut)->type().attrIndex("gap")) == doctest::Approx(0.02));
    run("strapPreset", {{"args", {mesh}}, {"preset", "strap"}});
    CHECK(g.find(cut)->as<double>(g.find(cut)->type().attrIndex("gap")) == doctest::Approx(0.0));

    json baked = strap_test::run("strap", with(slash, {{"preset", "tape"}, {"constructionHistory", false}}));
    CHECK(baked[2].is_null());
    CHECK(baked[3].is_null());
    CHECK(meshBounds(baked[0].get<std::string>()).max.x == doctest::Approx(50.07).epsilon(1e-4));

    CHECK_THROWS_AS(strap_test::run("strap", with(slash, {{"start", {0, 300, 300}}, {"end", {0, 400, 300}}})), CommandError);
    CHECK_THROWS_AS(strap_test::run("strap", {{"args", {box}}, {"start", {0, 0, 0}}}), CommandError);
    CHECK_THROWS_AS(strap_test::run("strap", with(slash, {{"preset", "chain"}})), CommandError);
}

TEST_CASE("strap command takes visibility and the selection: hidden meshes, selected faces")
{
    strap_test::run("newScene");
    std::string a = strap_test::run("polyCube", {{"width", 40}, {"height", 40}, {"depth", 40}})[0];
    std::string b = strap_test::run("polyCube", {{"width", 40}, {"height", 40}, {"depth", 40}})[0];
    strap_test::run("move", {{"args", {a}}, {"by", {-100, 0, 0}}});
    strap_test::run("move", {{"args", {b}}, {"by", {100, 0, 0}}});
    const json slash = {{"start", {-200, 0, 300}}, {"end", {200, 0, 300}}, {"view", {0, 0, -1}}};
    scene::Graph& g = scene::Graph::current();
    auto loopsOf = [&](const json& made) {
        g.evaluate();
        scene::Node* cut = g.find(made[3].get<std::string>());
        REQUIRE(cut);
        return cut->as<int64_t>(cut->type().attrIndex("loops"));
    };
    auto pathOf = [&](const json& made) {
        g.evaluate();
        const curve::Splines& sp = curve::splinesOf(g.find(made[1].get<std::string>())->value(curve::CurveShape::kWorldSpace));
        REQUIRE(sp.size() == 1);
        return sp.front();
    };

    // Nothing selected: every visible mesh, so both boxes are bundled.
    strap_test::run("select", {{"args", json::array()}});
    CHECK(loopsOf(strap_test::run("strap", slash)) == 2);
    // A hidden box is left out, selected or not.
    strap_test::run("hide", {{"args", {b}}});
    strap_test::run("select", {{"args", json::array()}});
    CHECK(loopsOf(strap_test::run("strap", slash)) == 1);
    strap_test::run("select", {{"args", {a, b}}});
    CHECK(loopsOf(strap_test::run("strap", slash)) == 1);
    strap_test::run("show", {{"all", true}});

    // Selected faces: only the +x side of box a, so the strap runs across that face alone.
    strap_test::run("select", {{"args", json::array()}});
    scene::Node* na = g.find(a);
    scene::MeshPtr m = na->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    scene::components::Ids side;
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f) && m->faceCentroid(f).x > 19.0)
            side.push_back(f);
    REQUIRE(side.size() == 1);
    scene::components::setMode(scene::ComponentMode::Face);
    scene::components::select(na->id(), scene::ComponentMode::Face, side);
    json made = strap_test::run("strap", slash);
    curve::Spline across = pathOf(made);
    CHECK_FALSE(across.closed);
    for (const D3& p : across.points)
        CHECK(p.x - (-100.0) == doctest::Approx(20.01).epsilon(1e-6));
    // Seen edge on, its outside still comes from the face, not the eye.
    curve::StrapOptions bandOpts;
    scene::Node* op = g.find(made[2].get<std::string>());
    bandOpts.up = op->as<D3>(op->type().attrIndex("up"));
    CHECK(curve::makeStrap(across, bandOpts).mesh.bounds().min.x > -80.0);
    // The faces stay with the strap: moving box a carries it along.
    scene::components::setMode(scene::ComponentMode::Object);
    strap_test::run("move", {{"args", {a}}, {"by", {0, 0, 10}}});
    CHECK_FALSE(pathOf(made).closed);

    // Faces given to the command win: null is the whole mesh.
    made = strap_test::run("strap", with(slash, {{"args", {a}}, {"faces", {nullptr}}}));
    CHECK(pathOf(made).closed);
    CHECK_THROWS_AS(strap_test::run("strap", with(slash, {{"args", {a}}, {"faces", {nullptr, nullptr}}})), CommandError);
}
