#include "curve/Fit.h"

#include "core/Profile.h"
#include "curve/CurveDeform.h"
#include "curve/Internal.h"
#include "curve/Tessellate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>

namespace lila::curve
{
namespace
{
using D3 = glm::dvec3;
using D4 = glm::dvec4;

constexpr double kEps = 1e-12;

D3 unit(const D3& v)
{
    double l = glm::length(v);
    return l > kEps ? v / l : D3(0.0);
}

Piece cubic(const D3& a, const D3& b, const D3& c, const D3& d)
{
    Piece p;
    p.cv = {D4(a, 1.0), D4(b, 1.0), D4(c, 1.0), D4(d, 1.0)};
    return p;
}

D3 bez(const D3* q, double t)
{
    double u = 1.0 - t;
    return q[0] * (u * u * u) + q[1] * (3 * u * u * t) + q[2] * (3 * u * t * t) + q[3] * (t * t * t);
}

D3 bezD1(const D3* q, double t)
{
    double u = 1.0 - t;
    return (q[1] - q[0]) * (3 * u * u) + (q[2] - q[1]) * (6 * u * t) + (q[3] - q[2]) * (3 * t * t);
}

D3 bezD2(const D3* q, double t)
{
    return (q[2] - q[1] * 2.0 + q[0]) * (6 * (1 - t)) + (q[3] - q[2] * 2.0 + q[1]) * (6 * t);
}

// Schneider, "An Algorithm for Automatically Fitting Digitized Curves" (Graphics Gems, 1990).
struct Schneider
{
    std::span<const D3> d;
    double error;
    std::vector<Piece>& out;

    std::vector<double> chord(size_t first, size_t last) const
    {
        std::vector<double> u(last - first + 1, 0.0);
        for (size_t i = first + 1; i <= last; ++i)
            u[i - first] = u[i - first - 1] + glm::distance(d[i], d[i - 1]);
        double total = u.back();
        for (double& x : u)
            x = total > kEps ? x / total : 0.0;
        return u;
    }

    std::array<D3, 4> generate(size_t first, size_t last, const std::vector<double>& u, const D3& t1, const D3& t2) const
    {
        double C[2][2] = {{0, 0}, {0, 0}}, X[2] = {0, 0};
        for (size_t i = 0; i < u.size(); ++i)
        {
            double t = u[i], s = 1.0 - t;
            double b0 = s * s * s, b1 = 3 * t * s * s, b2 = 3 * t * t * s, b3 = t * t * t;
            D3 a1 = t1 * b1, a2 = t2 * b2;
            C[0][0] += glm::dot(a1, a1);
            C[0][1] += glm::dot(a1, a2);
            C[1][1] += glm::dot(a2, a2);
            D3 tmp = d[first + i] - (d[first] * (b0 + b1) + d[last] * (b2 + b3));
            X[0] += glm::dot(a1, tmp);
            X[1] += glm::dot(a2, tmp);
        }
        C[1][0] = C[0][1];
        double det = C[0][0] * C[1][1] - C[1][0] * C[0][1];
        double aL = 0.0, aR = 0.0;
        if (std::abs(det) > 1e-18)
        {
            aL = (X[0] * C[1][1] - X[1] * C[0][1]) / det;
            aR = (C[0][0] * X[1] - C[1][0] * X[0]) / det;
        }
        double segLen = glm::distance(d[first], d[last]);
        double eps = 1e-6 * segLen;
        if (aL < eps || aR < eps)
            aL = aR = segLen / 3.0;
        return {d[first], d[first] + t1 * aL, d[last] + t2 * aR, d[last]};
    }

    double maxError(size_t first, size_t last, const std::array<D3, 4>& q, const std::vector<double>& u, size_t* split) const
    {
        double best = 0.0;
        *split = (first + last) / 2;
        for (size_t i = first + 1; i < last; ++i)
        {
            double dd = glm::distance(bez(q.data(), u[i - first]), d[i]);
            if (dd > best)
            {
                best = dd;
                *split = i;
            }
        }
        return best;
    }

    void reparameterize(size_t first, const std::array<D3, 4>& q, std::vector<double>& u) const
    {
        for (size_t i = 0; i < u.size(); ++i)
        {
            double t = u[i];
            D3 r = bez(q.data(), t) - d[first + i];
            D3 d1 = bezD1(q.data(), t), d2 = bezD2(q.data(), t);
            double den = glm::dot(d1, d1) + glm::dot(r, d2);
            if (std::abs(den) > kEps)
                u[i] = std::clamp(t - glm::dot(r, d1) / den, 0.0, 1.0);
        }
    }

