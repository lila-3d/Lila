#include <doctest/doctest.h>

#include "TestUtil.h"

#include "curve/Blend.h"
#include "curve/BorderBlend.h"
#include "curve/Fair.h"
#include "curve/Fit.h"
#include "curve/Intersect.h"
#include "curve/Network.h"
#include "curve/Ops.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"
#include "geo/Topology.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <random>

using namespace lila;
using namespace lila::curve;
using D3 = glm::dvec3;
using namespace curvetest;

namespace
{
Spline rawSegment(const D3& a, const D3& b) { return poly({a, b}); }

D3 endPoint(const Spline& s, bool atEnd) { return pointAt(s, atEnd ? spanCount(s) : 0.0); }

/** Continuity at a join: one curve's geometry leaving it and the other's heading on from it agree up to order. */
void checkJoin(const Geometry& out, const Geometry& on, int order, double tol)
{
    CHECK(glm::distance(out.p, on.p) < tol);
    if (order >= 1)
        CHECK(glm::distance(out.t, on.t) < tol);
    if (order >= 2)
        CHECK(glm::distance(out.k, on.k) < tol * std::max(1.0, glm::length(on.k)));
    if (order >= 3)
        CHECK(glm::distance(out.j, on.j) < tol * 10.0 * std::max(1.0, glm::length(on.j)));
}

/** z = f(x, y) sampled along a line of the grid, as an interpolating NURBS. */
double bump(double x, double y) { return 0.004 * (x - 10.0) * (x - 10.0) - 0.003 * (y - 10.0) * (y - 10.0) + 0.05 * x; }

Spline surfaceLine(bool alongX, double fixed)
{
    std::vector<D3> pts;
    for (int i = 0; i <= 8; ++i)
    {
        double v = -2.0 + 3.0 * i;
        double x = alongX ? v : fixed, y = alongX ? fixed : v;
        pts.push_back(D3(x, y, bump(x, y)));
    }
    return interpolate(pts, 3, false);
}

/** A flat grid of quads in the xz plane at height y, nx by nz quads of size h, from (x0, z0). */
geo::Mesh gridMesh(double x0, double z0, double y, int nx, int nz, double h)
{
    geo::Mesh m;
    std::vector<geo::Index> idx;
    for (int j = 0; j <= nz; ++j)
        for (int i = 0; i <= nx; ++i)
            idx.push_back(m.addPoint(D3(x0 + i * h, y, z0 + j * h)));
    for (int j = 0; j < nz; ++j)
        for (int i = 0; i < nx; ++i)
        {
            std::vector<geo::Index> q = {idx[size_t(j * (nx + 1) + i)], idx[size_t(j * (nx + 1) + i + 1)], idx[size_t((j + 1) * (nx + 1) + i + 1)],
                                         idx[size_t((j + 1) * (nx + 1) + i)]};
            m.addFace(q);
        }
    return m;
}

/** The border chain of m whose points all have x == at. */
geo::EdgePath borderAtX(const geo::Mesh& m, double at)
{
    std::vector<geo::Index> edges;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e) && std::abs(m.position(m.edgePoint(e, 0)).x - at) < 1e-9 &&
            std::abs(m.position(m.edgePoint(e, 1)).x - at) < 1e-9)
            edges.push_back(e);
    std::vector<geo::EdgePath> chains = geo::chainEdges(m, edges);
    REQUIRE(chains.size() == 1);
    return chains[0];
}
} // namespace

