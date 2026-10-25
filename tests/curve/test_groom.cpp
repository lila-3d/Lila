#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "curve/Groom.h"
#include "curve/HairCards.h"
#include "curve/Pipe.h"
#include "geo/Attribute.h"
#include "geo/ColorSets.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <filesystem>

using namespace lila;
using namespace lila::curve;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace
{
/** A 40 x 40 ground plane facing +Y. */
groom::Surface ground(std::string densitySet = {}, geo::Mesh* keep = nullptr)
{
    geo::Mesh m = geo::makePlane({40.0, 40.0, 8, 8});
    if (keep)
        *keep = m;
    return groom::Surface(std::move(m), std::move(densitySet));
}

Spline strand(const D3& root, const D3& dir, double length, int points)
{
    Spline s;
    s.kind = Kind::Poly;
    for (int i = 0; i < points; ++i)
        s.points.push_back(root + dir * (length * i / (points - 1)));
    s.normalize();
    return s;
}

/** A 5 x 5 patch of upright strands 2 cm apart around the origin. */
Splines patch(double length = 6.0, int points = 7)
{
    Splines s;
    for (int x = -2; x <= 2; ++x)
        for (int z = -2; z <= 2; ++z)
            s.push_back(strand(D3(x * 2.0, 0.0, z * 2.0), D3(0, 1, 0), length, points));
    return s;
}

std::vector<double> lengths(const Spline& s)
{
    std::vector<double> out;
    for (size_t i = 1; i < s.size(); ++i)
        out.push_back(glm::distance(s.points[i - 1], s.points[i]));
    return out;
}

groom::Dab dabAt(const D3& c, double r)
{
    groom::Dab d;
    d.center = c;
    d.radius = r;
    d.normal = D3(0, 1, 0);
    return d;
}

json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }
} // namespace

TEST_CASE("groom resample spreads points evenly")
{
    std::vector<D3> r = groom::resample({D3(0), D3(1, 0, 0), D3(10, 0, 0)}, 6);
    REQUIRE(r.size() == 6);
    for (int i = 0; i < 6; ++i)
        CHECK(r[size_t(i)].x == doctest::Approx(2.0 * i));
    std::vector<D3> same = groom::resample({D3(1), D3(1)}, 3);
    CHECK(same.size() == 3);
    CHECK(glm::distance(same[2], D3(1)) < 1e-12);
}

TEST_CASE("groom comb, smooth and puff keep roots and lengths")
{
    groom::Surface surf = ground();
    std::mt19937 rng(1);
    Splines s = patch();
    const Splines before = s;
    groom::Settings o;
    o.brush = groom::Brush::Comb;
    o.strength = 1.0;
    groom::Dab d = dabAt(D3(0, 3, 0), 3.5);
    d.delta = D3(2, 0, 0);
    groom::Stats st;
    groom::applyDab(s, o, d, &surf, rng, &st);
    CHECK(st.changed > 0);
    const Spline& mid = s[12];
    CHECK(glm::distance(mid.points[0], before[12].points[0]) < 1e-12);
    CHECK(mid.points.back().x > 0.5);
    for (size_t k = 0; k < lengths(mid).size(); ++k)
        CHECK(lengths(mid)[k] == doctest::Approx(1.0));
    // Far away strands stay as they were.
    CHECK(glm::distance(s[0].points.back(), before[0].points.back()) < 1e-12);

    // Smooth straightens a zigzag.
    Spline zig;
    zig.kind = Kind::Poly;
    for (int i = 0; i < 7; ++i)
        zig.points.push_back(D3(i % 2 ? 0.6 : 0.0, i * 1.0, 0.0));
    zig.normalize();
    Splines z = {zig};
    o.brush = groom::Brush::Smooth;
    o.keepLength = false;
    for (int k = 0; k < 10; ++k)
        groom::applyDab(z, o, dabAt(D3(0, 3, 0), 10.0), &surf, rng);
    double wiggle = 0.0;
    for (size_t i = 1; i + 1 < z[0].size(); ++i)
        wiggle += std::abs(z[0].points[i].x - (z[0].points[i - 1].x + z[0].points[i + 1].x) * 0.5);
    CHECK(wiggle < 0.6);

    // Puff stands a flat strand up along the surface normal.
    Splines flat = {strand(D3(0), D3(1, 0, 0), 6.0, 7)};
    o.brush = groom::Brush::Puff;
    o.keepLength = true;
    for (int k = 0; k < 20; ++k)
        groom::applyDab(flat, o, dabAt(D3(3, 0, 0), 8.0), &surf, rng);
    CHECK(flat[0].points.back().y > 4.0);
    CHECK(glm::length(flat[0].points.front()) < 1e-12);
    o.invert = true;
    for (int k = 0; k < 20; ++k)
        groom::applyDab(flat, o, dabAt(D3(0, 3, 0), 8.0), &surf, rng);
    CHECK(flat[0].points.back().y < 2.0);
}

