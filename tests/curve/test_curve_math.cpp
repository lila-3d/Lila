#include <doctest/doctest.h>

#include "curve/Fit.h"
#include "curve/Ops.h"
#include "curve/Spline.h"
#include "curve/Sweep.h"
#include "curve/Tessellate.h"
#include "geo/Bvh.h"
#include "geo/Primitives.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <random>

using namespace lila;
using namespace lila::curve;
using D3 = glm::dvec3;

namespace
{
Spline poly(std::vector<D3> pts, bool closed = false)
{
    Spline s;
    s.kind = Kind::Poly;
    s.closed = closed;
    s.points = std::move(pts);
    return s;
}

Spline randomBezier(std::mt19937& rng, int n, bool closed)
{
    std::uniform_real_distribution<double> d(-50.0, 50.0);
    Spline s;
    s.kind = Kind::Bezier;
    s.closed = closed;
    for (int i = 0; i < n; ++i)
    {
        D3 p(d(rng), d(rng), d(rng));
        s.points.push_back(p);
        s.handleL.push_back(p + D3(d(rng), d(rng), d(rng)) * 0.3);
        s.handleR.push_back(p + D3(d(rng), d(rng), d(rng)) * 0.3);
        s.typeL.push_back(HandleType::Free);
        s.typeR.push_back(HandleType::Free);
    }
    return s;
}

Spline randomNurbs(std::mt19937& rng, int n, int degree, bool closed, bool rational)
{
    std::uniform_real_distribution<double> d(-50.0, 50.0), w(0.5, 2.0);
    Spline s;
    s.kind = Kind::Nurbs;
    s.degree = degree;
    s.closed = closed;
    for (int i = 0; i < n; ++i)
    {
        s.points.push_back(D3(d(rng), d(rng), d(rng)));
        if (rational)
            s.weights.push_back(w(rng));
    }
    return s;
}

double maxEdgeAngle(const Polyline& pl)
{
    double worst = 0.0;
    for (size_t i = 0; i < pl.size(); ++i)
        if (!pl.kink[i])
            worst = std::max(worst, std::acos(std::clamp(glm::dot(pl.tangentIn[i], pl.tangentOut[i]), -1.0, 1.0)));
    return worst;
}

bool allQuads(const geo::Mesh& m)
{
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceSize(f) != 4)
            return false;
    return true;
}

int boundaryEdges(const geo::Mesh& m)
{
    int n = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            ++n;
    return n;
}

bool manifold(const geo::Mesh& m)
{
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) > 2)
            return false;
    return true;
}

bool uvsIn(const geo::Mesh& m, float lo, float hi)
{
    auto uv = m.cornerAttrs().get<glm::vec2>("uv0");
    if (uv.empty())
        return false;
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c) && (uv[c].x < lo - 1e-5f || uv[c].x > hi + 1e-5f || uv[c].y < lo - 1e-5f || uv[c].y > hi + 1e-5f))
            return false;
    return true;
}

/** Normals point away from the centroid on the whole (closed meshes). */
double outwardness(const geo::Mesh& m)
{
    D3 c(0.0);
    int n = 0;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            c += m.position(p);
            ++n;
        }
    c /= double(std::max(n, 1));
    double sum = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            sum += glm::dot(m.faceAreaVector(f), m.faceCentroid(f) - c);
    return sum;
}
} // namespace