TEST_CASE("curve intersections: 3D, projected, circles and Beziers")
{
    Spline a = rawSegment(D3(0, 0, 0), D3(10, 0, 0)), b = rawSegment(D3(4, -5, 0), D3(4, 5, 0));
    std::vector<CurveHit> h = intersect(a, b);
    REQUIRE(h.size() == 1);
    CHECK(h[0].ua == doctest::Approx(0.4));
    CHECK(h[0].ub == doctest::Approx(0.5));
    // Skew lines meet only when seen along z.
    Spline lifted = rawSegment(D3(4, -5, 3), D3(4, 5, 3));
    CHECK(intersect(a, lifted).empty());
    IntersectOptions view;
    view.direction = D3(0, 0, 1);
    REQUIRE(intersect(a, lifted, view).size() == 1);
    CHECK(intersect(a, lifted, view)[0].pa.x == doctest::Approx(4.0));

    Spline c = circle(D3(0), D3(0, 0, 1), 5.0);
    std::vector<CurveHit> hc = intersect(rawSegment(D3(-10, 0, 0), D3(10, 0, 0)), c);
    REQUIRE(hc.size() == 2);
    CHECK(hc[0].pa.x == doctest::Approx(-5.0).epsilon(1e-6));
    CHECK(hc[1].pa.x == doctest::Approx(5.0).epsilon(1e-6));
    for (const CurveHit& x : hc)
        CHECK(glm::distance(x.pa, x.pb) < 1e-6);

    // Two Bezier waves crossing several times: every hit is a true crossing.
    Spline w1 = catmullRom(std::vector<D3>{D3(0, 0, 0), D3(5, 4, 0), D3(10, -4, 0), D3(15, 4, 0), D3(20, 0, 0)}, false);
    Spline w2 = rawSegment(D3(-1, 0.5, 0), D3(21, 0.5, 0));
    std::vector<CurveHit> hw = intersect(w1, w2);
    CHECK(hw.size() == 4);
    for (const CurveHit& x : hw)
        CHECK(std::abs(pointAt(w1, x.ua).y - 0.5) < 1e-6);
    for (size_t i = 1; i < hw.size(); ++i)
        CHECK(hw[i].ua > hw[i - 1].ua);
    // Degenerate input gives nothing rather than failing.
    CHECK(intersect(Spline(), w2).empty());
    CHECK(intersect(poly({D3(1, 1, 1), D3(1, 1, 1)}), w2).empty());
}

TEST_CASE("trim, split and sub-curves")
{
    Spline a = rawSegment(D3(0, 0, 0), D3(10, 0, 0));
    Splines cutters = {rawSegment(D3(3, -1, 0), D3(3, 1, 0)), rawSegment(D3(7, -1, 0), D3(7, 1, 0))};
    std::vector<double> cuts = intersectionParams(a, cutters);
    REQUIRE(cuts.size() == 2);
    Splines left = trimAt(a, cuts, 0.5);
    REQUIRE(left.size() == 2);
    CHECK(length(left[0]) == doctest::Approx(3.0));
    CHECK(length(left[1]) == doctest::Approx(3.0));
    Splines endTrim = trimAt(a, cuts, 0.9);
    REQUIRE(endTrim.size() == 1);
    CHECK(length(endTrim[0]) == doctest::Approx(7.0));
    // No cuts: trimming deletes the whole curve.
    CHECK(trimAt(a, {}, 0.5).empty());
    Splines parts = splitAt(a, cuts);
    REQUIRE(parts.size() == 3);
    CHECK(length(parts[0]) + length(parts[1]) + length(parts[2]) == doctest::Approx(10.0));
    CHECK(parts[1].kind == Kind::Poly);

    // A closed circle cut twice leaves one open arc; split gives both arcs.
    Spline c = circle(D3(0), D3(0, 0, 1), 5.0);
    std::vector<double> cc = intersectionParams(c, {rawSegment(D3(-10, 0, 0), D3(10, 0, 0))});
    REQUIRE(cc.size() == 2);
    D3 top;
    double uTop = closestParam(c, D3(0, 5, 0), &top);
    Splines arc = trimAt(c, cc, uTop);
    REQUIRE(arc.size() == 1);
    CHECK(!arc[0].closed);
    CHECK(length(arc[0]) == doctest::Approx(glm::pi<double>() * 5.0).epsilon(1e-6));
    CHECK(pointAt(arc[0], spanCount(arc[0]) * 0.5).y < -4.9);
    Splines halves = splitAt(c, cc);
    REQUIRE(halves.size() == 2);
    CHECK(length(halves[0]) == doctest::Approx(length(halves[1])).epsilon(1e-6));

    // Open NURBS slices keep their degree, sit exactly on the original and stay clamped.
    Spline n = interpolate(std::vector<D3>{D3(0), D3(3, 4, 0), D3(7, -2, 1), D3(10, 3, 0), D3(14, 0, 2)}, 3, false);
    Spline mid = subCurve(n, 0.7, 2.6);
    REQUIRE(isValid(mid));
    CHECK(mid.kind == Kind::Nurbs);
    CHECK(mid.degree == 3);
    CHECK(glm::distance(endPoint(mid, false), pointAt(n, 0.7)) < 1e-9);
    CHECK(glm::distance(endPoint(mid, true), pointAt(n, 2.6)) < 1e-9);
    for (int i = 0; i <= 10; ++i)
    {
        D3 p = pointAt(mid, spanCount(mid) * i / 10.0), q;
        closestParam(n, p, &q);
        CHECK(glm::distance(p, q) < 1e-7);
    }
    // Bezier sub-curves stay Bezier.
    Spline bz = catmullRom(std::vector<D3>{D3(0), D3(5, 5, 0), D3(10, 0, 0), D3(15, 5, 0)}, false);
    Spline bs = subCurve(bz, 0.5, 2.25);
    CHECK(bs.kind == Kind::Bezier);
    CHECK(glm::distance(endPoint(bs, true), pointAt(bz, 2.25)) < 1e-9);
}