TEST_CASE("groom length, cut and pinch")
{
    std::mt19937 rng(1);
    groom::Settings o;
    o.strength = 1.0;
    o.falloff = [](double) { return 1.0; };
    Splines s = {strand(D3(0), D3(0, 1, 0), 6.0, 7)};
    o.brush = groom::Brush::Length;
    groom::applyDab(s, o, dabAt(D3(0, 3, 0), 5.0), nullptr, rng);
    CHECK(s[0].points.back().y == doctest::Approx(6.6));
    o.invert = true;
    groom::applyDab(s, o, dabAt(D3(0, 3, 0), 5.0), nullptr, rng);
    CHECK(s[0].points.back().y == doctest::Approx(6.6 * 0.9));
    o.invert = false;

    Splines c = {strand(D3(0), D3(0, 1, 0), 6.0, 7), strand(D3(10, 0, 0), D3(0, 1, 0), 6.0, 7)};
    o.brush = groom::Brush::Cut;
    o.frontOnly = false;
    groom::applyDab(c, o, dabAt(D3(0, 4.2, 0), 0.7), nullptr, rng);
    REQUIRE(c.size() == 2);
    CHECK(c[0].size() == 7);
    CHECK(c[0].points.back().y == doctest::Approx(3.5));
    CHECK(c[1].points.back().y == doctest::Approx(6.0));
    // Cutting through the root takes the whole curve.
    groom::applyDab(c, o, dabAt(D3(10, 0.2, 0), 0.5), nullptr, rng);
    CHECK(c.size() == 1);

    Splines p = {strand(D3(-1, 0, 0), D3(0, 1, 0), 6.0, 7), strand(D3(1, 0, 0), D3(0, 1, 0), 6.0, 7)};
    o.brush = groom::Brush::Pinch;
    o.keepLength = false;
    groom::Dab d = dabAt(D3(0, 5, 0), 3.0);
    d.viewDir = D3(0, 0, -1);
    groom::applyDab(p, o, d, nullptr, rng);
    CHECK(p[0].points.back().x > -1.0);
    CHECK(p[1].points.back().x < 1.0);
    CHECK(p[0].points.front().x == doctest::Approx(-1.0));
}