TEST_CASE("circle is an exact rational quadratic")
{
    Spline c = circle(D3(1, 2, 3), D3(0.3, 1, -0.2), 7.5);
    REQUIRE(isValid(c));
    CHECK(spanCount(c) == 4);
    double worst = 0.0;
    D3 n = glm::normalize(D3(0.3, 1, -0.2));
    for (int i = 0; i <= 400; ++i)
    {
        D3 p = pointAt(c, 4.0 * i / 400.0);
        worst = std::max(worst, std::abs(glm::distance(p, D3(1, 2, 3)) - 7.5));
        CHECK(std::abs(glm::dot(p - D3(1, 2, 3), n)) < 1e-9);
    }
    CHECK(worst < 1e-12);
    // Arcs of any sweep too.
    Spline a = arc(D3(0.0), D3(0, 0, 1), D3(5, 0, 0), 4.0);
    for (int i = 0; i <= 100; ++i)
        CHECK(std::abs(glm::length(pointAt(a, spanCount(a) * i / 100.0)) - 5.0) < 1e-12);
    CHECK(glm::distance(pointAt(a, spanCount(a)), D3(5 * std::cos(4.0), 5 * std::sin(4.0), 0)) < 1e-12);
    // Knot insertion into the periodic circle keeps it a circle.
    Spline c2 = c;
    insertKnot(c2, 0.5, 1);
    CHECK(c2.size() == 9);
    for (int i = 0; i <= 200; ++i)
        CHECK(std::abs(glm::distance(pointAt(c2, spanCount(c2) * i / 200.0), D3(1, 2, 3)) - 7.5) < 1e-9);
}

TEST_CASE("Bezier and its NURBS form agree")
{
    std::mt19937 rng(7);
    for (bool closed : {false, true})
    {
        Spline b = randomBezier(rng, 6, closed);
        Spline n = toNurbs(b);
        REQUIRE(isValid(n));
        CHECK(spanCount(n) == spanCount(b));
        for (int i = 0; i <= 300; ++i)
        {
            double u = spanCount(b) * i / 300.0;
            CHECK(glm::distance(pointAt(b, u), pointAt(n, u)) < 1e-9);
        }
        Spline back = toBezier(n);
        CHECK(back.kind == Kind::Bezier);
        CHECK(back.size() == b.size());
        for (int i = 0; i <= 100; ++i)
        {
            double u = spanCount(b) * i / 100.0;
            CHECK(glm::distance(pointAt(b, u), pointAt(back, u)) < 1e-9);
        }
    }
    Spline p = poly({D3(0), D3(10, 0, 0), D3(10, 5, 0)}, true);
    Spline pn = toNurbs(p);
    for (int i = 0; i <= 30; ++i)
        CHECK(glm::distance(pointAt(p, 3.0 * i / 30), pointAt(pn, 3.0 * i / 30)) < 1e-12);
}

TEST_CASE("knot insertion leaves the shape alone")
{
    std::mt19937 rng(11);
    for (int degree : {2, 3, 5})
        for (bool closed : {false, true})
            for (bool rational : {false, true})
            {
                Spline s = randomNurbs(rng, 9, degree, closed, rational);
                REQUIRE(isValid(s));
                std::vector<D3> before;
                std::vector<double> ks;
                for (int i = 0; i <= 120; ++i)
                {
                    double k = knotParam(s, spanCount(s) * i / 120.0);
                    ks.push_back(k);
                    before.push_back(pointAt(s, spanParam(s, k)));
                }
                Spline t = s;
                insertKnot(t, 2.37, 1);
                insertKnot(t, 0.5, degree);
                CHECK(t.size() == s.size() + size_t(degree) + 1);
                double worst = 0.0;
                for (size_t i = 0; i < ks.size(); ++i)
                    worst = std::max(worst, glm::distance(before[i], pointAt(t, spanParam(t, ks[i]))));
                CHECK(worst < 1e-7);
            }
}

TEST_CASE("tessellation keeps kinks")
{
    Spline b;
    b.kind = Kind::Bezier;
    b.points = {D3(0), D3(10, 0, 0), D3(10, 10, 0)};
    b.handleL = b.handleR = b.points;
    b.typeL.assign(3, HandleType::Auto);
    b.typeR.assign(3, HandleType::Auto);
    // A curved first span, then a corner at the middle point.
    b.handleR[0] = D3(3, 4, 0);
    b.handleL[1] = D3(7, 4, 0);
    b.handleR[1] = D3(10, 3, 0);
    b.handleL[2] = D3(10, 7, 0);
    b.typeL.assign(3, HandleType::Free);
    b.typeR.assign(3, HandleType::Free);
    for (TessMode mode : {TessMode::Fixed, TessMode::Adaptive, TessMode::Game})
    {
        TessOptions o;
        o.mode = mode;
        o.maxAngle = 0.1;
        Polyline pl = tessellate(b, o);
        bool found = false;
        for (size_t i = 0; i < pl.size(); ++i)
            if (glm::distance(pl.points[i], D3(10, 0, 0)) < 1e-12)
            {
                found = true;
                CHECK(pl.kink[i]);
            }
        CHECK(found);
        int kinks = 0;
        for (uint8_t k : pl.kink)
            kinks += k;
        CHECK(kinks == 1);
        if (mode == TessMode::Game)
        {
            // The straight second span is a single segment; the curved one turns no more than maxAngle per segment.
            CHECK(glm::distance(pl.points[pl.size() - 2], D3(10, 0, 0)) < 1e-12);
            CHECK(maxEdgeAngle(pl) < 0.1 + 1e-6);
        }
    }
}