TEST_CASE("extend to a boundary: line, arc and smooth")
{
    Spline a = rawSegment(D3(0, 0, 0), D3(5, 0, 0));
    Spline wall = rawSegment(D3(10, -5, 0), D3(10, 5, 0));
    ExtendOptions o;
    o.mode = ExtendMode::Line;
    ExtendResult r = extendTo(a, true, {wall}, o);
    REQUIRE(r.hit);
    CHECK(r.added == doctest::Approx(5.0).epsilon(1e-6));
    CHECK(glm::distance(endPoint(r.curve, true), D3(10, 0, 0)) < 1e-6);
    CHECK(glm::distance(endPoint(r.curve, false), D3(0, 0, 0)) < 1e-9);
    // The start extends backward to a wall behind it.
    ExtendResult back = extendTo(a, false, {rawSegment(D3(-4, -5, 0), D3(-4, 5, 0))}, o);
    REQUIRE(back.hit);
    CHECK(glm::distance(endPoint(back.curve, false), D3(-4, 0, 0)) < 1e-6);
    // Nothing in reach: unchanged.
    ExtendResult miss = extendTo(a, true, {rawSegment(D3(10, 5, 0), D3(10, 9, 0))}, o);
    CHECK(!miss.hit);
    CHECK(length(miss.curve) == doctest::Approx(5.0));

    // An arc carries on round its circle.
    Spline quarter = arc(D3(0), D3(0, 0, 1), D3(10, 0, 0), glm::half_pi<double>());
    ExtendOptions ao;
    ao.mode = ExtendMode::Arc;
    ExtendResult ra = extendTo(quarter, true, {rawSegment(D3(-20, 0, 0), D3(0, 0, 0))}, ao);
    REQUIRE(ra.hit);
    CHECK(glm::distance(endPoint(ra.curve, true), D3(-10, 0, 0)) < 1e-5);
    for (int i = 0; i <= 20; ++i)
        CHECK(glm::length(pointAt(ra.curve, spanCount(ra.curve) * i / 20.0)) == doctest::Approx(10.0).epsilon(1e-6));

    // Smooth continues the end span's polynomial; the join keeps its tangent.
    Spline n = interpolate(std::vector<D3>{D3(0), D3(4, 3, 0), D3(8, 2, 0), D3(12, 4, 0)}, 3, false);
    ExtendOptions so;
    so.mode = ExtendMode::Smooth;
    ExtendResult rs = extendTo(n, true, {rawSegment(D3(18, -20, 0), D3(18, 20, 0))}, so);
    REQUIRE(rs.hit);
    CHECK(rs.curve.kind == Kind::Nurbs);
    CHECK(endPoint(rs.curve, true).x == doctest::Approx(18.0).epsilon(1e-6));
    double uJoin = closestParam(rs.curve, D3(12, 4, 0));
    Geometry before = geometryAt(rs.curve, uJoin - 1e-4, true), after = geometryAt(rs.curve, uJoin + 1e-4, true);
    CHECK(glm::dot(before.t, after.t) > 0.9999);
    // By length.
    Spline longer = extendBy(a, true, ExtendMode::Line, 2.5);
    CHECK(length(longer) == doctest::Approx(7.5));
    // Closed curves have no end to extend.
    CHECK(!extension(circle(D3(0), D3(0, 1, 0), 3.0), true, ExtendMode::Line, 5.0));
}

