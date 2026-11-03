#include "curve/Blend.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "curve/Ops.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace lila::curve
{
namespace
{
using geo::Index;

/** Point and first three derivatives of a rational Bezier piece at t. */
std::array<D3, 4> pieceDerivatives(const Piece& pc, double t)
{
    std::array<D3, 4> out{};
    std::vector<D4> q = pc.cv;
    std::array<D4, 4> A{};
    int n = pc.degree();
    for (int k = 0; k <= 3; ++k)
    {
        if (k > n || q.empty())
            break;
        std::vector<D4> c = q;
        for (size_t r = 1; r < c.size(); ++r)
            for (size_t i = 0; i + r < c.size(); ++i)
                c[i] = c[i] * (1.0 - t) + c[i + 1] * t;
        A[size_t(k)] = c[0];
        std::vector<D4> d;
        for (size_t i = 0; i + 1 < q.size(); ++i)
            d.push_back((q[i + 1] - q[i]) * double(int(q.size()) - 1));
        q = std::move(d);
    }
    double w = A[0].w;
    if (std::abs(w) < kEps)
        return out;
    D3 C0 = D3(A[0]) / w;
    D3 C1 = (D3(A[1]) - A[1].w * C0) / w;
    D3 C2 = (D3(A[2]) - 2.0 * A[1].w * C1 - A[2].w * C0) / w;
    D3 C3 = (D3(A[3]) - 3.0 * A[1].w * C2 - 3.0 * A[2].w * C1 - A[3].w * C0) / w;
    return {C0, C1, C2, C3};
}

/** Arc-length derivatives from parametric ones (r1..r3 with respect to any parameter running forward). */
Geometry arcLengthGeometry(const D3& p, const D3& r1, const D3& r2, const D3& r3)
{
    Geometry g;
    g.p = p;
    double v = glm::length(r1);
    if (v < kEps)
        return g;
    double v2 = v * v, v3 = v2 * v, v5 = v3 * v2;
    double w = 1.0 / v, d12 = glm::dot(r1, r2);
    double w1 = -d12 / v3;
    double w2 = -(glm::dot(r2, r2) + glm::dot(r1, r3)) / v3 + 3.0 * d12 * d12 / v5;
    g.t = r1 * w;
    g.k = w * (w1 * r1 + w * r2);
    g.j = w * ((w1 * w1 + w * w2) * r1 + 3.0 * w * w1 * r2 + w * w * r3);
    return g;
}

/** Non-zero basis functions of degree p on knot interval i and their first n derivatives at u (Piegl & Tiller A2.3). */
std::vector<std::vector<double>> basisDerivatives(int i, double u, int p, int n, const std::vector<double>& U)
{
    std::vector<std::vector<double>> ders(size_t(n) + 1, std::vector<double>(size_t(p) + 1, 0.0));
    std::vector<std::vector<double>> ndu(size_t(p) + 1, std::vector<double>(size_t(p) + 1, 0.0));
    std::vector<std::vector<double>> a(2, std::vector<double>(size_t(p) + 1, 0.0));
    std::vector<double> left(size_t(p) + 1, 0.0), right(size_t(p) + 1, 0.0);
    auto at = [](auto& m, int r, int c) -> double& { return m[size_t(r)][size_t(c)]; };
    at(ndu, 0, 0) = 1.0;
    for (int j = 1; j <= p; ++j)
    {
        left[size_t(j)] = u - U[size_t(i + 1 - j)];
        right[size_t(j)] = U[size_t(i + j)] - u;
        double saved = 0.0;
        for (int r = 0; r < j; ++r)
        {
            at(ndu, j, r) = right[size_t(r + 1)] + left[size_t(j - r)];
            double temp = at(ndu, r, j - 1) / at(ndu, j, r);
            at(ndu, r, j) = saved + right[size_t(r + 1)] * temp;
            saved = left[size_t(j - r)] * temp;
        }
        at(ndu, j, j) = saved;
    }
    for (int j = 0; j <= p; ++j)
        at(ders, 0, j) = at(ndu, j, p);
    for (int r = 0; r <= p; ++r)
    {
        int s1 = 0, s2 = 1;
        at(a, 0, 0) = 1.0;
        for (int k = 1; k <= n; ++k)
        {
            double d = 0.0;
            int rk = r - k, pk = p - k;
            if (r >= k)
            {
                at(a, s2, 0) = at(a, s1, 0) / at(ndu, pk + 1, rk);
                d = at(a, s2, 0) * at(ndu, rk, pk);
            }
            int j1 = rk >= -1 ? 1 : -rk;
            int j2 = r - 1 <= pk ? k - 1 : p - r;
            for (int j = j1; j <= j2; ++j)
            {
                at(a, s2, j) = (at(a, s1, j) - at(a, s1, j - 1)) / at(ndu, pk + 1, rk + j);
                d += at(a, s2, j) * at(ndu, rk + j, pk);
            }
            if (r <= pk)
            {
                at(a, s2, k) = -at(a, s1, k - 1) / at(ndu, pk + 1, r);
                d += at(a, s2, k) * at(ndu, r, pk);
            }
            at(ders, k, r) = d;
            std::swap(s1, s2);
        }
    }
    double r = p;
    for (int k = 1; k <= n; ++k)
    {
        for (int j = 0; j <= p; ++j)
            at(ders, k, j) *= r;
        r *= double(p - k);
    }
    return ders;
}

/** Matches a Bezier's start: anchor, its outgoing handle, and for G2 the next anchor's incoming handle. */
void matchBezierStart(Spline& s, const Geometry& want, int c, bool preserve)
{
    if (c >= 2 && s.size() == 2 && preserve)
        insertPointAt(s, 0.5);
    D3 Q0 = s.points[0], Q1 = s.handleR[0], Q2 = s.handleL[1], Q3 = s.points[1];
    double v = 3.0 * glm::distance(Q1, Q0);
    D3 tin = glm::length(Q1 - Q0) > kEps ? glm::normalize(Q1 - Q0) : glm::normalize(Q3 - Q0);
    if (v < kEps)
        v = glm::distance(Q3, Q0);
    double accel = glm::dot(6.0 * (Q2 - 2.0 * Q1 + Q0), tin);
    D3 Tin = -want.t;
    D3 N0 = want.p;
    D3 N1 = c >= 1 ? N0 + Tin * (v / 3.0) : Q1 + (N0 - Q0);
    D3 N2 = Q2;
    if (c >= 2)
        N2 = (v * v * want.k + accel * Tin) / 6.0 + 2.0 * N1 - N0;
    std::vector<std::pair<size_t, int>> edited;
    s.handleL[0] += N0 - Q0;
    s.points[0] = N0;
    s.handleR[0] = N1;
    s.typeL[0] = s.typeR[0] = HandleType::Free;
    if (c >= 2)
    {
        s.handleL[1] = N2;
        bool smooth = s.typeR[1] == HandleType::Aligned || s.typeR[1] == HandleType::Auto;
        if (s.size() > 2 && smooth)
        {
            // The far handle turns to stay opposite so the next anchor keeps its tangent continuity.
            s.typeL[1] = s.typeR[1] = HandleType::Aligned;
            edited.push_back({1, 0});
        }
        else
            s.typeL[1] = HandleType::Free;
    }
    updateHandles(s, edited);
}

/** Matches a NURBS start by solving for its first c + 1 control points (weights kept). */
void matchNurbsStart(Spline& s, const Geometry& want, int c, bool preserve)
{
    if (s.degree < std::max(c, 1))
    {
        std::vector<Piece> ps = pieces(s);
        for (Piece& p : ps)
            p = elevate(p, std::max(c, 1));
        s = fromPieces(ps, false, Kind::Nurbs);
    }
    const size_t need = preserve ? size_t(2 * (c + 1)) : size_t(c + 2);
    for (int guard = 0; s.size() < need && guard < 16; ++guard)
    {
        std::vector<double> U = knotVector(s);
        int spans = spanCount(s), best = 0;
        double widest = -1.0;
        for (int k = 0; k < spans; ++k)
            if (double w = knotParam(s, k + 1) - knotParam(s, k); w > widest)
            {
                widest = w;
                best = k;
            }
        insertKnot(s, best + 0.5, 1);
    }
    const int p = s.degree;
    std::vector<double> U = knotVector(s);
    const double u0 = U[size_t(p)];
    std::vector<std::vector<double>> N = basisDerivatives(p, u0, p, std::min(c, p), U);
    if (s.weights.size() != s.size())
        s.weights.clear();
    auto w = [&](size_t i) { return s.weight(i); };
    std::array<double, 3> wd{};
    std::array<D3, 3> Ad{};
    for (int k = 0; k <= std::min(2, p); ++k)
        for (int r = 0; r <= p; ++r)
        {
            wd[size_t(k)] += N.size() > size_t(k) ? N[size_t(k)][size_t(r)] * w(size_t(r)) : 0.0;
            Ad[size_t(k)] += N.size() > size_t(k) ? N[size_t(k)][size_t(r)] * w(size_t(r)) * s.points[size_t(r)] : D3(0.0);
        }
    D3 C0 = Ad[0] / wd[0];
    D3 C1 = (Ad[1] - wd[1] * C0) / wd[0];
    D3 C2 = (Ad[2] - 2.0 * wd[1] * C1 - wd[2] * C0) / wd[0];
    double v = glm::length(C1);
    D3 tin = v > kEps ? C1 / v : D3(0.0);
    if (v < kEps)
        v = std::max(length(s) / std::max(1, spanCount(s)), 1e-6);
    double accel = glm::dot(C2, tin);
    D3 Tin = -want.t;
    std::array<D3, 3> T{want.p, v * Tin, v * v * want.k + accel * Tin};
    std::array<D3, 3> At{wd[0] * T[0], wd[1] * T[0] + wd[0] * T[1], wd[2] * T[0] + 2.0 * wd[1] * T[1] + wd[0] * T[2]};
    for (int k = 0; k <= c; ++k)
    {
        D3 rest(0.0);
        for (int r = 0; r < k; ++r)
            rest += N[size_t(k)][size_t(r)] * w(size_t(r)) * s.points[size_t(r)];
        double piv = N[size_t(k)][size_t(k)] * w(size_t(k));
        if (std::abs(piv) < kEps)
            throw std::invalid_argument("the curve's end spans are degenerate");
        s.points[size_t(k)] = (At[size_t(k)] - rest) / piv;
    }
}
} // namespace

const char* continuityName(Continuity c)
{
    switch (c)
    {
    case Continuity::G0: return "g0";
    case Continuity::G1: return "g1";
    case Continuity::G2: return "g2";
    case Continuity::G3: return "g3";
    }
    return "g1";
}

bool parseContinuity(std::string_view name, Continuity* out)
{
    for (Continuity c : {Continuity::G0, Continuity::G1, Continuity::G2, Continuity::G3})
        if (name == continuityName(c))
        {
            *out = c;
            return true;
        }
    return false;
}

Geometry geometryAt(const Spline& s, double u, bool forward)
{
    std::vector<Piece> ps = pieces(s);
    if (ps.empty())
        return {};
    const int K = int(ps.size());
    u = std::clamp(u, 0.0, double(K));
    int span = std::clamp(int(std::floor(u)), 0, K - 1);
    // At a span boundary heading backward, the span behind is the one that leads away.
    if (!forward && u - span < 1e-12 && span > 0)
        --span;
    if (!forward && u - span < 1e-12 && span == 0 && s.closed)
    {
        span = K - 1;
        u = K;
    }
    double t = std::clamp(u - span, 0.0, 1.0);
    std::array<D3, 4> d = pieceDerivatives(ps[size_t(span)], t);
    if (glm::length(d[1]) < 1e-10 * std::max(1.0, glm::length(d[0])))
    {
        // A handle sitting on its anchor: step a hair into the span for the derivatives.
        double tt = t < 0.5 ? 1e-6 : 1.0 - 1e-6;
        std::array<D3, 4> e = pieceDerivatives(ps[size_t(span)], tt);
        d[1] = e[1];
        d[2] = e[2];
        d[3] = e[3];
    }
    Geometry g = arcLengthGeometry(d[0], d[1], d[2], d[3]);
    if (!forward)
    {
        g.t = -g.t;
        g.j = -g.j;
    }
    return g;
}

Geometry endGeometry(const Spline& s, bool atEnd) { return geometryAt(s, atEnd ? double(spanCount(s)) : 0.0, atEnd); }

std::optional<Geometry> borderGeometry(const geo::Mesh& m, Index e, double t)
{
    if (!m.edgeValid(e) || !m.isBoundaryEdge(e) || m.edgeCorner(e) == geo::kInvalid)
        return std::nullopt;
    Index f = m.cornerFace(m.edgeCorner(e));
    Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
    D3 pa = m.position(a), pb = m.position(b);
    t = std::clamp(t, 0.0, 1.0);
    D3 P = glm::mix(pa, pb, t), along = pb - pa;
    D3 d = glm::cross(along, m.faceNormal(f));
    if (glm::length(d) < kEps)
        return std::nullopt;
    d = glm::normalize(d);
    if (glm::dot(d, P - m.faceCentroid(f)) < 0.0)
        d = -d;
    // Curvature from the edge ring running into the mesh: a parabola through the point and the next two along it.
    auto ringCurvature = [&](Index v) {
        D3 pv = m.position(v);
        auto nextAlong = [&](Index from, const D3& dir, Index skip) {
            Index best = geo::kInvalid;
            double bestDot = 0.5;
            m.forEachPointEdge(from, [&](Index ed) {
                Index o = m.edgeOtherPoint(ed, from);
                if (o == skip || m.isBoundaryEdge(ed))
                    return;
                D3 step = m.position(o) - m.position(from);
                if (glm::length(step) < kEps)
                    return;
                double dd = glm::dot(glm::normalize(step), dir);
                if (dd > bestDot)
                {
                    bestDot = dd;
                    best = o;
                }
            });
            return best;
        };
        Index q1 = nextAlong(v, -d, geo::kInvalid);
        if (q1 == geo::kInvalid)
            return D3(0.0);
        D3 p1 = m.position(q1);
        Index q2 = nextAlong(q1, glm::normalize(p1 - pv), v);
        if (q2 == geo::kInvalid)
            return D3(0.0);
        D3 p2 = m.position(q2);
        double s1 = -glm::distance(pv, p1), s2 = s1 - glm::distance(p1, p2);
        if (std::abs(s1) < kEps || std::abs(s2 - s1) < kEps)
            return D3(0.0);
        D3 second = 2.0 * ((pv - p1) / (0.0 - s1) - (p1 - p2) / (s1 - s2)) / (0.0 - s2);
        return second - d * glm::dot(second, d);
    };
    D3 k = glm::mix(ringCurvature(a), ringCurvature(b), t);
    k -= d * glm::dot(k, d);
    return Geometry{P, d, k, D3(0.0)};
}

Spline blendCurve(const BlendEnd& A, const BlendEnd& B)
{
    LILA_ZONE();

    const D3 P0 = A.g.p, Pn = B.g.p;
    const double chord = glm::distance(P0, Pn);
    if (!(chord > 1e-9) || !std::isfinite(chord))
        throw std::invalid_argument("the blend's ends coincide");
    const int ca = int(A.continuity), cb = int(B.continuity);
    if (ca >= 1 && glm::length(A.g.t) < 0.5)
        throw std::invalid_argument("the first end has no tangent");
    if (cb >= 1 && glm::length(B.g.t) < 0.5)
        throw std::invalid_argument("the second end has no tangent");
    const int n = ca + cb + 1;
    const double La = std::max(A.bulge, 1e-3) * chord, Lb = std::max(B.bulge, 1e-3) * chord;
    const double f1 = n, f2 = double(n) * (n - 1), f3 = double(n) * (n - 1) * (n - 2);
    std::vector<D3> P(size_t(n) + 1, D3(0.0));
    P[0] = P0;
    P[size_t(n)] = Pn;
    // Bezier end derivatives n(P1 - P0), n(n-1)(P2 - 2P1 + P0), ... set to the ends' arc-length derivatives at speed L.
    if (ca >= 1)
        P[1] = P[0] + A.g.t * La / f1;
    if (ca >= 2)
        P[2] = 2.0 * P[1] - P[0] + A.g.k * (La * La) / f2;
    if (ca >= 3)
        P[3] = 3.0 * P[2] - 3.0 * P[1] + P[0] + A.g.j * (La * La * La) / f3;
    const size_t e = size_t(n);
    if (cb >= 1)
        P[e - 1] = P[e] + B.g.t * Lb / f1;
    if (cb >= 2)
        P[e - 2] = 2.0 * P[e - 1] - P[e] + B.g.k * (Lb * Lb) / f2;
    if (cb >= 3)
        P[e - 3] = P[e] - 3.0 * P[e - 1] + 3.0 * P[e - 2] + B.g.j * (Lb * Lb * Lb) / f3;
    for (const D3& q : P)
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z))
            throw std::invalid_argument("the blend's end geometry is not finite");
    Spline s;
    if (n == 1)
    {
        s.kind = Kind::Poly;
        s.points = P;
    }
    else if (n == 3)
    {
        s.kind = Kind::Bezier;
        s.points = {P[0], P[3]};
        s.handleL = {P[0], P[2]};
        s.handleR = {P[1], P[3]};
        s.typeL = s.typeR = {HandleType::Free, HandleType::Free};
    }
    else
    {
        s.kind = Kind::Nurbs;
        s.degree = n;
        s.points = P;
    }
    return s;
}