TEST_CASE("groom add, delete, density and slide on a surface")
{
    geo::Mesh plane;
    groom::Surface surf = ground({}, &plane);
    std::mt19937 rng(7);
    groom::Settings o;
    o.strength = 1.0;
    Splines s = patch(6.0, 7);
    const size_t start = s.size();
    o.brush = groom::Brush::Add;
    o.addCount = 6;
    o.minDistance = 0.3;
    groom::Stats st;
    groom::applyDab(s, o, dabAt(D3(10, 0, 10), 2.0), &surf, rng, &st);
    CHECK(st.added == 6);
    REQUIRE(s.size() == start + 6);
    for (size_t i = start; i < s.size(); ++i)
    {
        const Spline& sp = s[i];
        CHECK(std::abs(sp.points.front().y) < 1e-9);
        CHECK(glm::distance(sp.points.front(), D3(10, 0, 10)) <= 2.0 + 1e-9);
        // Shaped like the upright neighbours, 6 long.
        CHECK(sp.points.back().y == doctest::Approx(6.0).epsilon(1e-6));
    }
    // Without a surface add plants nothing and says why.
    groom::Stats none;
    groom::applyDab(s, o, dabAt(D3(10, 0, 10), 2.0), nullptr, rng, &none);
    CHECK(none.added == 0);
    CHECK(!none.warnings.empty());

    o.brush = groom::Brush::Delete;
    o.falloff = [](double) { return 1.0; };
    groom::Stats del;
    groom::applyDab(s, o, dabAt(D3(10, 3, 10), 3.0), &surf, rng, &del);
    CHECK(del.removed == 6);
    CHECK(s.size() == start);

    // Density fills an empty spot up to the target and thins a crowded one.
    o.brush = groom::Brush::Density;
    o.density = 1.0;
    o.addCount = 50;
    o.minDistance = 0.0;
    groom::applyDab(s, o, dabAt(D3(-12, 0, 12), 2.0), &surf, rng);
    size_t inside = 0;
    for (const Spline& sp : s)
        if (glm::distance(sp.points.front(), D3(-12, 0, 12)) < 2.0)
            ++inside;
    CHECK(inside >= 5);
    CHECK(inside <= 12);
    o.invert = true;
    o.density = 0.1;
    for (int k = 0; k < 20; ++k)
        groom::applyDab(s, o, dabAt(D3(-12, 0, 12), 2.0), &surf, rng);
    inside = 0;
    for (const Spline& sp : s)
        if (glm::distance(sp.points.front(), D3(-12, 0, 12)) < 2.0)
            ++inside;
    CHECK(inside <= 1);
    o.invert = false;

    // Slide moves roots over the surface, curves riding along.
    Splines one = {strand(D3(0), D3(0, 1, 0), 4.0, 5)};
    o.brush = groom::Brush::Slide;
    groom::Dab d = dabAt(D3(0, 0, 0), 2.0);
    d.delta = D3(3, 2, 0);
    groom::applyDab(one, o, d, &surf, rng);
    CHECK(one[0].points.front().x == doctest::Approx(3.0));
    CHECK(std::abs(one[0].points.front().y) < 1e-9);
    CHECK(one[0].points.back().y == doctest::Approx(4.0));
}

TEST_CASE("groom mask, attach and guides")
{
    groom::Surface surf = ground();
    std::mt19937 rng(1);
    groom::Settings o;
    o.brush = groom::Brush::Comb;
    o.strength = 1.0;
    Splines s = patch();
    const Splines before = s;
    o.mask.assign(s.size(), 0);
    o.mask[12] = 1;
    groom::Dab d = dabAt(D3(0, 3, 0), 10.0);
    d.delta = D3(1, 0, 0);
    groom::applyDab(s, o, d, &surf, rng);
    CHECK(glm::distance(s[12].points.back(), before[12].points.back()) > 0.1);
    CHECK(glm::distance(s[11].points.back(), before[11].points.back()) < 1e-12);

    Splines floating = {strand(D3(1, 3, 1), D3(0, 1, 0), 2.0, 3)};
    CHECK(groom::attachRoots(floating, surf) == 1);
    CHECK(std::abs(floating[0].points.front().y) < 1e-9);
    CHECK(floating[0].points.back().y == doctest::Approx(2.0));

    groom::GuideOptions g;
    g.count = 60;
    g.length = 3.0;
    g.points = 4;
    Splines gs = groom::guides(surf, g);
    CHECK(gs.size() == 60);
    for (const Spline& sp : gs)
    {
        CHECK(std::abs(sp.points.front().y) < 1e-9);
        CHECK(sp.points.back().y == doctest::Approx(3.0));
        CHECK(sp.size() == 4);
    }
    // A density map black on one half keeps every root on the other.
    geo::Mesh m = geo::makePlane({40.0, 40.0, 8, 8});
    std::span<glm::vec4> col = geo::colors::add(m, "density", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceCentroid(f).x > 0.0)
            m.forEachFaceCorner(f, [&](geo::Index c) { col[c] = glm::vec4(1.0f); });
    groom::Surface mapped(std::move(m), "density");
    Splines half = groom::guides(mapped, g);
    CHECK(!half.empty());
    for (const Spline& sp : half)
        CHECK(sp.points.front().x > -1e-9);
}