TEST_CASE("blend curves are exactly G0 to G3")
{
    Spline a = arc(D3(0), D3(0, 0, 1), D3(10, 0, 0), glm::half_pi<double>()); // ends at (0, 10)
    Spline b = interpolate(std::vector<D3>{D3(-20, 20, 2), D3(-24, 26, 1), D3(-30, 28, 0), D3(-36, 34, 0)}, 3, false);
    for (int ca = 0; ca <= 3; ++ca)
        for (int cb = 0; cb <= 3; ++cb)
        {
            BlendEnd ea{endGeometry(a, true), Continuity(ca), 1.0}, eb{endGeometry(b, false), Continuity(cb), 0.8};
            Spline bl = blendCurve(ea, eb);
            REQUIRE(isValid(bl));
            int degree = bl.kind == Kind::Nurbs ? bl.degree : bl.kind == Kind::Bezier ? 3 : 1;
            CHECK(degree == ca + cb + 1);
            checkJoin(ea.g, geometryAt(bl, 0.0, true), ca, 1e-7);
            checkJoin(eb.g, geometryAt(bl, spanCount(bl), false), cb, 1e-7);
        }
    // Bulge pulls the curve along the tangent.
    BlendEnd e1{endGeometry(a, true), Continuity::G1, 1.0}, e2{endGeometry(b, false), Continuity::G1, 1.0};
    Spline small = blendCurve(e1, e2);
    e1.bulge = 2.0;
    Spline big = blendCurve(e1, e2);
    CHECK(glm::distance(big.handleR[0], big.points[0]) == doctest::Approx(2.0 * glm::distance(small.handleR[0], small.points[0])));
    // Coincident ends cannot blend.
    CHECK_THROWS_AS(blendCurve(BlendEnd{endGeometry(a, true)}, BlendEnd{endGeometry(a, true)}), std::invalid_argument);

    // A mesh border: the blend leaves square to the edge within the face's plane.
    geo::Mesh m = gridMesh(0, 0, 0, 3, 3, 2.0);
    geo::EdgePath border = borderAtX(m, 6.0);
    std::optional<Geometry> g = borderGeometry(m, border.edges[1], 0.5);
    REQUIRE(g);
    CHECK(glm::distance(g->t, D3(1, 0, 0)) < 1e-9);
    CHECK(glm::length(g->k) < 1e-9);
    CHECK(!borderGeometry(m, m.findEdge(5, 6), 0.5));
}

