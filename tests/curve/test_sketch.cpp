#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "curve/Sketch.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"

#include <glm/gtc/constants.hpp>

#include <filesystem>

using namespace lila;
using namespace lila::curve;
using namespace lila::curve::sketch;
using json = nlohmann::json;

namespace
{
/** The ground plane (Y up): horizontal is X, vertical is -Z. */
Plane ground() { return planeFrom(D3(0.0), D3(0, 1, 0)); }

Spline poly(std::initializer_list<D3> pts, bool closed = false)
{
    Spline s;
    s.kind = Kind::Poly;
    s.points = pts;
    s.closed = closed;
    s.normalize();
    return s;
}

Ref P(int s, int i) { return {RefKind::Point, s, i}; }
Ref Ln(int s, int i) { return {RefKind::Line, s, i}; }
Ref C(int s) { return {RefKind::Curve, s, 0}; }
Ref O(int s) { return {RefKind::Center, s, 0}; }

Constraint make(Type t, std::vector<Ref> refs, double value = 0.0)
{
    Constraint c;
    c.type = t;
    c.refs = std::move(refs);
    c.value = value;
    return c;
}

/** Adds c to k, prepared against s, and returns its id. */
uint32_t add(Sketch& k, const Splines& s, Constraint c)
{
    prepare(c, s, k.plane);
    return k.add(std::move(c));
}

D2 at(const Splines& s, const Plane& pl, Ref r) { return pl.to2d(refPosition(s, pl, r)); }

json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

json pts(std::initializer_list<D3> list)
{
    json a = json::array();
    for (const D3& p : list)
        a.push_back({p.x, p.y, p.z});
    return a;
}
} // namespace

TEST_CASE("sketch refs and storage round trip")
{
    for (const char* t : {"p0.3", "p2.end", "l1.0", "c4", "o2", "hl0.1", "hr3.end"})
    {
        Ref r;
        REQUIRE(parseRef(t, &r));
        CHECK(refName(r) == t);
    }
    Ref r;
    CHECK(!parseRef("p0", &r));
    CHECK(!parseRef("c0.1", &r));
    CHECK(!parseRef("x1.2", &r));
    CHECK(!parseRef("p-1.2", &r));

    Sketch k;
    k.plane = ground();
    Splines s = {poly({D3(0), D3(10, 0, 1), D3(10, 0, 5)})};
    add(k, s, make(Type::Horizontal, {Ln(0, 0)}));
    uint32_t d = add(k, s, make(Type::Length, {Ln(0, 1)}, 4.0));
    k.constraints.back().expr = "2 * 2cm";
    Sketch back = Sketch::fromText(k.toText());
    CHECK(back.toJson() == k.toJson());
    CHECK(back.find(d)->name == "d1");
    CHECK(Sketch::fromText("").empty());
    CHECK_THROWS(Sketch::fromText("{\"constraints\": [{\"type\": \"bogus\"}]}"));
}

TEST_CASE("sketch rectangle solves fully constrained")
{
    const Plane pl = ground();
    Splines s = {poly({D3(0.3, 0, 0.2), D3(9.0, 0, -0.5), D3(10.4, 0, -6.0), D3(-0.2, 0, -4.3)}, true)};
    Sketch k;
    k.plane = pl;
    Status st = analyze(s, k);
    CHECK(st.dof == 8);
    add(k, s, make(Type::Fixed, {P(0, 0)}));
    add(k, s, make(Type::Horizontal, {Ln(0, 0)}));
    add(k, s, make(Type::Horizontal, {Ln(0, 2)}));
    add(k, s, make(Type::Vertical, {Ln(0, 1)}));
    add(k, s, make(Type::Vertical, {Ln(0, 3)}));
    add(k, s, make(Type::Length, {Ln(0, 0)}, 10.0));
    CHECK(analyze(s, k).dof == 1);
    add(k, s, make(Type::Length, {Ln(0, 1)}, 5.0));
    SolveResult r = solve(s, k);
    REQUIRE(r.ok);
    CHECK(r.error < 1e-6);
    const D2 p0 = at(s, pl, P(0, 0)), p1 = at(s, pl, P(0, 1)), p2 = at(s, pl, P(0, 2)), p3 = at(s, pl, P(0, 3));
    CHECK(p0.x == doctest::Approx(0.3));
    CHECK(p0.y == doctest::Approx(-0.2));
    CHECK(p1.y == doctest::Approx(p0.y));
    CHECK(glm::distance(p0, p1) == doctest::Approx(10.0));
    CHECK(glm::distance(p1, p2) == doctest::Approx(5.0));
    CHECK(p2.x == doctest::Approx(p1.x));
    CHECK(p3.x == doctest::Approx(p0.x));
    CHECK(p3.y == doctest::Approx(p2.y));
    // Heights off the plane are kept.
    CHECK(s[0].points[1].y == doctest::Approx(0.0));
    st = analyze(s, k);
    CHECK(st.dof == 0);
    CHECK(st.splineFixed[0]);
    CHECK(st.lineFixed(0, 3));
    CHECK(st.redundant.empty());
}