TEST_CASE("hair cards: strips, atlas cells and facing")
{
    groom::Surface surf = ground();
    // Strands lying along the scalp: cards facing the surface lie flat, normals up.
    Splines s;
    for (int k = 0; k < 6; ++k)
        s.push_back(strand(D3(0, 0.1, k * 3.0), D3(1, 0, 0), 8.0, 5));
    CardOptions o;
    o.width = 1.0;
    o.segments = 4;
    o.widthSegments = 2;
    o.columns = 4;
    o.rows = 2;
    o.taper = parseRamp("0:1 1:0");
    CardResult r = hairCards(s, {}, &surf, o);
    CHECK(r.cards == 6);
    CHECK(r.mesh.numFaces() == 6 * 4 * 2);
    CHECK(r.warnings.empty());
    auto uv = r.mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    REQUIRE(!uv.empty());
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
    {
        if (!r.mesh.faceValid(f))
            continue;
        CHECK(r.mesh.faceSize(f) == 4);
        CHECK(r.mesh.faceArea(f) > 1e-8);
        CHECK(r.mesh.faceNormal(f).y > 0.99);
        // Every corner of a card's face sits in one atlas cell.
        glm::vec2 lo(1e9f), hi(-1e9f);
        r.mesh.forEachFaceCorner(f, [&](geo::Index c) {
            lo = glm::min(lo, uv[c]);
            hi = glm::max(hi, uv[c]);
        });
        CHECK(int(std::floor(lo.x * 4.0f + 1e-4f)) == int(std::floor(hi.x * 4.0f - 1e-4f)));
        CHECK(int(std::floor(lo.y * 2.0f + 1e-4f)) == int(std::floor(hi.y * 2.0f - 1e-4f)));
    }
    for (int c : r.cells)
        CHECK((c >= 0 && c < 8));
    // The same seed picks the same cells; variations limits them.
    CHECK(hairCards(s, {}, &surf, o).cells == r.cells);
    o.variations = 1;
    for (int c : hairCards(s, {}, &surf, o).cells)
        CHECK(c == 0);

    o.doubleSided = true;
    CardResult two = hairCards(s, {}, &surf, o);
    CHECK(two.mesh.numFaces() == 2 * r.mesh.numFaces());

    // Upright strands facing up can't lie flat: they still make clean strips.
    o.doubleSided = false;
    Splines up = {strand(D3(0), D3(0, 1, 0), 5.0, 6)};
    CardResult u = hairCards(up, {}, &surf, o);
    CHECK(u.mesh.numFaces() == 8);
    for (geo::Index f = 0; f < u.mesh.faceCapacity(); ++f)
        if (u.mesh.faceValid(f))
            CHECK(u.mesh.faceArea(f) > 1e-8);
}