TEST_CASE("match moves only the end and keeps the other")
{
    Spline target = interpolate(std::vector<D3>{D3(20, 0, 0), D3(24, 3, 0), D3(30, 4, 1), D3(36, 2, 0)}, 3, false);
    Spline n = interpolate(std::vector<D3>{D3(0, 0, 0), D3(5, 2, 0), D3(10, -1, 0), D3(15, 1, 0), D3(18, 3, 0)}, 3, false);
    for (int c = 0; c <= 2; ++c)
    {
        MatchOptions o;
        o.continuity = Continuity(c);
        Spline m = matchEnd(n, true, geometryAt(target, 0.0, true), o);
        REQUIRE(isValid(m));
        checkJoin(endGeometry(m, true), geometryAt(target, 0.0, true), c, 1e-6);
        Geometry before = endGeometry(n, false), after = endGeometry(m, false);
        CHECK(glm::distance(before.p, after.p) < 1e-9);
        CHECK(glm::distance(before.t, after.t) < 1e-9);
        CHECK(glm::distance(before.k, after.k) < 1e-6);
    }
    // Bezier: G2 on a one-span curve inserts a point so the far end keeps its tangent.
    Spline bz;
    bz.kind = Kind::Bezier;
    bz.points = {D3(0), D3(15, 0, 0)};
    bz.handleL = {D3(0), D3(10, 4, 0)};
    bz.handleR = {D3(5, 4, 0), D3(15, 0, 0)};
    bz.typeL = bz.typeR = {HandleType::Free, HandleType::Free};
    MatchOptions o2;
    o2.continuity = Continuity::G2;
    Spline mb = matchEnd(bz, false, geometryAt(target, spanCount(target), false), o2);
    CHECK(mb.kind == Kind::Bezier);
    CHECK(mb.size() == 3);
    checkJoin(endGeometry(mb, false), geometryAt(target, spanCount(target), false), 2, 1e-6);
    CHECK(glm::distance(endGeometry(mb, true).t, endGeometry(bz, true).t) < 1e-9);
    // A poly curve becomes a Bezier for tangency.
    Spline pl = poly({D3(0, 0, 0), D3(6, 1, 0), D3(12, 0, 0)});
    MatchOptions o1;
    Spline mp = matchEnd(pl, true, geometryAt(target, 0.0, true), o1);
    CHECK(mp.kind == Kind::Bezier);
    checkJoin(endGeometry(mp, true), geometryAt(target, 0.0, true), 1, 1e-7);
    // Meeting halfway: both ends move to the average and agree to G2.
    Geometry avg = averageGeometry(endGeometry(n, true), endGeometry(target, false));
    Spline na = matchEnd(n, true, avg, o2), ta = matchEnd(target, false, reversed(avg), o2);
    checkJoin(endGeometry(na, true), geometryAt(ta, 0.0, true), 2, 1e-6);
    CHECK(glm::distance(endPoint(na, true), (D3(18, 3, 0) + D3(20, 0, 0)) * 0.5) < 1e-9);
    // Rational curves keep their weights and still match.
    Spline c = arc(D3(0), D3(0, 0, 1), D3(10, 0, 0), glm::half_pi<double>() * 0.9);
    Spline mc = matchEnd(c, true, geometryAt(target, 0.0, true), o2);
    CHECK(mc.rational());
    checkJoin(endGeometry(mc, true), geometryAt(target, 0.0, true), 2, 1e-6);
    CHECK_THROWS_AS(matchEnd(circle(D3(0), D3(0, 1, 0), 2.0), true, geometryAt(target, 0.0, true)), std::invalid_argument);
}

TEST_CASE("fair evens curvature within tolerance")
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> noise(-0.15, 0.15);
    std::vector<D3> pts;
    for (int i = 0; i <= 24; ++i)
    {
        double x = i * 2.0;
        pts.push_back(D3(x, 6.0 * std::sin(x * 0.12) + noise(rng), noise(rng)));
    }
    Spline n = interpolate(pts, 3, false);
    FairOptions o;
    o.tolerance = 0.2;
    FairResult r = fair(n, o);
    CHECK(r.variationAfter < r.variationBefore * 0.8);
    CHECK(r.maxDeviation <= o.tolerance + 1e-9);
    CHECK(r.maxDeviation > 0.0);
    CHECK(r.curve.size() == n.size());
    CHECK(glm::distance(endPoint(r.curve, false), endPoint(n, false)) < 1e-12);
    CHECK(glm::distance(endPoint(r.curve, true), endPoint(n, true)) < 1e-12);
    CHECK(glm::distance(endGeometry(r.curve, false).t, endGeometry(n, false).t) < 1e-9);
    // A Bezier stays a Bezier with smooth points aligned; a corner stays a corner.
    Spline bz = fitBezier(tessellate(n).points, false, 0.05);
    bz.points.push_back(D3(48, -10, 0));
    bz.handleL.push_back(D3(48, -6, 0));
    bz.handleR.push_back(D3(48, -10, 0));
    bz.typeL.push_back(HandleType::Free);
    bz.typeR.push_back(HandleType::Free);
    bz.handleR[bz.size() - 2] = bz.points[bz.size() - 2] + D3(0, -2, 0);
    bz.typeR[bz.size() - 2] = HandleType::Free;
    FairResult rb = fair(bz, o);
    CHECK(rb.curve.kind == Kind::Bezier);
    CHECK(rb.maxDeviation <= o.tolerance + 1e-6);
    size_t corner = bz.size() - 2;
    CHECK(glm::distance(rb.curve.points[corner], bz.points[corner]) < 1e-12);
    // Closed curves fair all the way round.
    Spline loop = interpolate(std::vector<D3>{D3(10, 0, 0), D3(7, 7.4, 0), D3(0, 10, 0.3), D3(-7.2, 7, 0), D3(-10, 0, 0), D3(-7, -7, -0.2), D3(0, -10.4, 0), D3(7, -7, 0)}, 3, true);
    FairResult rl = fair(loop, o);
    CHECK(rl.curve.closed);
    CHECK(rl.maxDeviation <= o.tolerance + 1e-9);
    CHECK(rl.variationAfter <= rl.variationBefore);
}