TEST_CASE("sketch dof per point and redundancy")
{
    const Plane pl = ground();
    Splines s = {poly({D3(0), D3(10, 0, 0.5)})};
    Sketch k;
    k.plane = pl;
    add(k, s, make(Type::Fixed, {P(0, 0)}));
    Status st = analyze(s, k);
    CHECK(st.dof == 2);
    CHECK(st.pointIsFixed(0, 0));
    CHECK(!st.pointIsFixed(0, 1));
    add(k, s, make(Type::Horizontal, {Ln(0, 0)}));
    solve(s, k);
    CHECK(analyze(s, k).dof == 1);
    // Horizontal again through the two points adds nothing.
    Constraint again = make(Type::Horizontal, {P(0, 0), P(0, 1)});
    prepare(again, s, pl);
    again.id = 99;
    AddCheck chk = checkAdd(s, k, again);
    CHECK(chk.result.ok);
    CHECK(chk.redundant);
    Constraint len = make(Type::Length, {Ln(0, 0)}, 7.0);
    prepare(len, s, pl);
    len.id = 100;
    chk = checkAdd(s, k, len);
    CHECK(chk.result.ok);
    CHECK(!chk.redundant);
}

TEST_CASE("sketch conflicts name the constraints in the way")
{
    const Plane pl = ground();
    Splines s = {poly({D3(0), D3(10, 0, 0)})};
    Sketch k;
    k.plane = pl;
    uint32_t a = add(k, s, make(Type::Fixed, {P(0, 0)}));
    uint32_t b = add(k, s, make(Type::Fixed, {P(0, 1)}));
    const Splines before = s;
    uint32_t len = add(k, s, make(Type::Length, {Ln(0, 0)}, 4.0));
    SolveResult r = solve(s, k);
    CHECK(!r.ok);
    CHECK(std::ranges::find(r.conflicting, len) != r.conflicting.end());
    CHECK(std::ranges::find(r.conflicting, a) != r.conflicting.end());
    CHECK(std::ranges::find(r.conflicting, b) != r.conflicting.end());
    // The failed group keeps its geometry.
    CHECK(s[0].points[1].x == doctest::Approx(10.0));
}

