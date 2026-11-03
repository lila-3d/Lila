#include "curve/Fair.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "curve/Tessellate.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lila::curve
{
namespace
{
double turn(const D3& a, const D3& b)
{
    if (glm::length(a) < kEps || glm::length(b) < kEps)
        return 0.0;
    return std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0));
}

/** How each control point may move: fixed, along a line (a kept end tangent), or anywhere within the tolerance ball. */
struct Constraint
{
    enum Kind : uint8_t
    {
        Free,
        Line,
        Fixed
    } kind = Free;
    D3 dir{0.0};
};

/** The control polygon fairing works on, and how it maps back to the spline. */
struct Polygon
{
    std::vector<D3> q;
    std::vector<Constraint> c;
    bool closed = false;
};

Polygon polygonOf(const Spline& s, const FairOptions& o)
{
    Polygon P;
    P.closed = s.closed;
    const size_t n = s.size();
    if (s.kind == Kind::Bezier)
    {
        for (size_t i = 0; i < n; ++i)
        {
            if (i > 0 || s.closed)
                P.q.push_back(s.handleL[i]);
            P.q.push_back(s.points[i]);
            if (i + 1 < n || s.closed)
                P.q.push_back(s.handleR[i]);
        }
        // Rotate so a closed polygon starts on the first anchor like an open one: [A0 R0 L1 A1 ... L0].
        if (s.closed)
            std::rotate(P.q.begin(), P.q.begin() + 1, P.q.end());
    }
    else
        P.q = s.points;
    P.c.assign(P.q.size(), {});
    const size_t m = P.q.size();
    if (!P.closed && m >= 2)
    {
        P.c.front().kind = P.c.back().kind = Constraint::Fixed;
        if (o.keepTangents && m >= 4)
        {
            for (auto [i, from] : {std::pair{size_t(1), size_t(0)}, std::pair{m - 2, m - 1}})
            {
                D3 d = P.q[i] - P.q[from];
                if (glm::length(d) > kEps)
                    P.c[i] = {Constraint::Line, glm::normalize(d)};
                else
                    P.c[i].kind = Constraint::Fixed;
            }
        }
    }
    if (!o.keepCorners)
        return P;
    auto fix = [&](size_t i) {
        if (i < m)
            P.c[i].kind = Constraint::Fixed;
    };
    if (s.kind == Kind::Bezier)
        for (size_t i = 0; i < n; ++i)
        {
            if (!s.closed && (i == 0 || i + 1 == n))
                continue;
            if (turn(s.points[i] - s.handleL[i], s.handleR[i] - s.points[i]) > o.cornerAngle)
            {
                size_t a = 3 * i;
                fix(a);
                fix(a + 1);
                fix(a == 0 ? m - 1 : a - 1);
            }
        }
    else if (s.kind == Kind::Poly)
        for (size_t i = 0; i < n; ++i)
        {
            if (!s.closed && (i == 0 || i + 1 == n))
                continue;
            if (turn(s.points[i] - s.points[(i + n - 1) % n], s.points[(i + 1) % n] - s.points[i]) > o.cornerAngle)
                fix(i);
        }
    else if (!s.closed && s.degree > 1)
    {
        // A knot at full multiplicity puts a control point on the curve; a kink there is a corner.
        std::vector<double> U = knotVector(s);
        const int p = s.degree;
        for (size_t i = size_t(p) + 1; i + size_t(p) < U.size();)
        {
            size_t j = i;
            while (j < U.size() && std::abs(U[j] - U[i]) < 1e-12)
                ++j;
            if (int(j - i) >= p && i >= 1 && i < m)
            {
                size_t on = i - 1;
                if (on >= 1 && on + 1 < m && turn(P.q[on] - P.q[on - 1], P.q[on + 1] - P.q[on]) > o.cornerAngle)
                {
                    fix(on - 1);
                    fix(on);
                    fix(on + 1);
                }
            }
            i = j;
        }
    }
    return P;
}

void writeBack(Spline& s, const Polygon& P, const FairOptions& o)
{
    if (s.kind != Kind::Bezier)
    {
        s.points = P.q;
        return;
    }
    std::vector<D3> q = P.q;
    if (s.closed)
        std::rotate(q.rbegin(), q.rbegin() + 1, q.rend());
    const size_t n = s.size();
    size_t k = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (i > 0 || s.closed)
            s.handleL[i] = q[k++];
        s.points[i] = q[k++];
        if (i + 1 < n || s.closed)
            s.handleR[i] = q[k++];
    }
    for (size_t i = 0; i < n; ++i)
    {
        bool end = !s.closed && (i == 0 || i + 1 == n);
        D3 l = s.points[i] - s.handleL[i], r = s.handleR[i] - s.points[i];
        bool corner = o.keepCorners && turn(l, r) > o.cornerAngle;
        if (end || corner || glm::length(l) < kEps || glm::length(r) < kEps)
        {
            s.typeL[i] = s.typeR[i] = HandleType::Free;
            continue;
        }
        // Smooth points come out exactly smooth: the handles move square onto their shared line.
        D3 dir = glm::normalize(s.handleR[i] - s.handleL[i]);
        s.handleL[i] = s.points[i] + dir * glm::dot(s.handleL[i] - s.points[i], dir);
        s.handleR[i] = s.points[i] + dir * glm::dot(s.handleR[i] - s.points[i], dir);
        s.typeL[i] = s.typeR[i] = HandleType::Aligned;
    }
}