TEST_CASE("hair cards: clumps, guides and degenerate curves")
{
    Splines s;
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 5; ++k)
            s.push_back(strand(D3(c * 20.0 + k * 0.2, 0, k * 0.1), D3(0, 1, 0), 6.0, 6));
    CardOptions o;
    o.source = CardSource::Clumps;
    o.clumps = 3;
    o.autoWidth = true;
    o.segments = 5;
    CardResult r = hairCards(s, {}, nullptr, o);
    CHECK(r.cards == 3);
    CHECK(r.mesh.numFaces() == 15);

    Splines guides = {strand(D3(0), D3(0, 1, 0), 6.0, 4), strand(D3(20, 0, 0), D3(0, 1, 0), 6.0, 4)};
    o.source = CardSource::Guides;
    CHECK(hairCards(s, guides, nullptr, o).cards == 2);
    CHECK(!hairCards(s, {}, nullptr, o).warnings.empty());

    Splines bad = {strand(D3(1), D3(0, 1, 0), 0.0, 3), strand(D3(0), D3(0, 1, 0), 3.0, 3)};
    o.source = CardSource::Curves;
    CardResult b = hairCards(bad, {}, nullptr, o);
    CHECK(b.cards == 1);
    REQUIRE(!b.warnings.empty());
    for (const D3& p : b.mesh.positions())
        CHECK(std::isfinite(p.x + p.y + p.z));
}

TEST_CASE("groom commands: sculpt stroke, surface, guides and live cards")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 40}, {"depth", 40}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    std::string guides = run("groomGuides", {{"args", {plane}}, {"count", 30}, {"length", 5.0}, {"points", 6}, {"name", "Hair"}});
    json data = run("curveData", {{"args", {guides}}});
    CHECK(data.size() == 30);
    const json before = data;
    json dab = {{"center", {0, 2.5, 0}}, {"normal", {0, 1, 0}}, {"radius", 30.0}, {"strength", 1.0}, {"delta", {2, 0, 0}}, {"view", {0, 0, -1}}};
    json r = run("curveSculptStroke", {{"args", {guides}}, {"brush", "comb"}, {"strength", 1.0}, {"dabs", {dab}}});
    CHECK(r["changed"].get<int>() > 0);
    json after = run("curveData", {{"args", {guides}}});
    CHECK(after != before);
    CHECK(after[0]["points"][0] == before[0]["points"][0]);
    run("undo");
    CHECK(run("curveData", {{"args", {guides}}}) == before);

    json add = {{"center", {5, 0, 5}}, {"normal", {0, 1, 0}}, {"radius", 3.0}, {"strength", 1.0}};
    r = run("curveSculptStroke", {{"args", {guides}}, {"brush", "add"}, {"addCount", 4}, {"dabs", {add}}, {"seed", 3}});
    CHECK(r["added"].get<int>() >= 1);
    CHECK(r["curves"].get<int>() == 30 + r["added"].get<int>());

    std::string floating = run("curve", {{"type", "poly"}, {"points", {{1, 4, 1}, {1, 6, 1}}}, {"name", "Float"}});
    CHECK(run("groomSurface", {{"args", {floating}}, {"mesh", plane}}).get<int>() == 1);
    CHECK(std::abs(run("curveData", {{"args", {floating}}})[0]["points"][0][1].get<double>()) < 1e-9);

    json cards = run("hairCards", {{"args", {guides}}, {"width", 0.5}, {"segments", 3}});
    REQUIRE(cards.size() == 2);
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::MeshPtr m = g.find(cards[0].get<std::string>())->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    const geo::Index faces = m->numFaces();
    CHECK(faces == r["curves"].get<geo::Index>() * 3);
    // Live: a new setting rebuilds the cards.
    run("setAttr", {{"args", {cards[1].get<std::string>() + ".segments", 5}}});
    g.evaluate();
    CHECK(g.find(cards[0].get<std::string>())->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->numFaces() == faces / 3 * 5);

    auto dir = std::filesystem::temp_directory_path() / "lila_curve_tests";
    std::string path = (dir / "groom.lila").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    scene::Graph& g2 = scene::Graph::current();
    g2.evaluate();
    CHECK(g2.find(cards[0].get<std::string>())->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->numFaces() == faces / 3 * 5);
    CHECK(run("getAttr", {{"args", {guides + ".groomSurface"}}}).get<std::string>().find("Plane") != std::string::npos);
}