TEST_CASE("sketch geometric constraints between lines")
{
    const Plane pl = ground();
    Splines s = {poly({D3(0), D3(10, 0, -1)}), poly({D3(2, 0, -3), D3(8, 0, -9)}), poly({D3(20, 0, 0), D3(21, 0, -6)})};
    Sketch k;
    k.plane = pl;
    add(k, s, make(Type::Fixed, {Ln(0, 0)}));
    add(k, s, make(Type::Perpendicular, {Ln(0, 0), Ln(1, 0)}));
    add(k, s, make(Type::Equal, {Ln(0, 0), Ln(1, 0)}));
    add(k, s, make(Type::Parallel, {Ln(2, 0), Ln(1, 0)}));
    add(k, s, make(Type::Midpoint, {P(1, 0), Ln(0, 0)}));
    REQUIRE(solve(s, k).ok);
    const D2 a0 = at(s, pl, P(0, 0)), a1 = at(s, pl, P(0, 1)), b0 = at(s, pl, P(1, 0)), b1 = at(s, pl, P(1, 1));
    const D2 c0 = at(s, pl, P(2, 0)), c1 = at(s, pl, P(2, 1));
    CHECK(glm::dot(a1 - a0, b1 - b0) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(glm::distance(a0, a1) == doctest::Approx(glm::distance(b0, b1)));
    CHECK((c1 - c0).x * (b1 - b0).y - (c1 - c0).y * (b1 - b0).x == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(glm::distance(b0, (a0 + a1) * 0.5) < 1e-6);

    // Exactly parallel lines made perpendicular (a flat spot for cross and dot residuals).
    Splines par = {poly({D3(0), D3(10, 0, 0)}), poly({D3(0, 0, -3), D3(10, 0, -3)})};
    Sketch pk;
    pk.plane = pl;
    add(pk, par, make(Type::Fixed, {Ln(0, 0)}));
    add(pk, par, make(Type::Perpendicular, {Ln(0, 0), Ln(1, 0)}));
    REQUIRE(solve(par, pk).ok);
    const D2 pd = at(par, pl, P(1, 1)) - at(par, pl, P(1, 0));
    CHECK(std::abs(pd.x) < 1e-6 * glm::length(pd));

    // Angle and collinear.
    Splines t = {poly({D3(0), D3(10, 0, 0)}), poly({D3(0, 0, -1), D3(5, 0, -4)}), poly({D3(12, 0, 1), D3(16, 0, 2)})};
    Sketch q;
    q.plane = pl;
    add(q, t, make(Type::Fixed, {Ln(0, 0)}));
    add(q, t, make(Type::Coincident, {P(1, 0), P(0, 0)}));
    add(q, t, make(Type::Angle, {Ln(0, 0), Ln(1, 0)}, 30.0));
    add(q, t, make(Type::Collinear, {Ln(0, 0), Ln(2, 0)}));
    REQUIRE(solve(t, q).ok);
    const D2 d = at(t, pl, P(1, 1)) - at(t, pl, P(1, 0));
    CHECK(glm::degrees(std::abs(std::atan2(d.y, d.x))) == doctest::Approx(30.0));
    CHECK(at(t, pl, P(2, 0)).y == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(at(t, pl, P(2, 1)).y == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("sketch symmetric, distances and point on line")
{
    const Plane pl = ground();
    Splines s = {poly({D3(0, 0, 5), D3(0, 0, -5)}), poly({D3(-3, 0, -1), D3(4, 0, -2)}), poly({D3(5, 0, 5), D3(9, 0, 4)})};
    Sketch k;
    k.plane = pl;
    add(k, s, make(Type::Fixed, {Ln(0, 0)}));
    add(k, s, make(Type::Symmetric, {P(1, 0), P(1, 1), Ln(0, 0)}));
    add(k, s, make(Type::HorizontalDistance, {P(1, 0), P(1, 1)}, 8.0));
    add(k, s, make(Type::Distance, {P(2, 0), Ln(0, 0)}, 3.0));
    add(k, s, make(Type::PointOnCurve, {P(2, 1), Ln(1, 0)}));
    REQUIRE(solve(s, k).ok);
    const D2 a = at(s, pl, P(1, 0)), b = at(s, pl, P(1, 1));
    CHECK(a.x == doctest::Approx(-4.0));
    CHECK(b.x == doctest::Approx(4.0));
    CHECK(a.y == doctest::Approx(b.y));
    CHECK(at(s, pl, P(2, 0)).x == doctest::Approx(3.0));
    const D2 p = at(s, pl, P(2, 1));
    CHECK(std::abs((b - a).x * (p - a).y - (b - a).y * (p - a).x) < 1e-6);
}

TEST_CASE("sketch circles and arcs")
{
    const Plane pl = ground();
    Splines s = {circle(D3(0), D3(0, 1, 0), 3.0), poly({D3(-10, 0, -5), D3(10, 0, -4)}), circle(D3(1, 0, 1), D3(0, 1, 0), 1.0)};
    REQUIRE(circleOf(s[0], pl).shape == Shape::Circle);
    CHECK(circleOf(s[1], pl).shape == Shape::Points);
    Sketch k;
    k.plane = pl;
    add(k, s, make(Type::Fixed, {C(0)}));
    add(k, s, make(Type::Horizontal, {Ln(1, 0)}));
    add(k, s, make(Type::Tangent, {Ln(1, 0), C(0)}));
    add(k, s, make(Type::Concentric, {C(2), C(0)}));
    add(k, s, make(Type::Diameter, {C(2)}, 4.0));
    REQUIRE(solve(s, k).ok);
    CHECK(circleOf(s[0], pl).radius == doctest::Approx(3.0));
    // The line stays tangent on the side it started.
    CHECK(std::abs(at(s, pl, P(1, 0)).y) == doctest::Approx(3.0));
    CHECK(at(s, pl, P(1, 0)).y == doctest::Approx(at(s, pl, P(1, 1)).y));
    const CircleFit inner = circleOf(s[2], pl);
    REQUIRE(inner.shape == Shape::Circle);
    CHECK(glm::length(inner.center) < 1e-6);
    CHECK(inner.radius == doctest::Approx(2.0));
    CHECK(analyze(s, k).splineFixed[2]);

    // An arc's end held on a line end, its radius dimensioned.
    Splines t = {poly({D3(-10, 0, 0), D3(0, 0, 0)}), arc(D3(0.5, 0, -5), D3(0, 1, 0), D3(0.5, 0, 0), glm::half_pi<double>())};
    REQUIRE(circleOf(t[1], pl).shape == Shape::Arc);
    Sketch q;
    q.plane = pl;
    add(q, t, make(Type::Fixed, {Ln(0, 0)}));
    add(q, t, make(Type::Coincident, {P(1, 0), P(0, 1)}));
    REQUIRE(solve(t, q).ok);
    // Meeting at an end, the tangent runs square to the radius there: well conditioned next to the coincident.
    add(q, t, make(Type::Tangent, {Ln(0, 0), C(1)}));
    CHECK(q.constraints.back().endB == 0);
    CHECK(analyze(t, q).redundant.empty());
    add(q, t, make(Type::Radius, {C(1)}, 4.0));
    REQUIRE(solve(t, q).ok);
    const CircleFit af = circleOf(t[1], pl);
    REQUIRE(af.shape == Shape::Arc);
    CHECK(af.radius == doctest::Approx(4.0));
    CHECK(glm::distance(at(t, pl, P(1, 0)), D2(0.0)) < 1e-6);
    CHECK(std::abs(af.center.x) < 1e-6);
    CHECK(q.constraints[1].refs[0].index == 0);

    // Two arcs meeting end to start: tangent lines their radii up there; a rim drag sets the radius.
    Splines u = {arc(D3(0, 0, -5), D3(0, 1, 0), D3(0, 0, 0), glm::half_pi<double>()), arc(D3(5, 0, -9), D3(0, 1, 0), D3(5, 0, -5), 1.0)};
    Sketch w;
    w.plane = pl;
    add(w, u, make(Type::Fixed, {C(0)}));
    add(w, u, make(Type::Coincident, {P(1, 0), P(0, -1)}));
    REQUIRE(solve(u, w).ok);
    add(w, u, make(Type::Tangent, {C(0), C(1)}));
    REQUIRE(solve(u, w).ok);
    const CircleFit f0 = circleOf(u[0], pl), f1 = circleOf(u[1], pl);
    const D2 e = at(u, pl, P(0, -1));
    CHECK(std::abs((e - f0.center).x * (e - f1.center).y - (e - f0.center).y * (e - f1.center).x) < 1e-6);
    SolveOptions rim;
    rim.drags.push_back({C(1), pl.to3d(f1.center + D2(7.0, 0.0))});
    REQUIRE(solve(u, w, rim).ok);
    CHECK(glm::distance(at(u, pl, P(1, 0)), e) < 1e-6);
}

TEST_CASE("sketch drag keeps constraints")
{
    const Plane pl = ground();
    Splines s = {poly({D3(0), D3(10, 0, 0), D3(10, 0, -5)})};
    Sketch k;
    k.plane = pl;
    add(k, s, make(Type::Fixed, {P(0, 0)}));
    add(k, s, make(Type::Horizontal, {Ln(0, 0)}));
    add(k, s, make(Type::Perpendicular, {Ln(0, 0), Ln(0, 1)}));
    SolveOptions o;
    o.drags.push_back({P(0, 1), D3(14, 0, -3)});
    REQUIRE(solve(s, k, o).ok);
    CHECK(at(s, pl, P(0, 1)).x == doctest::Approx(14.0));
    CHECK(at(s, pl, P(0, 1)).y == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(at(s, pl, P(0, 2)).x == doctest::Approx(14.0));
    CHECK(glm::length(at(s, pl, P(0, 0))) < 1e-6);
    // A pinned point dragged stays put.
    o.drags = {{P(0, 0), D3(3, 0, 3)}};
    REQUIRE(solve(s, k, o).ok);
    CHECK(glm::length(at(s, pl, P(0, 0))) < 1e-6);
}

TEST_CASE("sketch tangent keeps a Bezier smooth")
{
    const Plane pl = ground();
    Spline b;
    b.kind = Kind::Bezier;
    b.points = {D3(10, 0, 0), D3(20, 0, -8)};
    b.handleL = {D3(9, 0, 1), D3(17, 0, -10)};
    b.handleR = {D3(12, 0, -4), D3(22, 0, -6)};
    b.typeL.assign(2, HandleType::Free);
    b.typeR.assign(2, HandleType::Free);
    Splines s = {poly({D3(0), D3(10, 0, 0)}), b};
    Sketch k;
    k.plane = pl;
    add(k, s, make(Type::Fixed, {Ln(0, 0)}));
    add(k, s, make(Type::Tangent, {Ln(0, 0), C(1)}));
    REQUIRE(solve(s, k).ok);
    const D3 h = s[1].handleR[0] - s[1].points[0];
    CHECK(std::abs(h.z) < 1e-6);
    CHECK(h.x > 0.0);
}

TEST_CASE("sketch point on a NURBS curve")
{
    const Plane pl = ground();
    Spline n;
    n.kind = Kind::Nurbs;
    n.degree = 3;
    n.points = {D3(0), D3(5, 0, -6), D3(10, 0, 6), D3(15, 0, 0)};
    Splines s = {n, poly({D3(7, 0, -9), D3(9, 0, -12)})};
    Sketch k;
    k.plane = pl;
    add(k, s, make(Type::Fixed, {C(0)}));
    add(k, s, make(Type::PointOnCurve, {P(1, 0), C(0)}));
    REQUIRE(solve(s, k).ok);
    D3 near;
    closestParam(s[0], s[1].points[0], &near);
    CHECK(glm::distance(near, s[1].points[0]) < 1e-4);
}

TEST_CASE("sketch dimension expressions")
{
    std::map<std::string, double> names = {{"d1", 10.0}};
    CHECK(*evalExpression("d1 * 2 + 5mm", names) == doctest::Approx(20.5));
    CHECK(*evalExpression("2^3 - (1 + 1)", names) == doctest::Approx(6.0));
    CHECK(*evalExpression("1in", names) == doctest::Approx(2.54));
    CHECK(*evalExpression("sqrt(16) + cos(60)", names) == doctest::Approx(4.5));
    std::string why;
    CHECK(!evalExpression("d2 + 1", names, &why));
    CHECK(why.find("d2") != std::string::npos);
    CHECK(!evalExpression("1 / 0", names));
    CHECK(!evalExpression("(1 + 2", names));

    const Plane pl = ground();
    Splines s = {poly({D3(0), D3(10, 0, 0), D3(10, 0, -5)})};
    Sketch k;
    k.plane = pl;
    add(k, s, make(Type::Fixed, {P(0, 0)}));
    add(k, s, make(Type::Horizontal, {Ln(0, 0)}));
    add(k, s, make(Type::Vertical, {Ln(0, 1)}));
    add(k, s, make(Type::Length, {Ln(0, 0)}, 12.0));
    Constraint d2 = make(Type::Length, {Ln(0, 1)});
    d2.expr = "d1 / 4";
    add(k, s, d2);
    REQUIRE(solve(s, k).ok);
    CHECK(glm::distance(s[0].points[1], s[0].points[2]) == doctest::Approx(3.0));
    k.constraints[3].expr = "d2";
    CHECK_THROWS_AS(dimensionValues(k), std::invalid_argument);
}

TEST_CASE("sketch inference while drawing")
{
    const Plane pl = ground();
    Splines s = {poly({D3(0), D3(10, 0, 0)}), circle(D3(20, 0, 0), D3(0, 1, 0), 2.0)};
    Inference i = infer(s, pl, D2(10.2, 0.1), std::nullopt, 0.5);
    REQUIRE(i.constraints.size() == 1);
    CHECK(i.constraints[0].type == Type::Coincident);
    CHECK(i.constraints[0].refs[1] == P(0, 1));
    CHECK(i.at.x == doctest::Approx(10.0));
    i = infer(s, pl, D2(20.1, -0.1), std::nullopt, 0.5);
    REQUIRE(i.constraints.size() == 1);
    CHECK(i.constraints[0].refs[1] == O(1));
    i = infer(s, pl, D2(5, 7.2), D2(0, 7), 0.5);
    REQUIRE(i.constraints.size() == 1);
    CHECK(i.constraints[0].type == Type::Horizontal);
    CHECK(i.at.y == doctest::Approx(7.0));
    i = infer(s, pl, D2(4, 0.3), std::nullopt, 0.5);
    REQUIRE(i.constraints.size() == 1);
    CHECK(i.constraints[0].type == Type::PointOnCurve);
    CHECK(i.at.y == doctest::Approx(0.0));
}

TEST_CASE("sketch degenerate input stays finite")
{
    const Plane pl = ground();
    Splines s = {poly({D3(0), D3(0)}), poly({D3(1, 0, 1), D3(1, 0, 1)})};
    Sketch k;
    k.plane = pl;
    add(k, s, make(Type::Parallel, {Ln(0, 0), Ln(1, 0)}));
    add(k, s, make(Type::Angle, {Ln(0, 0), Ln(1, 0)}, 45.0));
    SolveResult r = solve(s, k);
    for (const Spline& sp : s)
        for (const D3& p : sp.points)
            CHECK(std::isfinite(p.x + p.y + p.z));
    (void)r;
    Constraint bad = make(Type::Length, {Ln(0, 5)}, 1.0);
    CHECK_THROWS_AS(prepare(bad, s, pl), std::invalid_argument);
    Constraint wrong = make(Type::Radius, {Ln(0, 0)}, 1.0);
    CHECK_THROWS_AS(prepare(wrong, s, pl), std::invalid_argument);
    // A constraint whose geometry went away is skipped, not a crash.
    Sketch stale;
    stale.plane = pl;
    Constraint c = make(Type::Horizontal, {Ln(7, 0)});
    stale.add(c);
    Splines t = s;
    SolveResult rs = solve(t, stale);
    CHECK(rs.invalid.size() == 1);
}

TEST_CASE("sketch commands: constrain, dimension, undo, save and drag")
{
    run("newScene");
    std::string c = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(10, 0, 0.7), D3(11, 0, -6)})}, {"name", "Sk"}});
    json ids = run("sketchConstrain", {{"args", {c}}, {"type", "fixed"}, {"refs", {"p0.0"}}});
    REQUIRE(ids.size() == 1);
    run("sketchConstrain", {{"args", {c}}, {"type", "horizontal"}, {"refs", {"l0.0"}}});
    json pts0 = run("curveData", {{"args", {c}}})[0]["points"];
    CHECK(pts0[1][2].get<double>() == doctest::Approx(0.0).epsilon(1e-6));
    json d = run("sketchDimension", {{"args", {c}}, {"type", "length"}, {"refs", {"l0.0"}}, {"value", 8.0}});
    CHECK(d["name"] == "d1");
    CHECK(run("curveData", {{"args", {c}}})[0]["points"][1][0].get<double>() == doctest::Approx(8.0));
    // Conflicting and redundant constraints are refused with the reason.
    CHECK_THROWS_WITH_AS(run("sketchConstrain", {{"args", {c}}, {"type", "vertical"}, {"refs", {"l0.0"}}}), doctest::Contains("conflicts"), CommandError);
    CHECK_THROWS_WITH_AS(run("sketchConstrain", {{"args", {c}}, {"type", "horizontal"}, {"refs", {"p0.0", "p0.1"}}}), doctest::Contains("adds nothing"),
                         CommandError);
    run("sketchEditDimension", {{"args", {c}}, {"dimension", "d1"}, {"expr", "50mm * 3"}});
    CHECK(run("curveData", {{"args", {c}}})[0]["points"][1][0].get<double>() == doctest::Approx(15.0));
    run("undo");
    CHECK(run("curveData", {{"args", {c}}})[0]["points"][1][0].get<double>() == doctest::Approx(8.0));
    json info = run("sketchInfo", {{"args", {c}}});
    CHECK(info["constraints"].size() == 3);
    CHECK(info["dof"] == 2);
    CHECK(info["pointFixed"][0][1] == true);
    CHECK(info["pointFixed"][0][2] == false);

    // Moving a point in vertex mode drags it: the constraints keep holding.
    run("selectComponents", {{"args", {c}}, {"vertices", json::array({componentId(0, 1, PointRole::Point)})}});
    run("moveComponents", {{"by", {0, 0, 4}}});
    json moved = run("curveData", {{"args", {c}}})[0]["points"];
    CHECK(moved[1][2].get<double>() == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(moved[1][0].get<double>() == doctest::Approx(8.0));
    run("sketchDrag", {{"args", {c}}, {"point", "p0.2"}, {"to", {3, 0, -2}}});
    CHECK(run("curveData", {{"args", {c}}})[0]["points"][2][0].get<double>() == doctest::Approx(3.0));

    auto dir = std::filesystem::temp_directory_path() / "lila_curve_tests";
    for (const char* ext : {".lila", ".lilaa"})
    {
        std::string path = (dir / (std::string("sketch") + ext)).string();
        json before = run("sketchInfo", {{"args", {c}}});
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        json after = run("sketchInfo", {{"args", {c}}});
        CHECK(after["constraints"] == before["constraints"]);
        CHECK(after["dof"] == before["dof"]);
    }
    run("sketchDelete", {{"args", {c}}, {"ids", {d["id"]}}});
    CHECK(run("sketchInfo", {{"args", {c}}})["constraints"].size() == 2);
}

TEST_CASE("sketch commands: combine and add with auto constraints")
{
    run("newScene");
    std::string a = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(10, 0, 0)})}, {"name", "A"}});
    std::string b = run("curve", {{"type", "poly"}, {"points", pts({D3(10, 0, 0.2), D3(10, 0, -5)})}, {"name", "B"}});
    run("sketchConstrain", {{"args", {b}}, {"type", "vertical"}, {"refs", {"l0.0"}}});
    std::string merged = run("sketchCombine", {{"args", {a, b}}});
    CHECK(merged == a);
    CHECK(!run("objExists", {{"args", {"B"}}}).get<bool>());
    json info = run("sketchInfo", {{"args", {a}}});
    REQUIRE(info["constraints"].size() == 1);
    CHECK(info["constraints"][0]["refs"][0] == "l1.0");
    run("sketchConstrain", {{"args", {a}}, {"type", "coincident"}, {"refs", {"p0.1", "p1.0"}}});
    json data = run("curveData", {{"args", {a}}});
    CHECK(data[1]["points"][0][2].get<double>() == doctest::Approx(data[0]["points"][1][2].get<double>()));

    json r = run("sketchAdd", {{"args", {a}},
                               {"splines", json::array({splineToJson(poly({D3(10, 0, -5), D3(0, 0, -5)}))})},
                               {"constraints", json::array({{{"type", "coincident"}, {"refs", {"p2.0", "p1.1"}}},
                                                            {{"type", "horizontal"}, {"refs", {"l2.0"}}},
                                                            {{"type", "length"}, {"refs", {"l2.0"}}, {"value", 10.0}},
                                                            {{"type", "vertical"}, {"refs", {"l2.0"}}}})}});
    CHECK(r["splines"] == json::array({2}));
    // Vertical on a horizontal line of length 10 conflicts, so it is dropped.
    CHECK(r["constraints"].size() == 3);
    CHECK(run("sketchInfo", {{"args", {a}}})["constraints"].size() == 5);
}