TEST_CASE("curve network: families, order, smooth Gordon surface and poles")
{
    Splines curves;
    for (double y : {0.0, 9.0, 20.0})
        curves.push_back(surfaceLine(true, y));
    for (double x : {0.0, 6.0, 13.0, 20.0})
        curves.push_back(surfaceLine(false, x));
    // Shuffle and reverse a couple so sorting and orienting are exercised.
    std::swap(curves[0], curves[5]);
    reverse(curves[2]);
    reverse(curves[4]);
    NetworkOptions o;
    o.tolerance = 0.05;
    o.spansU = 3;
    o.spansV = 2;
    NetworkResult r = curveNetwork(curves, o);
    CHECK(r.warnings.empty());
    int u = 0, v = 0;
    for (int f : r.family)
        (f == 0 ? u : v) += 1;
    CHECK(((u == 3 && v == 4) || (u == 4 && v == 3)));
    const size_t cells = (r.uOrder.size() - 1) * (r.vOrder.size() - 1);
    CHECK(r.mesh.numFaces() == geo::Index(cells * 6));
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
        if (r.mesh.faceValid(f))
            CHECK(r.mesh.faceSize(f) == 4);
    CHECK(r.mesh.validate(geo::kValidateAll).empty());
    // Points sit on the sampled surface (it is smooth and the network samples it).
    double worst = 0.0;
    for (geo::Index p = 0; p < r.mesh.pointCapacity(); ++p)
        if (r.mesh.pointValid(p))
        {
            D3 q = r.mesh.position(p);
            worst = std::max(worst, std::abs(q.z - bump(q.x, q.y)));
        }
    CHECK(worst < 0.05);
    // Every network curve is an edge loop: mesh points all along it (crossings sit midway between curves that meet within tolerance).
    for (const Spline& c : curves)
    {
        int on = 0;
        for (geo::Index p = 0; p < r.mesh.pointCapacity(); ++p)
        {
            D3 at;
            closestParam(c, r.mesh.position(p), &at);
            on += glm::distance(at, r.mesh.position(p)) < 0.01;
        }
        CHECK(on >= 5);
    }

    // A three-sided patch: two V curves meeting at the top over two U curves.
    Splines tri = {rawSegment(D3(0, 0, 0), D3(10, 0, 0)), rawSegment(D3(1, 5, 0), D3(9, 5, 0)), rawSegment(D3(0, 0, 0), D3(5, 10, 0)), rawSegment(D3(10, 0, 0), D3(5, 10, 0))};
    NetworkOptions to;
    to.spansU = 2;
    to.spansV = 2;
    NetworkResult t = curveNetwork(tri, to);
    int tris = 0, quads = 0;
    for (geo::Index f = 0; f < t.mesh.faceCapacity(); ++f)
        if (t.mesh.faceValid(f))
            (t.mesh.faceSize(f) == 3 ? tris : quads) += 1;
    CHECK(tris == 2);
    CHECK(quads == 2 * 2 + 2);
    CHECK(t.mesh.validate().empty());

    // A stray curve is left out with a warning; a non-grid throws.
    Splines stray = tri;
    stray.push_back(rawSegment(D3(50, 50, 0), D3(60, 50, 0)));
    NetworkResult s = curveNetwork(stray, to);
    CHECK(s.family.back() == -1);
    CHECK(!s.warnings.empty());
    Splines bad = {rawSegment(D3(0, 0, 0), D3(10, 0, 0)), rawSegment(D3(5, -5, 0), D3(5, 5, 0)), rawSegment(D3(0, -3, 0), D3(10, 3, 0))};
    CHECK_THROWS_AS(curveNetwork(bad, to), std::invalid_argument);
}