Spline matchEnd(const Spline& src, bool atEnd, const Geometry& target, const MatchOptions& o)
{
    LILA_ZONE();

    if (src.closed)
        throw std::invalid_argument("a closed curve has no end to match");
    if (!isValid(src) || spanCount(src) == 0 || length(src) < 1e-9)
        throw std::invalid_argument("the curve is degenerate");
    const int c = std::min(int(o.continuity), 2);
    Spline s = src;
    if (atEnd)
        reverse(s);
    // Working on the start: the curve's outward direction there is where it heads into the target.
    Geometry want = o.flip ? reversed(target) : target;
    if (c >= 1 && glm::length(target.t) < 0.5)
        throw std::invalid_argument("the target has no tangent there");
    if (s.kind == Kind::Poly)
    {
        if (c == 0)
            s.points.front() = want.p;
        else
        {
            Spline bez = fromPieces(pieces(s), false, Kind::Bezier);
            bez.tilt = s.tilt;
            bez.radius = s.radius;
            bez.cornerRadius = s.cornerRadius;
            bez.normalize();
            s = bez;
        }
    }
    if (s.kind == Kind::Bezier)
        matchBezierStart(s, want, c, o.preserveOther);
    else if (s.kind == Kind::Nurbs)
        matchNurbsStart(s, want, c, o.preserveOther);
    if (atEnd)
        reverse(s);
    return s;
}

Geometry reversed(const Geometry& g) { return {g.p, -g.t, g.k, -g.j}; }

Geometry facing(const Geometry& g, const glm::dvec3& dir) { return glm::dot(g.t, dir) >= 0.0 ? g : reversed(g); }

Geometry averageGeometry(const Geometry& a, const Geometry& b)
{
    Geometry g;
    g.p = (a.p + b.p) * 0.5;
    D3 t = a.t - b.t;
    g.t = glm::length(t) > kEps ? glm::normalize(t) : a.t;
    g.k = (a.k + b.k) * 0.5;
    g.k -= g.t * glm::dot(g.k, g.t);
    g.j = (a.j - b.j) * 0.5;
    return g;
}
} // namespace lila::curve