TEST_CASE("rotation minimising frames don't flip and close up")
{
    // A trefoil-ish closed space curve.
    std::vector<D3> pts;
    for (int i = 0; i < 200; ++i)
    {
        double t = glm::two_pi<double>() * i / 200;
        pts.push_back(D3(std::sin(t) + 2 * std::sin(2 * t), std::cos(t) - 2 * std::cos(2 * t), -std::sin(3 * t)) * 10.0);
    }
    Spline s = interpolate(pts, 3, true);
    TessOptions to;
    to.mode = TessMode::Fixed;
    to.perSpan = 4;
    Polyline pl = tessellate(s, to);
    std::vector<Frame> fr = frames(pl);
    REQUIRE(fr.size() == pl.size());
    for (size_t i = 0; i < fr.size(); ++i)
    {
        CHECK(std::abs(glm::dot(fr[i].n, fr[i].t)) < 1e-9);
        CHECK(std::abs(glm::length(fr[i].n) - 1.0) < 1e-9);
        size_t j = (i + 1) % fr.size();
        // Neighbouring normals stay close: no flips anywhere, the seam included.
        CHECK(glm::dot(fr[i].n, fr[j].n) > 0.9);
    }
    // Open: a planar curve's frames keep the plane normal.
    Spline arcS = arc(D3(0.0), D3(0, 1, 0), D3(10, 0, 0), 3.0);
    Polyline apl = tessellate(arcS);
    for (const Frame& f : frames(apl))
        CHECK(std::abs(std::abs(f.n.y) - 1.0) < 1e-9);
    // Fixed up keeps the normal toward up.
    FrameOptions fo;
    fo.mode = FrameMode::FixedUp;
    for (const Frame& f : frames(apl, fo))
        CHECK(f.n.y > 0.99);
}

TEST_CASE("fit, interpolate and rebuild")
{
    std::vector<D3> pts;
    for (int i = 0; i <= 60; ++i)
    {
        double a = glm::pi<double>() * i / 60;
        pts.push_back(D3(std::cos(a), std::sin(a), 0) * 20.0);
    }
    Spline fit = fitBezier(pts, false, 0.05);
    CHECK(fit.kind == Kind::Bezier);
    CHECK(fit.size() <= 7);
    for (const D3& p : pts)
    {
        D3 at;
        closestParam(fit, p, &at);
        CHECK(glm::distance(at, p) < 0.06);
    }
    // EP curve: the points are the edit points.
    std::vector<D3> eps = {D3(0), D3(10, 5, 0), D3(20, -3, 4), D3(30, 0, 0), D3(42, 8, -2)};
    for (int degree : {1, 2, 3, 5})
    {
        Spline ep = interpolate(eps, degree, false);
        REQUIRE(isValid(ep));
        std::vector<D3> got = editPoints(ep);
        REQUIRE(got.size() == eps.size());
        for (size_t i = 0; i < eps.size(); ++i)
            CHECK(glm::distance(got[i], eps[i]) < 1e-8);
    }
    Spline loop = interpolate(eps, 3, true);
    std::vector<D3> got = editPoints(loop);
    REQUIRE(got.size() == eps.size());
    for (size_t i = 0; i < eps.size(); ++i)
        CHECK(glm::distance(got[i], eps[i]) < 1e-8);
    // Rebuild to 6 uniform spans keeps the shape close.
    RebuildOptions ro;
    ro.spans = 6;
    Spline rb = rebuild(fit, ro);
    CHECK(spanCount(rb) == 6);
    for (int i = 0; i <= 20; ++i)
    {
        D3 at;
        closestParam(rb, pointAt(fit, spanCount(fit) * i / 20.0), &at);
        CHECK(glm::distance(at, pointAt(fit, spanCount(fit) * i / 20.0)) < 0.1);
    }
    // Rebuild keeps corners.
    Spline sq = poly({D3(0), D3(10, 0, 0), D3(10, 10, 0), D3(0, 10, 0)}, true);
    ro.kind = Kind::Nurbs;
    ro.spans = 8;
    Spline rs = rebuild(sq, ro);
    for (const D3& c : sq.points)
    {
        D3 at;
        closestParam(rs, c, &at);
        CHECK(glm::distance(at, c) < 1e-6);
    }
    // Simplify a dense poly line.
    std::vector<D3> line;
    for (int i = 0; i <= 50; ++i)
        line.push_back(D3(i, 0.001 * (i % 2), 0));
    CHECK(simplify(poly(line), 0.01).size() == 2);
}