TEST_CASE("profile blend between mesh borders")
{
    geo::Mesh a = gridMesh(0, 0, 0, 3, 5, 2.0);     // border at x = 6, six points
    geo::Mesh b = gridMesh(16, 0, 5, 3, 3, 10.0 / 3.0); // border at x = 16, four points
    BorderSide sa = borderSide(a, borderAtX(a, 6.0), D3(16, 5, 5));
    BorderSide sb = borderSide(b, borderAtX(b, 16.0), D3(6, 0, 5));
    for (const D3& d : sa.out)
        CHECK(glm::distance(d, D3(1, 0, 0)) < 1e-9);
    for (const D3& d : sb.out)
        CHECK(glm::distance(d, D3(-1, 0, 0)) < 1e-9);
    BorderBlendOptions o;
    o.segments = 8;
    BorderBlendResult r = borderBlend(sa, sb, {}, o);
    CHECK(r.mesh.numFaces() == 8 * 5);
    CHECK(r.mesh.validate(geo::kValidateAll).empty());
    REQUIRE(r.rowA.size() == 6);
    for (size_t c = 0; c < 6; ++c)
    {
        CHECK(glm::distance(r.mesh.position(r.rowA[c]), sa.points[r.stationsA[c].segment]) < 1e-9);
        CHECK(r.stationsA[c].t == 0.0);
    }
    // The first row leaves A within its plane (y = 0) and the last arrives in B's (y = 5): tangent continuity.
    for (size_t c = 0; c < 6; ++c)
    {
        D3 p0 = r.mesh.position(r.rowA[c]);
        D3 nb = r.mesh.position(r.rowB[c]);
        CHECK(nb.y == doctest::Approx(5.0));
        CHECK(std::abs(p0.y) < 1e-12);
    }
    // B's four points are all used, the rest of its stations fall between them.
    int exact = 0;
    for (const SideStation& st : r.stationsB)
        exact += st.t == 0.0;
    CHECK(exact == 4);
    // The strip winds on from A's faces: the shared border edge runs opposite ways in the two meshes.
    int againstA = 0;
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
        if (r.mesh.faceValid(f))
        {
            std::vector<geo::Index> pts = r.mesh.facePoints(f);
            for (size_t k = 0; k < pts.size(); ++k)
            {
                D3 p = r.mesh.position(pts[k]), q = r.mesh.position(pts[(k + 1) % pts.size()]);
                if (std::abs(p.x - 6.0) < 1e-9 && std::abs(q.x - 6.0) < 1e-9 && std::abs(p.y) < 1e-9 && std::abs(q.y) < 1e-9)
                    againstA += q.z < p.z ? 1 : -1;
            }
        }
    // A's faces wind counter-clockwise seen from +y, so along x = 6 they run +z; the strip must run -z.
    CHECK(againstA == 5);
    // A profile bulges the cross-section where it lies, eased out at the sides.
    Spline prof = catmullRom(std::vector<D3>{D3(6, 0, 0), D3(11, 6, 0), D3(16, 5, 0)}, false);
    BorderBlendResult rp = borderBlend(sa, sb, {prof}, o);
    double plain = r.mesh.position(r.rowA[0] + 4).y, shaped = rp.mesh.position(rp.rowA[0] + 4).y;
    CHECK(shaped > plain + 0.5);
    CHECK_THROWS_AS(borderBlend(sa, BorderSide{}, {}, o), std::invalid_argument);
}