    void fit(size_t first, size_t last, D3 t1, D3 t2, int depth = 0)
    {
        if (last - first == 1)
        {
            double dist = glm::distance(d[first], d[last]) / 3.0;
            out.push_back(cubic(d[first], d[first] + t1 * dist, d[last] + t2 * dist, d[last]));
            return;
        }
        std::vector<double> u = chord(first, last);
        std::array<D3, 4> q = generate(first, last, u, t1, t2);
        size_t split = 0;
        double err = maxError(first, last, q, u, &split);
        if (err < error || depth > 40)
        {
            out.push_back(cubic(q[0], q[1], q[2], q[3]));
            return;
        }
        if (err < error * 4.0)
            for (int it = 0; it < 6; ++it)
            {
                reparameterize(first, q, u);
                q = generate(first, last, u, t1, t2);
                err = maxError(first, last, q, u, &split);
                if (err < error)
                {
                    out.push_back(cubic(q[0], q[1], q[2], q[3]));
                    return;
                }
            }
        split = std::clamp(split, first + 1, last - 1);
        D3 center = unit(d[split - 1] - d[split + 1]);
        if (glm::length(center) < 0.5)
            center = unit(d[split - 1] - d[split]);
        fit(first, split, t1, center, depth + 1);
        fit(split, last, -center, t2, depth + 1);
    }
};

std::vector<D3> dedupe(std::span<const D3> pts, bool closed)
{
    std::vector<D3> out;
    for (const D3& p : pts)
        if (out.empty() || glm::distance(out.back(), p) > 1e-9)
            out.push_back(p);
    if (closed)
        while (out.size() > 2 && glm::distance(out.back(), out.front()) <= 1e-9)
            out.pop_back();
    return out;
}

/** Indices of corners: the direction over a few points either side turns more than angle. */
std::vector<size_t> findCorners(const std::vector<D3>& p, bool closed, double angle)
{
    std::vector<size_t> out;
    size_t n = p.size();
    if (n < 3)
        return out;
    double total = 0.0;
    for (size_t i = 1; i < n; ++i)
        total += glm::distance(p[i], p[i - 1]);
    // Look a little way along the stroke so jitter between neighbouring samples doesn't read as a corner.
    double reach = std::max(total / double(n) * 2.5, 1e-9);
    auto walk = [&](size_t i, int dir) -> std::optional<D3> {
        double acc = 0.0;
        size_t j = i;
        for (size_t step = 0; step < n; ++step)
        {
            if (!closed && ((dir < 0 && j == 0) || (dir > 0 && j + 1 >= n)))
                break;
            size_t k = (j + n + size_t(dir + int(n))) % n;
            acc += glm::distance(p[j], p[k]);
            j = k;
            if (acc >= reach)
                break;
        }
        if (j == i)
            return std::nullopt;
        return p[j];
    };
    std::vector<double> turn(n, 0.0);
    for (size_t i = 0; i < n; ++i)
    {
        if (!closed && (i == 0 || i + 1 == n))
            continue;
        auto a = walk(i, -1), b = walk(i, 1);
        if (!a || !b)
            continue;
        D3 u = unit(p[i] - *a), v = unit(*b - p[i]);
        turn[i] = std::atan2(glm::length(glm::cross(u, v)), glm::dot(u, v));
    }
    for (size_t i = 0; i < n; ++i)
        if (turn[i] > angle && turn[i] >= turn[(i + n - 1) % n] && turn[i] >= turn[(i + 1) % n])
        {
            if (!out.empty() && i - out.back() < 2)
                continue;
            out.push_back(i);
        }
    return out;
}

/** Schneider over runs between the given corner indices. */
std::vector<Piece> fitRuns(const std::vector<D3>& p, bool closed, double tol, std::vector<size_t> corners)
{
    std::vector<Piece> out;
    size_t n = p.size();
    if (n < 2)
        return out;
    std::vector<D3> work = p;
    if (closed)
    {
        // Start at a corner when there is one, so the seam isn't smoothed over twice.
        if (!corners.empty())
        {
            size_t c0 = corners.front();
            std::rotate(work.begin(), work.begin() + ptrdiff_t(c0), work.end());
            for (size_t& c : corners)
                c = (c + n - c0) % n;
        }
        work.push_back(work.front());
    }
    std::vector<size_t> cuts;
    cuts.push_back(0);
    for (size_t c : corners)
        if (c > 0 && c + 1 < work.size())
            cuts.push_back(c);
    cuts.push_back(work.size() - 1);
    bool smoothSeam = closed && corners.empty();
    Schneider s{work, tol, out};
    for (size_t k = 0; k + 1 < cuts.size(); ++k)
    {
        size_t a = cuts[k], b = cuts[k + 1];
        if (b <= a)
            continue;
        D3 t1 = unit(work[std::min(a + 1, b)] - work[a]);
        D3 t2 = unit(work[b > 0 ? std::max(b - 1, a) : 0] - work[b]);
        if (smoothSeam)
        {
            D3 seam = unit(work[1] - work[work.size() - 2]);
            if (a == 0)
                t1 = seam;
            if (b == work.size() - 1)
                t2 = -seam;
        }
        s.fit(a, b, t1, t2);
    }
    return out;
}

void dersBasisFuns(int i, double u, int p, int nd, const std::vector<double>& U, std::vector<std::vector<double>>& ders)
{
    std::vector<std::vector<double>> ndu(size_t(p) + 1, std::vector<double>(size_t(p) + 1, 0.0));
    std::vector<double> left(size_t(p) + 1), right(size_t(p) + 1);
    ndu[0][0] = 1.0;
    for (int j = 1; j <= p; ++j)
    {
        left[size_t(j)] = u - U[size_t(i + 1 - j)];
        right[size_t(j)] = U[size_t(i + j)] - u;
        double saved = 0.0;
        for (int r = 0; r < j; ++r)
        {
            ndu[size_t(j)][size_t(r)] = right[size_t(r) + 1] + left[size_t(j - r)];
            double temp = ndu[size_t(r)][size_t(j) - 1] / ndu[size_t(j)][size_t(r)];
            ndu[size_t(r)][size_t(j)] = saved + right[size_t(r) + 1] * temp;
            saved = left[size_t(j - r)] * temp;
        }
        ndu[size_t(j)][size_t(j)] = saved;
    }
    ders.assign(size_t(nd) + 1, std::vector<double>(size_t(p) + 1, 0.0));
    for (int j = 0; j <= p; ++j)
        ders[0][size_t(j)] = ndu[size_t(j)][size_t(p)];
    std::vector<std::vector<double>> a(2, std::vector<double>(size_t(p) + 1, 0.0));
    for (int r = 0; r <= p; ++r)
    {
        int s1 = 0, s2 = 1;
        a[0][0] = 1.0;
        for (int k = 1; k <= nd; ++k)
        {
            double dd = 0.0;
            int rk = r - k, pk = p - k;
            if (r >= k)
            {
                a[size_t(s2)][0] = a[size_t(s1)][0] / ndu[size_t(pk) + 1][size_t(rk)];
                dd = a[size_t(s2)][0] * ndu[size_t(rk)][size_t(pk)];
            }
            int j1 = rk >= -1 ? 1 : -rk;
            int j2 = r - 1 <= pk ? k - 1 : p - r;
            for (int j = j1; j <= j2; ++j)
            {
                a[size_t(s2)][size_t(j)] = (a[size_t(s1)][size_t(j)] - a[size_t(s1)][size_t(j) - 1]) / ndu[size_t(pk) + 1][size_t(rk + j)];
                dd += a[size_t(s2)][size_t(j)] * ndu[size_t(rk + j)][size_t(pk)];
            }
            if (r <= pk)
            {
                a[size_t(s2)][size_t(k)] = -a[size_t(s1)][size_t(k) - 1] / ndu[size_t(pk) + 1][size_t(r)];
                dd += a[size_t(s2)][size_t(k)] * ndu[size_t(r)][size_t(pk)];
            }
            ders[size_t(k)][size_t(r)] = dd;
            std::swap(s1, s2);
        }
    }
    double r = p;
    for (int k = 1; k <= nd; ++k)
    {
        for (int j = 0; j <= p; ++j)
            ders[size_t(k)][size_t(j)] *= r;
        r *= p - k;
    }
}

Spline fromSolution(const std::vector<D4>& P, const std::vector<double>& knots, int degree, bool closed)
{
    Spline s;
    s.kind = Kind::Nurbs;
    s.degree = degree;
    s.closed = closed;
    s.knots = knots;
    for (const D4& q : P)
        s.points.push_back(D3(q));
    return s;
}
} // namespace

Spline fitBezier(std::span<const glm::dvec3> pts, bool closed, double tolerance, double cornerAngle)
{
    LILA_ZONE();

    std::vector<D3> p = dedupe(pts, closed);
    Spline out;
    out.kind = Kind::Bezier;
    out.closed = closed && p.size() > 2;
    if (p.size() < 2)
    {
        out.points = p;
        out.normalize();
        return out;
    }
    std::vector<Piece> ps = fitRuns(p, out.closed, std::max(tolerance, 1e-6), findCorners(p, out.closed, cornerAngle));
    out = fromPieces(ps, out.closed, Kind::Bezier);
    return out;
}

std::vector<Piece> approximateCubic(const Piece& p, double tolerance)
{
    constexpr int kSamples = 48;
    std::vector<D3> pts;
    for (int i = 0; i <= kSamples; ++i)
        pts.push_back(evaluate(p, double(i) / kSamples).p);
    std::vector<Piece> out;
    Schneider s{pts, std::max(tolerance, 1e-6), out};
    s.fit(0, pts.size() - 1, endTangent(p, false), -endTangent(p, true));
    for (Piece& q : out)
        q.span = p.span;
    return out;
}

Spline interpolate(std::span<const glm::dvec3> input, int degree, bool closed)
{
    LILA_ZONE();

    std::vector<D3> Q = dedupe(input, closed);
    size_t m = Q.size();
    Spline out;
    out.kind = Kind::Nurbs;
    out.closed = closed;
    if (m < 2 || (closed && m < 3))
    {
        out.degree = 1;
        out.points = Q;
        out.closed = false;
        return out;
    }
    int p = std::clamp(degree, 1, 7);
    std::vector<double> ub = detail::chordParams(Q, closed, 0.5);
    if (closed)
    {
        p = std::min(p, int(m) - 1);
        p = std::max(p, 1);
        // Periodic: the points sit on the breakpoints, so they are the edit points.
        int n = int(m);
        std::vector<double> b(size_t(n) + 1);
        for (int i = 0; i <= n; ++i)
            b[size_t(i)] = ub[size_t(i)] * n;
        Spline probe;
        probe.kind = Kind::Nurbs;
        probe.degree = p;
        probe.closed = true;
        probe.points.assign(size_t(n), D3(0.0));
        probe.knots = b;
        if (!isValid(probe))
        {
            out.degree = 1;
            out.points = Q;
            return out;
        }
        std::vector<double> U = knotVector(probe);
        std::vector<double> A(size_t(n) * size_t(n), 0.0);
        std::vector<D4> rhs(static_cast<size_t>(n));
        std::vector<double> N(size_t(p) + 1);
        for (int r = 0; r < n; ++r)
        {
            double x = b[size_t(r)];
            int i = detail::findSpan(U, x, p, n + p);
            detail::basisFuns(i, x, p, U, N.data());
            for (int k = 0; k <= p; ++k)
                A[size_t(r) * size_t(n) + size_t((i - p + k) % n)] += N[size_t(k)];
            rhs[size_t(r)] = D4(Q[size_t(r)], 1.0);
        }
        if (!detail::solveDense(A, n, rhs))
        {
            out.degree = 1;
            out.points = Q;
            return out;
        }
        out = fromSolution(rhs, b, p, true);
        return out;
    }
    p = std::min(p, int(m) - 1);
    int nIntervals = int(m) - 1;
    if (p == 1)
    {
        out.degree = 1;
        out.points = Q;
        out.knots = {0.0};
        for (double u : ub)
            out.knots.push_back(u * nIntervals);
        out.knots.push_back(double(nIntervals));
        return out;
    }
    // Knots at the data parameters, so every point is an edit point; p - 1 end conditions close the system.
    std::vector<double> U(size_t(p) + 1, 0.0);
    for (int k = 1; k < nIntervals; ++k)
        U.push_back(ub[size_t(k)]);
    U.insert(U.end(), size_t(p) + 1, 1.0);
    int nCv = int(U.size()) - p - 1;
    std::vector<double> A(size_t(nCv) * size_t(nCv), 0.0);
    std::vector<D4> rhs(size_t(nCv), D4(0.0));
    int row = 0;
    std::vector<double> N(size_t(p) + 1);
    for (size_t k = 0; k < m; ++k)
    {
        double x = ub[k];
        int i = detail::findSpan(U, x, p, nCv);
        detail::basisFuns(i, x, p, U, N.data());
        for (int j = 0; j <= p; ++j)
            A[size_t(row) * size_t(nCv) + size_t(i - p + j)] = N[size_t(j)];
        rhs[size_t(row)] = D4(Q[k], 0.0);
        ++row;
    }
    // Quadratic: start tangent of the parabola through the first three points. Higher: derivatives 2, 2, 3, 3, ... vanish
    // at alternate ends (natural ends for a cubic).
    std::vector<std::vector<double>> ders;
    if (p == 2)
    {
        double t0 = ub[0], t1 = ub[1], t2 = m > 2 ? ub[2] : 1.0;
        D3 d0 = m > 2 ? Q[0] * ((2 * t0 - t1 - t2) / ((t0 - t1) * (t0 - t2))) + Q[1] * ((t0 - t2) / ((t1 - t0) * (t1 - t2))) +
                            Q[2] * ((t0 - t1) / ((t2 - t0) * (t2 - t1)))
                      : (Q[1] - Q[0]) / std::max(t1 - t0, kEps);
        int i = detail::findSpan(U, 0.0, p, nCv);
        dersBasisFuns(i, 0.0, p, 1, U, ders);
        for (int j = 0; j <= p; ++j)
            A[size_t(row) * size_t(nCv) + size_t(i - p + j)] = ders[1][size_t(j)];
        rhs[size_t(row)] = D4(d0, 0.0);
        ++row;
    }
    else
        for (int c = 0; row < nCv; ++c)
        {
            int order = 2 + c / 2;
            double x = c % 2 == 0 ? 0.0 : 1.0;
            int i = detail::findSpan(U, x, p, nCv);
            dersBasisFuns(i, x, p, order, U, ders);
            for (int j = 0; j <= p; ++j)
                A[size_t(row) * size_t(nCv) + size_t(i - p + j)] = ders[size_t(order)][size_t(j)];
            rhs[size_t(row)] = D4(0.0);
            ++row;
        }
    if (!detail::solveDense(A, nCv, rhs))
    {
        out.degree = 1;
        out.points = Q;
        return out;
    }
    for (double& u : U)
        u *= nIntervals;
    return fromSolution(rhs, U, p, false);
}

Spline fitNurbs(std::span<const glm::dvec3> input, int degree, int spans, bool closed)
{
    LILA_ZONE();

    std::vector<D3> Q = dedupe(input, closed);
    size_t m = Q.size();
    int p = std::clamp(degree, 1, 7);
    spans = std::max(1, spans);
    Spline out;
    out.kind = Kind::Nurbs;
    out.closed = closed;
    out.degree = p;
    if (m < 2)
    {
        out.points = Q;
        return out;
    }
    std::vector<double> ub = detail::chordParams(Q, closed, 1.0);
    if (closed)
    {
        int n = std::max({spans, p, 3});
        out.points.assign(size_t(n), D3(0.0));
        std::vector<double> U = knotVector(out);
        std::vector<double> A(size_t(n) * size_t(n), 0.0);
        std::vector<D4> rhs(size_t(n), D4(0.0));
        std::vector<double> N(size_t(p) + 1);
        for (size_t k = 0; k < m; ++k)
        {
            double x = ub[k] * n;
            int i = detail::findSpan(U, x, p, n + p);
            detail::basisFuns(i, x, p, U, N.data());
            for (int a = 0; a <= p; ++a)
            {
                int ia = (i - p + a) % n;
                rhs[size_t(ia)] += D4(Q[k], 0.0) * N[size_t(a)];
                for (int b = 0; b <= p; ++b)
                    A[size_t(ia) * size_t(n) + size_t((i - p + b) % n)] += N[size_t(a)] * N[size_t(b)];
            }
        }
        for (int i = 0; i < n; ++i)
            A[size_t(i) * size_t(n) + size_t(i)] += 1e-9;
        if (!detail::solveDense(A, n, rhs))
            return interpolate(Q, p, true);
        for (int i = 0; i < n; ++i)
            out.points[size_t(i)] = D3(rhs[size_t(i)]);
        return out;
    }
    int nCv = spans + p;
    if (size_t(nCv) > m)
        return interpolate(Q, p, false);
    out.points.assign(size_t(nCv), D3(0.0));
    std::vector<double> U = knotVector(out);
    double range = U.back();
    out.points.front() = Q.front();
    out.points.back() = Q.back();
    int inner = nCv - 2;
    if (inner <= 0)
        return out;
    std::vector<double> A(size_t(inner) * size_t(inner), 0.0);
    std::vector<D4> rhs(size_t(inner), D4(0.0));
    std::vector<double> N(size_t(p) + 1);
    for (size_t k = 1; k + 1 < m; ++k)
    {
        double x = ub[k] * range;
        int i = detail::findSpan(U, x, p, nCv);
        detail::basisFuns(i, x, p, U, N.data());
        D3 r = Q[k];
        for (int a = 0; a <= p; ++a)
        {
            int ia = i - p + a;
            if (ia == 0)
                r -= Q.front() * N[size_t(a)];
            else if (ia == nCv - 1)
                r -= Q.back() * N[size_t(a)];
        }
        for (int a = 0; a <= p; ++a)
        {
            int ia = i - p + a;
            if (ia <= 0 || ia >= nCv - 1)
                continue;
            rhs[size_t(ia - 1)] += D4(r, 0.0) * N[size_t(a)];
            for (int b = 0; b <= p; ++b)
            {
                int ib = i - p + b;
                if (ib <= 0 || ib >= nCv - 1)
                    continue;
                A[size_t(ia - 1) * size_t(inner) + size_t(ib - 1)] += N[size_t(a)] * N[size_t(b)];
            }
        }
    }
    for (int i = 0; i < inner; ++i)
        A[size_t(i) * size_t(inner) + size_t(i)] += 1e-9;
    if (!detail::solveDense(A, inner, rhs))
        return interpolate(Q, p, false);
    for (int i = 0; i < inner; ++i)
        out.points[size_t(i) + 1] = D3(rhs[size_t(i)]);
    return out;
}

namespace
{
/** Points evenly spaced by arc length along a polyline segment range [a, b] of indices (b may wrap past the end). */
std::vector<D3> resampleRun(const Polyline& pl, double s0, double s1, int count)
{
    std::vector<D3> out;
    for (int i = 0; i <= count; ++i)
        out.push_back(pl.pointAtLength(s0 + (s1 - s0) * double(i) / count));
    return out;
}
} // namespace

Spline rebuild(const Spline& s, const RebuildOptions& o)
{
    LILA_ZONE();

    if (o.keepPointCount)
        return evenSpacing(s, o.keepCorners);

    TessOptions to;
    to.chordError = 1e-3;
    to.maxAngle = 0.03;
    Polyline pl = tessellate(s, to);
    if (pl.size() < 2)
        return s;
    // Runs between kinks, by arc length.
    std::vector<double> cuts;
    if (o.keepCorners)
        for (size_t i = 0; i < pl.size(); ++i)
            if (pl.kink[i])
                cuts.push_back(pl.arcLength[i]);
    bool closed = s.closed;
    std::vector<std::pair<double, double>> runs;
    if (cuts.empty())
        runs.push_back({0.0, pl.length});
    else if (closed)
        for (size_t k = 0; k < cuts.size(); ++k)
            runs.push_back({cuts[k], k + 1 < cuts.size() ? cuts[k + 1] : cuts[0] + pl.length});
    else
    {
        double prev = 0.0;
        for (double c : cuts)
            if (c > prev + 1e-9)
            {
                runs.push_back({prev, c});
                prev = c;
            }
        runs.push_back({prev, pl.length});
    }
    int spans = std::max(1, o.spans);
    bool wholeLoop = closed && runs.size() == 1 && cuts.empty();
    std::vector<Piece> all;
    Spline result;
    for (const auto& [a, b] : runs)
    {
        int n = runs.size() == 1 ? spans : std::max(1, int(std::round(spans * (b - a) / std::max(pl.length, 1e-12))));
        int samples = std::max(64, n * 16);
        std::vector<D3> pts = resampleRun(pl, a, b, samples);
        if (wholeLoop)
            pts.pop_back();
        Spline r;
        if (o.kind == Kind::Poly)
        {
            r.kind = Kind::Poly;
            r.closed = wholeLoop;
            for (int i = 0; i <= n; ++i)
                r.points.push_back(pl.pointAtLength(a + (b - a) * double(i) / n));
            if (wholeLoop)
                r.points.pop_back();
        }
        else
            r = fitNurbs(pts, o.kind == Kind::Bezier ? 3 : o.degree, n, wholeLoop);
        if (wholeLoop || (runs.size() == 1 && !closed))
        {
            // A single run keeps its own knot vector, so its spans stay uniform and smooth inside.
            result = r;
            break;
        }
        for (Piece& p : pieces(r))
            all.push_back(std::move(p));
    }
    if (!wholeLoop && (runs.size() > 1 || closed))
        result = fromPieces(all, closed, o.kind == Kind::Poly ? Kind::Poly : Kind::Nurbs);
    if (o.kind == Kind::Bezier)
        result = toBezier(result, o.tolerance);
    result.tilt.clear();
    result.radius.clear();
    result.cornerRadius.clear();
    return result;
}

std::vector<size_t> simplifyIndices(std::span<const glm::dvec3> pts, double tolerance, bool closed)
{
    size_t n = pts.size();
    if (n <= 2)
    {
        std::vector<size_t> all(n);
        for (size_t i = 0; i < n; ++i)
            all[i] = i;
        return all;
    }
    std::vector<uint8_t> keep(n, 0);
    std::function<void(size_t, size_t)> rdp = [&](size_t a, size_t b) {
        if (b <= a + 1)
            return;
        D3 d = pts[b % n] - pts[a];
        double l = glm::length(d);
        double best = -1.0;
        size_t idx = a;
        for (size_t i = a + 1; i < b; ++i)
        {
            D3 q = pts[i] - pts[a];
            double dist = l > kEps ? glm::length(q - d * std::clamp(glm::dot(q, d) / (l * l), 0.0, 1.0)) : glm::length(q);
            if (dist > best)
            {
                best = dist;
                idx = i;
            }
        }
        if (best > tolerance)
        {
            keep[idx] = 1;
            rdp(a, idx);
            rdp(idx, b);
        }
    };
    keep[0] = 1;
    if (closed)
    {
        // Split the loop at the point farthest from the start.
        size_t far = 0;
        double fd = 0.0;
        for (size_t i = 1; i < n; ++i)
            if (double d = glm::distance(pts[i], pts[0]); d > fd)
            {
                fd = d;
                far = i;
            }
        keep[far] = 1;
        rdp(0, far);
        rdp(far, n);
    }
    else
    {
        keep[n - 1] = 1;
        rdp(0, n - 1);
    }
    std::vector<size_t> out;
    for (size_t i = 0; i < n; ++i)
        if (keep[i])
            out.push_back(i);
    return out;
}

Spline simplify(const Spline& s, double tolerance)
{
    LILA_ZONE();

    if (s.kind == Kind::Poly)
    {
        Spline out = s;
        out.points.clear();
        out.tilt.clear();
        out.radius.clear();
        out.cornerRadius.clear();
        for (size_t i : simplifyIndices(s.points, tolerance, s.closed))
        {
            out.points.push_back(s.points[i]);
            if (s.tilt.size() == s.size())
                out.tilt.push_back(s.tilt[i]);
            if (s.radius.size() == s.size())
                out.radius.push_back(s.radius[i]);
            if (s.cornerRadius.size() == s.size())
                out.cornerRadius.push_back(s.cornerRadius[i]);
        }
        return out;
    }
    TessOptions to;
    to.chordError = std::max(tolerance * 0.1, 1e-4);
    Polyline pl = tessellate(s, to);
    std::vector<size_t> corners;
    for (size_t i = 0; i < pl.size(); ++i)
        if (pl.kink[i])
            corners.push_back(i);
    Spline out;
    out.kind = Kind::Bezier;
    out.closed = s.closed;
    std::vector<Piece> ps = fitRuns(pl.points, s.closed, std::max(tolerance, 1e-6), corners);
    out = fromPieces(ps, s.closed, Kind::Bezier);
    return s.kind == Kind::Nurbs ? toNurbs(out) : out;
}

Spline catmullRom(std::span<const glm::dvec3> input, bool closed, double alpha)
{
    std::vector<D3> P = dedupe(input, closed);
    size_t n = P.size();
    Spline out;
    out.kind = Kind::Bezier;
    out.closed = closed && n > 2;
    if (n < 2)
    {
        out.points = P;
        out.normalize();
        return out;
    }
    closed = out.closed;
    auto at = [&](long i) -> D3 {
        long nn = long(n);
        if (closed)
            return P[size_t(((i % nn) + nn) % nn)];
        // Open ends extrapolate a phantom point, so the end spans don't bend toward nothing.
        if (i < 0)
            return P[0] * 2.0 - P[1];
        if (i >= nn)
            return P[n - 1] * 2.0 - P[n - 2];
        return P[size_t(i)];
    };
    size_t segs = closed ? n : n - 1;
    std::vector<Piece> ps;
    for (size_t s = 0; s < segs; ++s)
    {
        long i = long(s);
        D3 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        auto dt = [&](const D3& a, const D3& b) { return std::max(std::pow(glm::distance(a, b), alpha), 1e-9); };
        double d0 = dt(p0, p1), d1 = dt(p1, p2), d2 = dt(p2, p3);
        D3 m1 = (p1 - p0) / d0 - (p2 - p0) / (d0 + d1) + (p2 - p1) / d1;
        D3 m2 = (p2 - p1) / d1 - (p3 - p1) / (d1 + d2) + (p3 - p2) / d2;
        m1 *= d1;
        m2 *= d1;
        ps.push_back(cubic(p1, p1 + m1 / 3.0, p2 - m2 / 3.0, p2));
    }
    return fromPieces(ps, closed, Kind::Bezier);
}

Spline kappaCurve(std::span<const glm::dvec3> input, bool closed, int iterations)
{
    LILA_ZONE();

    std::vector<D3> c = dedupe(input, closed);
    size_t m = c.size();
    if (m < 3 || (closed && m < 3))
        return catmullRom(c, closed);
    // Pieces: one per point on a loop, one per interior point on an open curve (whose ends are fixed anchors).
    size_t k = closed ? m : m - 2;
    auto user = [&](size_t i) { return closed ? c[i] : c[i + 1]; };
    std::vector<D3> mid(k), start(k), end(k);
    std::vector<double> lambda(k, 0.5), t(k, 0.5);
    for (size_t i = 0; i < k; ++i)
        mid[i] = user(i);
    auto area = [](const D3& a, const D3& b, const D3& d) { return 0.5 * glm::length(glm::cross(b - a, d - a)); };
    auto ends = [&] {
        for (size_t i = 0; i < k; ++i)
        {
            if (closed || i + 1 < k)
            {
                size_t j = (i + 1) % k;
                end[i] = mid[i] * (1.0 - lambda[i]) + mid[j] * lambda[i];
                start[j] = end[i];
            }
        }
        if (!closed)
        {
            start[0] = c.front();
            end[k - 1] = c.back();
        }
    };
    for (int it = 0; it < std::max(1, iterations); ++it)
    {
        ends();
        for (size_t i = 0; i < k; ++i)
        {
            if (!closed && i + 1 == k)
                continue;
            size_t j = (i + 1) % k;
            double a = std::sqrt(area(start[i], mid[i], mid[j])), b = std::sqrt(area(mid[i], mid[j], end[j]));
            lambda[i] = a + b > kEps ? a / (a + b) : 0.5;
        }
        ends();
        // Parameter where the piece's curvature peaks at its user point: the root in [0, 1] of Yan et al.'s cubic.
        for (size_t i = 0; i < k; ++i)
        {
            D3 p0 = start[i], p2 = end[i], ci = user(i);
            auto f = [&](double x) {
                return glm::dot(p2 - p0, p2 - p0) * x * x * x + 3.0 * glm::dot(p2 - p0, p0 - ci) * x * x +
                       glm::dot(p0 * 3.0 - ci * 2.0 - p2, p0 - ci) * x - glm::dot(p0 - ci, p0 - ci);
            };
            double lo = 0.0, hi = 1.0;
            if (f(lo) > 0.0 || f(hi) < 0.0)
            {
                t[i] = 0.5;
                continue;
            }
            for (int b = 0; b < 60; ++b)
            {
                double x = (lo + hi) * 0.5;
                (f(x) < 0.0 ? lo : hi) = x;
            }
            t[i] = std::clamp((lo + hi) * 0.5, 0.02, 0.98);
        }
        // c_i = (1-t)^2 start + 2(1-t)t mid + t^2 end, with start/end blends of the neighbouring mids: linear in the mids.
        std::vector<double> A(k * k, 0.0);
        std::vector<D4> rhs(k, D4(0.0));
        for (size_t i = 0; i < k; ++i)
        {
            double ti = t[i], a0 = (1 - ti) * (1 - ti), a1 = 2 * (1 - ti) * ti, a2 = ti * ti;
            D3 r = user(i);
            A[i * k + i] += a1;
            if (closed || i > 0)
            {
                size_t h = (i + k - 1) % k;
                A[i * k + h] += a0 * (1.0 - lambda[h]);
                A[i * k + i] += a0 * lambda[h];
            }
            else
                r -= c.front() * a0;
            if (closed || i + 1 < k)
            {
                size_t j = (i + 1) % k;
                A[i * k + i] += a2 * (1.0 - lambda[i]);
                A[i * k + j] += a2 * lambda[i];
            }
            else
                r -= c.back() * a2;
            rhs[i] = D4(r, 0.0);
        }
        if (!detail::solveDense(A, int(k), rhs))
            break;
        for (size_t i = 0; i < k; ++i)
            mid[i] = D3(rhs[i]);
    }
    ends();
    std::vector<Piece> ps;
    for (size_t i = 0; i < k; ++i)
    {
        Piece q;
        q.cv = {D4(start[i], 1.0), D4(mid[i], 1.0), D4(end[i], 1.0)};
        ps.push_back(elevate(q, 3));
    }
    return fromPieces(ps, closed, Kind::Bezier);
}
} // namespace lila::curve