TEST_CASE("interpolating draw modes pass through their points")
{
    std::vector<D3> pts = {D3(0), D3(10, 8, 0), D3(20, 0, 0), D3(30, 10, 5), D3(40, 2, 0)};
    Spline cr = catmullRom(pts, false);
    CHECK(cr.kind == Kind::Bezier);
    CHECK(cr.size() == pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        CHECK(glm::distance(cr.points[i], pts[i]) < 1e-12);
    for (bool closed : {false, true})
    {
        Spline k = kappaCurve(pts, closed);
        REQUIRE(isValid(k));
        size_t first = closed ? 0 : 1, last = closed ? pts.size() : pts.size() - 1;
        for (size_t i = first; i < last; ++i)
        {
            D3 at;
            closestParam(k, pts[i], &at);
            CHECK(glm::distance(at, pts[i]) < 1e-3);
        }
        Polyline pl = tessellate(k);
        int kinks = 0;
        for (uint8_t f : pl.kink)
            kinks += f;
        CHECK(kinks == 0);
    }
}

TEST_CASE("fillet arcs are tangent and exact")
{
    Spline l = poly({D3(0), D3(20, 0, 0), D3(20, 20, 0), D3(40, 20, 0)});
    FilletOptions o;
    o.radius = 5.0;
    Spline f = fillet(l, o);
    REQUIRE(isValid(f));
    CHECK(f.kind == Kind::Nurbs);
    Polyline pl = tessellate(f);
    for (uint8_t k : pl.kink)
        CHECK(k == 0);
    // The first corner's arc stays 5 from its centre (15, 5).
    for (size_t i = 0; i < pl.size(); ++i)
    {
        const D3& p = pl.points[i];
        if (p.x > 15.0 + 1e-9 && p.y < 5.0 - 1e-9)
            CHECK(std::abs(glm::distance(p, D3(15, 5, 0)) - 5.0) < 1e-9);
    }
    // Ends unchanged.
    CHECK(glm::distance(pointAt(f, 0.0), D3(0)) < 1e-12);
    CHECK(glm::distance(pointAt(f, spanCount(f)), D3(40, 20, 0)) < 1e-12);

    o.mode = CornerMode::Chamfer;
    Spline c = fillet(l, o);
    CHECK(c.kind == Kind::Poly);
    CHECK(c.size() == 6);

    o.mode = CornerMode::G2;
    o.radius = 3.0;
    Spline g = fillet(l, o);
    std::vector<Piece> ps = pieces(g);
    // Curvature continuous where blends meet the straight runs (zero there).
    for (const Piece& p : ps)
        if (!isStraight(p))
        {
            CHECK(curvature(evaluate(p, 0.0)) < 1e-9);
            CHECK(curvature(evaluate(p, 1.0)) < 1e-9);
            double minR = 1e30;
            for (int i = 0; i <= 200; ++i)
                if (double k = curvature(evaluate(p, i / 200.0)); k > 1e-12)
                    minR = std::min(minR, 1.0 / k);
            CHECK(minR == doctest::Approx(3.0).epsilon(0.02));
        }
    Polyline gpl = tessellate(g);
    for (uint8_t k : gpl.kink)
        CHECK(k == 0);

    // Per point radius and a closed square.
    Spline sq = poly({D3(0), D3(10, 0, 0), D3(10, 10, 0), D3(0, 10, 0)}, true);
    sq.cornerRadius = {1.0, 2.0, 0.0, 3.0};
    FilletOptions po;
    po.radius = 0.0;
    Spline fs = fillet(sq, po);
    Polyline fpl = tessellate(fs);
    int kinks = 0;
    for (uint8_t k : fpl.kink)
        kinks += k;
    CHECK(kinks == 1); // the corner without a radius
}

TEST_CASE("offset")
{
    Spline sq = poly({D3(0), D3(10, 0, 0), D3(10, 0, 10), D3(0, 0, 10)}, true);
    OffsetOptions o;
    o.distance = 1.0;
    o.join = JoinType::Miter;
    Splines out = offset(sq, o);
    REQUIRE(out.size() == 1);
    double lo = 1e9, hi = -1e9;
    for (const D3& p : out[0].points)
    {
        lo = std::min(lo, p.x);
        hi = std::max(hi, p.x);
        CHECK(std::abs(p.y) < 1e-9);
    }
    CHECK(lo == doctest::Approx(-1.0));
    CHECK(hi == doctest::Approx(11.0));
    o.distance = -1.0;
    out = offset(sq, o);
    REQUIRE(out.size() == 1);
    for (const D3& p : out[0].points)
        CHECK((p.x > 0.99 && p.x < 9.01));
    // Open: a U shape offset inward past its width loses the loop.
    Spline u = poly({D3(0, 0, 10), D3(0, 0, 0), D3(4, 0, 0), D3(4, 0, 10)});
    o.distance = 1.0;
    o.join = JoinType::Round;
    o.normal = D3(0, 1, 0);
    out = offset(u, o);
    REQUIRE(!out.empty());
    for (const Spline& s : out)
        for (const D3& p : s.points)
        {
            D3 at;
            closestParam(u, p, &at);
            CHECK(glm::distance(at, p) > 0.98);
        }
}

TEST_CASE("project onto a mesh")
{
    geo::Mesh plane = geo::makePlane({200, 200, 4, 4});
    geo::MeshBvh bvh(plane);
    Spline s = poly({D3(-50, 30, 0), D3(50, 40, 10)});
    ProjectOptions o;
    o.direction = D3(0, -1, 0);
    Splines out = project(s, plane, bvh, o);
    REQUIRE(out.size() == 1);
    for (const D3& p : out[0].points)
        CHECK(std::abs(p.y) < 1e-9);
    o.direction = D3(0.0);
    o.offset = 2.0;
    out = project(s, plane, bvh, o);
    REQUIRE(out.size() == 1);
    for (const D3& p : out[0].points)
        CHECK(std::abs(std::abs(p.y) - 2.0) < 1e-9);
}

TEST_CASE("attach, detach, open and close keep the shape")
{
    std::mt19937 rng(3);
    Spline a = randomBezier(rng, 4, false), b = randomBezier(rng, 3, false);
    Spline j = attach(a, b, false, AttachEnds::EndStart);
    CHECK(j.kind == Kind::Bezier);
    CHECK(j.size() == a.size() + b.size());
    for (int i = 0; i <= 30; ++i)
        CHECK(glm::distance(pointAt(j, 3.0 * i / 30), pointAt(a, 3.0 * i / 30)) < 1e-9);
    Splines parts = detach(a, 1.5);
    REQUIRE(parts.size() == 2);
    CHECK(glm::distance(pointAt(parts[0], spanCount(parts[0])), pointAt(a, 1.5)) < 1e-9);
    CHECK(glm::distance(pointAt(parts[1], 0.0), pointAt(a, 1.5)) < 1e-9);
    CHECK(glm::distance(pointAt(parts[1], 1.0), pointAt(a, 2.0)) < 1e-9);

    Spline c = closeCurve(a, false);
    CHECK(c.closed);
    CHECK(spanCount(c) == spanCount(a) + 1);
    Spline o = openCurve(c, 2.0);
    CHECK(!o.closed);
    CHECK(glm::distance(pointAt(o, 0.0), a.points[2]) < 1e-12);

    Spline n = randomNurbs(rng, 7, 3, true, true);
    Spline on = openCurve(n, 1.25);
    CHECK(glm::distance(pointAt(on, 0.0), pointAt(n, 1.25)) < 1e-9);
    CHECK(glm::distance(pointAt(on, spanCount(on)), pointAt(n, 1.25)) < 1e-9);

    Spline ins = a;
    size_t at = insertPointAt(ins, 1.5);
    CHECK(at == 2);
    CHECK(ins.size() == a.size() + 1);
    CHECK(glm::distance(ins.points[2], pointAt(a, 1.5)) < 1e-9);
    for (int i = 0; i <= 30; ++i)
    {
        D3 p = pointAt(a, 3.0 * i / 30), q;
        closestParam(ins, p, &q);
        CHECK(glm::distance(p, q) < 1e-7);
    }

    Spline r = a;
    reverse(r);
    for (int i = 0; i <= 30; ++i)
        CHECK(glm::distance(pointAt(r, 3.0 * i / 30), pointAt(a, 3.0 - 3.0 * i / 30)) < 1e-9);
    Spline rn = n;
    rn.knots = {0, 1, 1.5, 3, 4, 4.2, 6, 7};
    Spline rn2 = rn;
    reverse(rn2);
    int sp = spanCount(rn);
    for (int i = 0; i <= 40; ++i)
    {
        D3 p = pointAt(rn, sp * i / 40.0), q;
        closestParam(rn2, p, &q);
        CHECK(glm::distance(p, q) < 1e-6);
    }
}

TEST_CASE("sweep topology and uvs")
{
    Spline path = poly({D3(0), D3(0, 0, 50), D3(30, 0, 80), D3(30, 0, 120), D3(60, 20, 140)});
    Path p = pathFrom(path);
    REQUIRE(p.size() == 5);
    Profile prof = circleProfile(8, 5.0);
    SweepResult r = sweep(prof, p);
    CHECK(r.mesh.numFaces() == 8 * 4);
    CHECK(r.mesh.numPoints() == 8 * 5);
    CHECK(allQuads(r.mesh));
    CHECK(manifold(r.mesh));
    CHECK(boundaryEdges(r.mesh) == 16);
    CHECK(uvsIn(r.mesh, 0.0f, 1.0f));
    // Mitered: the kink ring keeps the tube's width measured square to each run.
    const SweepRing& k = r.rings[1];
    for (geo::Index j = 0; j < 8; ++j)
    {
        D3 q = r.mesh.position(k.firstPoint + j) - D3(0, 0, 50);
        D3 radialIn = q - D3(0, 0, 1) * glm::dot(q, D3(0, 0, 1));
        CHECK(glm::length(radialIn) == doctest::Approx(5.0).epsilon(1e-9));
    }
    // Caps: quads only with grid caps, closed and outward.
    SweepOptions o;
    o.startCap = o.endCap = CapMode::Grid;
    r = sweep(prof, p, o);
    CHECK(allQuads(r.mesh));
    CHECK(boundaryEdges(r.mesh) == 0);
    CHECK(r.mesh.numFaces() == 8 * 4 + 4 + 4);
    CHECK(outwardness(r.mesh) > 0.0);
    o.startCap = o.endCap = CapMode::Round;
    o.roundSegments = 3;
    r = sweep(prof, p, o);
    CHECK(allQuads(r.mesh));
    CHECK(boundaryEdges(r.mesh) == 0);
    CHECK(outwardness(r.mesh) > 0.0);
    // Parts: a split adds a ring and a second part with its own V range.
    o = {};
    o.splits = {60.0};
    o.uv.perCm = 0.0;
    r = sweep(prof, p, o);
    CHECK(r.parts == 2);
    CHECK(r.rings.size() == 6);
    CHECK(uvsIn(r.mesh, 0.0f, 1.0f));
    auto part = r.mesh.faceAttrs().get<int32_t>("part");
    REQUIRE(!part.empty());
    int ones = 0;
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
        if (r.mesh.faceValid(f))
            ones += part[f];
    CHECK(ones == 8 * 3);
    // Closed path: a torus with no border.
    Spline ring = circle(D3(0.0), D3(0, 1, 0), 40.0);
    TessOptions to;
    to.mode = TessMode::Fixed;
    to.perSpan = 6;
    r = sweep(prof, pathFrom(ring, to));
    CHECK(allQuads(r.mesh));
    CHECK(boundaryEdges(r.mesh) == 0);
    CHECK(r.mesh.numFaces() == 8 * 24);
    CHECK(outwardness(r.mesh) > 0.0);
}

TEST_CASE("revolve and loft topology")
{
    Spline prof = poly({D3(10, 0, 0), D3(10, 20, 0), D3(5, 30, 0)});
    RevolveOptions ro;
    ro.segments = 12;
    geo::Mesh m = revolve(tessellate(prof), ro);
    CHECK(m.numFaces() == 24);
    CHECK(allQuads(m));
    CHECK(manifold(m));
    CHECK(uvsIn(m, 0.0f, 1.0f));
    // A profile from the axis to the axis closes into a sphere-like shell with poles.
    Spline dome = arc(D3(0.0), D3(0, 0, 1), D3(0, -10, 0), glm::pi<double>());
    m = revolve(tessellate(dome), ro);
    CHECK(boundaryEdges(m) == 0);
    CHECK(manifold(m));
    CHECK(outwardness(m) > 0.0);
    // Partial with caps.
    Spline sq = poly({D3(10, 0, 0), D3(20, 0, 0), D3(20, 10, 0), D3(10, 10, 0)}, true);
    ro.endAngle = glm::pi<double>();
    m = revolve(tessellate(sq), ro);
    CHECK(boundaryEdges(m) == 0);
    CHECK(allQuads(m));

    std::vector<Spline> secs = {circle(D3(0), D3(0, 1, 0), 10.0), circle(D3(0, 20, 0), D3(0, 1, 0), 15.0),
                                circle(D3(0, 40, 0), D3(0, 1, 0), 8.0)};
    LoftOptions lo;
    lo.samples = 16;
    lo.spansBetween = 3;
    m = loft(secs, lo);
    CHECK(m.numFaces() == 16 * 6);
    CHECK(allQuads(m));
    CHECK(manifold(m));
    CHECK(boundaryEdges(m) == 32);
    CHECK(outwardness(m) > 0.0);
    lo.caps = true;
    m = loft(secs, lo);
    CHECK(boundaryEdges(m) == 0);
    CHECK(allQuads(m));
    // Squares with their corners lined up keep hard columns.
    std::vector<Spline> boxes = {poly({D3(0), D3(10, 0, 0), D3(10, 0, 10), D3(0, 0, 10)}, true),
                                 poly({D3(0, 20, 10), D3(0, 20, 0), D3(10, 20, 0), D3(10, 20, 10)}, true)};
    lo = {};
    m = loft(boxes, lo);
    CHECK(m.numFaces() == 4);
    auto hard = m.edgeAttrs().get<bool>("hard");
    int hardCount = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && hard.size() > e && hard[e])
            ++hardCount;
    CHECK(hardCount == 4);
    // A rail moves the in-between sections.
    Spline rail = poly({D3(10, 0, 0), D3(30, 20, 0), D3(8, 40, 0)});
    lo = {};
    lo.samples = 16;
    lo.spansBetween = 2;
    lo.smooth = false;
    m = loft(secs, lo, {rail});
    CHECK(allQuads(m));

    // Coons patch.
    std::array<Spline, 4> sides = {poly({D3(0), D3(10, 0, 0)}), poly({D3(10, 0, 0), D3(10, 0, 10)}), poly({D3(10, 0, 10), D3(0, 0, 10)}),
                                   poly({D3(0, 0, 10), D3(0)})};
    m = coonsPatch(sides, 4, 3);
    CHECK(m.numFaces() == 12);
    CHECK(allQuads(m));
}