/** One Gauss-Seidel sweep on the difference energy; returns the largest move. */
double sweep(Polygon& P, const std::vector<D3>& orig, double radius, int order)
{
    static const double kCoef[4][4] = {{1, -1, 0, 0}, {1, -2, 1, 0}, {1, -3, 3, -1}, {0, 0, 0, 0}};
    const double* coef = kCoef[order - 1];
    const int len = order + 1;
    const int m = int(P.q.size());
    const int terms = P.closed ? m : m - order;
    double moved = 0.0;
    for (int i = 0; i < m; ++i)
    {
        if (P.c[size_t(i)].kind == Constraint::Fixed)
            continue;
        D3 num(0.0);
        double den = 0.0;
        for (int r = 0; r < len; ++r)
        {
            int j = i - r;
            if (P.closed)
                j = (j % m + m) % m;
            else if (j < 0 || j >= terms)
                continue;
            D3 rest(0.0);
            for (int k = 0; k < len; ++k)
                if (k != r)
                    rest += coef[k] * P.q[size_t((j + k) % m)];
            num -= coef[r] * rest;
            den += coef[r] * coef[r];
        }
        if (den < kEps)
            continue;
        D3 x = num / den;
        const D3& o = orig[size_t(i)];
        if (P.c[size_t(i)].kind == Constraint::Line)
            x = o + P.c[size_t(i)].dir * glm::dot(x - o, P.c[size_t(i)].dir);
        // Every control point stays within the tolerance, so by the convex hull property the curve does too.
        D3 off = x - o;
        double d = glm::length(off);
        if (d > radius)
            x = o + off * (radius / d);
        moved = std::max(moved, glm::distance(x, P.q[size_t(i)]));
        P.q[size_t(i)] = x;
    }
    return moved;
}

double hausdorff(const Polyline& a, const Polyline& b)
{
    auto oneWay = [](const Polyline& x, const Polyline& y) {
        double worst = 0.0;
        size_t segs = y.closed ? y.size() : (y.size() ? y.size() - 1 : 0);
        for (const D3& p : x.points)
        {
            double best = std::numeric_limits<double>::max();
            for (size_t i = 0; i < segs; ++i)
            {
                const D3& u = y.points[i];
                D3 d = y.points[(i + 1) % y.size()] - u;
                double l2 = glm::dot(d, d);
                double t = l2 > kEps ? std::clamp(glm::dot(p - u, d) / l2, 0.0, 1.0) : 0.0;
                best = std::min(best, glm::distance(u + d * t, p));
            }
            if (segs == 0 && y.size())
                best = glm::distance(p, y.points[0]);
            worst = std::max(worst, best);
        }
        return worst;
    };
    return std::max(oneWay(a, b), oneWay(b, a));
}
} // namespace

double curvatureVariation(const Spline& s, int samples)
{
    const int K = spanCount(s);
    if (K == 0 || samples < 2)
        return 0.0;
    double total = 0.0, prev = 0.0;
    for (int i = 0; i <= samples; ++i)
    {
        double k = curvature(evaluate(s, double(K) * i / samples));
        if (i > 0)
            total += std::abs(k - prev);
        prev = k;
    }
    return total;
}

FairResult fair(const Spline& s, const FairOptions& o)
{
    LILA_ZONE();

    FairResult r;
    r.curve = s;
    if (!isValid(s) || spanCount(s) == 0)
        return r;
    r.variationBefore = r.variationAfter = curvatureVariation(s);
    Polygon P = polygonOf(s, o);
    const int m = int(P.q.size());
    const int order = std::min(3, P.closed ? m - 1 : m - 1);
    if (order < 2 || o.tolerance <= 0.0)
        return r;
    const std::vector<D3> orig = P.q;
    D3 lo, hi;
    bounds(s, lo, hi);
    const double still = std::max(glm::distance(lo, hi), 1.0) * 1e-9;
    for (int it = 0; it < std::max(1, o.iterations); ++it)
    {
        r.iterations = it + 1;
        if (sweep(P, orig, o.tolerance, order) < still)
            break;
    }
    Spline out = s;
    writeBack(out, P, o);
    out.knots = s.knots;
    out.weights = s.weights;
    if (!isValid(out))
        return r;
    TessOptions to;
    to.chordError = std::max(o.tolerance * 0.01, 1e-5);
    to.maxAngle = 0.03;
    r.maxDeviation = hausdorff(tessellate(out, to), tessellate(s, to));
    r.curve = std::move(out);
    r.variationAfter = curvatureVariation(r.curve);
    return r;
}
} // namespace lila::curve