TEST_CASE("offset an open corner without a plane given")
{
    Spline l = poly({D3(0), D3(20, 0, 0), D3(20, 20, 0)});
    OffsetOptions o;
    o.distance = 2.0;
    o.join = JoinType::Miter;
    Splines out = offset(l, o);
    REQUIRE(out.size() == 1);
    CHECK(out[0].size() == 3);
    o.distance = -2.0;
    out = offset(l, o);
    REQUIRE(out.size() == 1);
}

#include "gn/Field.h"
#include "gn/Geometry.h"

TEST_CASE("geometry node curve sets carry splines")
{
    gn::CurveSet cs;
    cs.addCurve(std::vector<D3>{D3(0), D3(1, 0, 0)}, false);
    CHECK(cs.isPoly());
    Spline c = circle(D3(0.0), D3(0, 1, 0), 10.0);
    cs.addSpline(c);
    Spline b = toBezier(poly({D3(0), D3(10, 0, 0), D3(10, 10, 0)}));
    b.radius = {1.0, 2.0, 3.0};
    cs.addSpline(b);
    CHECK(!cs.isPoly());
    Spline back = cs.spline(1);
    CHECK(back.kind == Kind::Nurbs);
    CHECK(back.degree == 2);
    for (int i = 0; i <= 20; ++i)
        CHECK(glm::distance(pointAt(back, 4.0 * i / 20), pointAt(c, 4.0 * i / 20)) < 1e-5);
    CHECK(cs.spline(2).radius.size() == 3);
    // The first poly curve keeps radius 1 though a later one brought the layer.
    CHECK(cs.spline(0).radius == std::vector<double>{1.0, 1.0});
    gn::CurveSet e = cs.evaluated();
    CHECK(e.isPoly());
    CHECK(e.numCurves() == 3);
    CHECK(e.curveSize(1) > 8);
    for (const D3& p : e.curvePoints(1))
        CHECK(std::abs(glm::length(p) - 10.0) < 1e-5);
    gn::transformCurves(cs, glm::translate(glm::dmat4(1.0), D3(0, 5, 0)));
    CHECK(cs.spline(2).handleR[0].y == doctest::Approx(5.0));

    // Frames around a closed loop close up.
    std::vector<D3> loop;
    for (int i = 0; i < 64; ++i)
    {
        double t = glm::two_pi<double>() * i / 64;
        loop.push_back(D3(std::cos(t), std::sin(2 * t) * 0.5, std::sin(t)) * 20.0);
    }
    std::vector<D3> tan, nrm;
    gn::curveFrames(loop, true, tan, nrm);
    CHECK(glm::dot(nrm.front(), nrm.back()) > 0.9);
}

TEST_CASE("fillet keeps per-point radius and tilt")
{
    Spline b = convert(poly({D3(0), D3(20, 0, 0), D3(20, 20, 0), D3(40, 20, 0)}), Kind::Bezier);
    b.radius = {1.0, 1.0, 2.0, 3.0};
    b.tilt = {0.0, 0.0, 0.0, 0.5};
    for (bool exact : {false, true})
    {
        FilletOptions o;
        o.radius = 5.0;
        o.exactArcs = exact;
        Spline f = fillet(b, o);
        CHECK(f.kind == (exact ? Kind::Nurbs : Kind::Bezier));
        REQUIRE(f.radius.size() == f.size());
        REQUIRE(f.tilt.size() == f.size());
        const int spans = spanCount(f);
        CHECK(pointValueAt(f, f.radius, 1.0, 0.0) == doctest::Approx(1.0));
        CHECK(pointValueAt(f, f.radius, 1.0, spans) == doctest::Approx(3.0));
        CHECK(pointValueAt(f, f.tilt, 0.0, spans) == doctest::Approx(0.5));
        for (size_t i = 1; i < f.size(); ++i)
            CHECK(f.radius[i] >= f.radius[i - 1] - 1e-12);
    }
}
