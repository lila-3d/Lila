#include "curve/Spline.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "curve/Fit.h"
#include "curve/Internal.h"
#include "curve/Tessellate.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace lila::curve
{
namespace
{
D4 homo(const D3& p, double w) { return D4(p * w, w); }

template <class T> void erase(std::vector<T>& v, size_t i)
{
    if (i < v.size())
        v.erase(v.begin() + ptrdiff_t(i));
}

template <class T> void insertCopy(std::vector<T>& v, size_t i, size_t n)
{
    if (v.size() != n || n == 0)
        return;
    T value = v[std::min(i, n - 1)];
    v.insert(v.begin() + ptrdiff_t(i), value);
}

/** Bernstein-form value of the homogeneous control points. */
D4 casteljau(std::vector<D4> q, double t)
{
    for (size_t r = 1; r < q.size(); ++r)
        for (size_t k = 0; k + r < q.size(); ++k)
            q[k] = q[k] * (1.0 - t) + q[k + 1] * t;
    return q.empty() ? D4(0.0) : q[0];
}

/** Interval indices of the non-empty knot spans, in order. */
std::vector<int> nurbsSpans(const Spline& s, const std::vector<double>& U)
{
    std::vector<int> out;
    int p = s.degree;
    int m = int(U.size()) - p - 1; // control points (unrolled)
    for (int i = p; i < m; ++i)
        if (U[size_t(i) + 1] - U[size_t(i)] > kEps)
            out.push_back(i);
    return out;
}

/** Blossom of the span on interval i at (a x (p - j), b x j): the span's j-th Bezier control point. */
D4 blossom(const std::vector<D4>& P, const std::vector<double>& U, int p, int i, int j)
{
    std::array<D4, 8> d;
    for (int k = 0; k <= p; ++k)
        d[size_t(k)] = P[size_t(i - p + k)];
    double a = U[size_t(i)], b = U[size_t(i) + 1];
    for (int r = 1; r <= p; ++r)
    {
        double u = r <= p - j ? a : b;
        for (int k = p; k >= r; --k)
        {
            double lo = U[size_t(i - p + k)], hi = U[size_t(i + 1 + k - r)];
            double alpha = hi - lo > kEps ? (u - lo) / (hi - lo) : 0.0;
            d[size_t(k)] = d[size_t(k) - 1] * (1.0 - alpha) + d[size_t(k)] * alpha;
        }
    }
    return d[size_t(p)];
}

/** Rescales a rational piece so both end weights are 1 (same curve, reparameterised). */
Piece standardForm(Piece p)
{
    double w0 = p.cv.front().w, wn = p.cv.back().w;
    int n = p.degree();
    if (n < 1 || w0 <= 0.0 || wn <= 0.0)
        return p;
    double c = std::pow(w0 / wn, 1.0 / n);
    for (int i = 0; i <= n; ++i)
        p.cv[size_t(i)] *= std::pow(c, i) / w0;
    return p;
}

int openKnotCount(const Spline& s) { return int(s.size()) + s.degree + 1; }
} // namespace

const char* kindName(Kind k)
{
    switch (k)
    {
    case Kind::Poly: return "poly";
    case Kind::Bezier: return "bezier";
    case Kind::Nurbs: return "nurbs";
    }
    return "poly";
}

bool parseKind(std::string_view name, Kind* out)
{
    for (Kind k : {Kind::Poly, Kind::Bezier, Kind::Nurbs})
        if (name == kindName(k))
        {
            *out = k;
            return true;
        }
    return false;
}

const char* handleTypeName(HandleType t)
{
    switch (t)
    {
    case HandleType::Auto: return "auto";
    case HandleType::Vector: return "vector";
    case HandleType::Aligned: return "aligned";
    case HandleType::Free: return "free";
    }
    return "auto";
}

bool parseHandleType(std::string_view name, HandleType* out)
{
    for (HandleType t : {HandleType::Auto, HandleType::Vector, HandleType::Aligned, HandleType::Free})
        if (name == handleTypeName(t))
        {
            *out = t;
            return true;
        }
    return false;
}

bool Spline::rational() const
{
    if (kind != Kind::Nurbs)
        return false;
    return std::ranges::any_of(weights, [](double w) { return std::abs(w - 1.0) > 1e-12; });
}

bool Piece::rational() const
{
    return std::ranges::any_of(cv, [&](const D4& q) { return std::abs(q.w - cv.front().w) > 1e-12; });
}

void Spline::normalize()
{
    size_t n = points.size();
    auto fit = [n](auto& v) {
        if (!v.empty() && v.size() != n)
            v.clear();
    };
    fit(weights);
    fit(tilt);
    fit(radius);
    fit(cornerRadius);
    if (kind != Kind::Bezier)
    {
        handleL.clear();
        handleR.clear();
        typeL.clear();
        typeR.clear();
    }
    else if (handleL.size() != n || handleR.size() != n || typeL.size() != n || typeR.size() != n)
    {
        handleL.assign(n, D3(0.0));
        handleR.assign(n, D3(0.0));
        typeL.assign(n, HandleType::Vector);
        typeR.assign(n, HandleType::Vector);
        updateHandles(*this);
    }
    if (kind != Kind::Nurbs)
    {
        weights.clear();
        knots.clear();
    }
    else
    {
        degree = std::clamp(degree, 1, 7);
        size_t want = closed ? n + 1 : size_t(openKnotCount(*this));
        if (!knots.empty() && knots.size() != want)
            knots.clear();
    }
}

void Spline::erasePoint(size_t i)
{
    if (i >= points.size())
        return;
    size_t n = points.size();
    erase(points, i);
    erase(handleL, i);
    erase(handleR, i);
    erase(typeL, i);
    erase(typeR, i);
    if (weights.size() == n)
        erase(weights, i);
    if (tilt.size() == n)
        erase(tilt, i);
    if (radius.size() == n)
        erase(radius, i);
    if (cornerRadius.size() == n)
        erase(cornerRadius, i);
    knots.clear();
}

void Spline::insertPoint(size_t i, const glm::dvec3& p)
{
    size_t n = points.size();
    i = std::min(i, n);
    insertCopy(handleL, i, n);
    insertCopy(handleR, i, n);
    insertCopy(typeL, i, n);
    insertCopy(typeR, i, n);
    insertCopy(weights, i, n);
    insertCopy(tilt, i, n);
    insertCopy(radius, i, n);
    insertCopy(cornerRadius, i, n);
    points.insert(points.begin() + ptrdiff_t(i), p);
    if (kind == Kind::Bezier && handleL.size() == points.size())
    {
        handleL[i] = handleR[i] = p;
        typeL[i] = typeR[i] = HandleType::Auto;
        updateHandles(*this);
    }
    knots.clear();
}

void validate(const Spline& s)
{
    size_t n = s.size();
    auto fail = [](const std::string& why) { throw std::invalid_argument(why); };
    if (s.kind == Kind::Nurbs)
    {
        if (s.degree < 1 || s.degree > 7)
            fail("degree must be 1 to 7");
        size_t need = s.closed ? size_t(std::max(3, s.degree)) : size_t(s.degree) + 1;
        if (n < need)
            fail("a degree " + std::to_string(s.degree) + (s.closed ? " closed" : "") + " curve needs at least " + std::to_string(need) + " points");
        if (!s.weights.empty() && s.weights.size() != n)
            fail("weights need one value per point");
        for (double w : s.weights)
            if (!(w > 0.0))
                fail("weights must be positive");
        if (!s.knots.empty())
        {
            size_t want = s.closed ? n + 1 : size_t(openKnotCount(s));
            if (s.knots.size() != want)
                fail("knots: expected " + std::to_string(want) + " values, got " + std::to_string(s.knots.size()));
            for (size_t k = 1; k < s.knots.size(); ++k)
                if (s.knots[k] < s.knots[k - 1])
                    fail("knots must not decrease");
            if (s.knots.back() - s.knots.front() <= kEps)
                fail("knots span no range");
        }
    }
    else
    {
        size_t need = s.kind == Kind::Poly && s.closed ? 3 : 2;
        if (n < need)
            fail("a curve needs at least " + std::to_string(need) + " points");
    }
    if (s.kind == Kind::Bezier && (s.handleL.size() != n || s.handleR.size() != n))
        fail("Bezier handles need one per point");
}

bool isValid(const Spline& s, std::string* why)
{
    try
    {
        validate(s);
        return true;
    }
    catch (const std::exception& e)
    {
        if (why)
            *why = e.what();
        return false;
    }
}

int spanCount(const Spline& s)
{
    if (!isValid(s))
        return 0;
    int n = int(s.size());
    if (s.kind != Kind::Nurbs)
        return s.closed ? n : n - 1;
    std::vector<double> U = knotVector(s);
    return int(nurbsSpans(s, U).size());
}

std::vector<double> knotVector(const Spline& s)
{
    int n = int(s.size()), p = s.degree;
    std::vector<double> U;
    if (!s.closed)
    {
        if (int(s.knots.size()) == n + p + 1)
            return s.knots;
        for (int i = 0; i <= p; ++i)
            U.push_back(0.0);
        for (int i = 1; i < n - p; ++i)
            U.push_back(double(i));
        for (int i = 0; i <= p; ++i)
            U.push_back(double(std::max(1, n - p)));
        return U;
    }
    std::vector<double> b = s.knots;
    if (int(b.size()) != n + 1)
    {
        b.resize(size_t(n) + 1);
        for (int i = 0; i <= n; ++i)
            b[size_t(i)] = double(i);
    }
    double T = b[size_t(n)] - b[0];
    U.assign(size_t(n + 2 * p + 1), 0.0);
    for (int i = 0; i <= n; ++i)
        U[size_t(p + i)] = b[size_t(i)];
    for (int k = 1; k <= p; ++k)
    {
        U[size_t(p - k)] = b[size_t(n - k)] - T;
        U[size_t(p + n + k)] = b[size_t(k)] + T;
    }
    return U;
}

std::vector<glm::dvec4> controlPoints(const Spline& s)
{
    std::vector<D4> P;
    size_t n = s.size();
    P.reserve(n + size_t(s.degree));
    for (size_t i = 0; i < n; ++i)
        P.push_back(homo(s.points[i], s.weight(i)));
    if (s.kind == Kind::Nurbs && s.closed)
        for (int k = 0; k < s.degree; ++k)
            P.push_back(P[size_t(k) % n]);
    return P;
}

Piece piece(const Spline& s, int span)
{
    Piece pc;
    pc.span = span;
    int n = int(s.size());
    if (s.kind == Kind::Poly)
    {
        pc.cv = {homo(s.points[size_t(span)], 1.0), homo(s.points[size_t((span + 1) % n)], 1.0)};
        return pc;
    }
    if (s.kind == Kind::Bezier)
    {
        size_t a = size_t(span), b = size_t((span + 1) % n);
        pc.cv = {homo(s.points[a], 1.0), homo(s.handleR[a], 1.0), homo(s.handleL[b], 1.0), homo(s.points[b], 1.0)};
        return pc;
    }
    std::vector<double> U = knotVector(s);
    std::vector<D4> P = controlPoints(s);
    std::vector<int> spans = nurbsSpans(s, U);
    int i = spans[size_t(std::clamp(span, 0, int(spans.size()) - 1))];
    for (int j = 0; j <= s.degree; ++j)
        pc.cv.push_back(blossom(P, U, s.degree, i, j));
    return pc;
}

std::vector<Piece> pieces(const Spline& s)
{
    LILA_ZONE();

    std::vector<Piece> out;
    if (!isValid(s))
        return out;
    if (s.kind != Kind::Nurbs)
    {
        int count = spanCount(s);
        for (int k = 0; k < count; ++k)
            out.push_back(piece(s, k));
        return out;
    }
    std::vector<double> U = knotVector(s);
    std::vector<D4> P = controlPoints(s);
    std::vector<int> spans = nurbsSpans(s, U);
    for (size_t k = 0; k < spans.size(); ++k)
    {
        Piece pc;
        pc.span = int(k);
        for (int j = 0; j <= s.degree; ++j)
            pc.cv.push_back(blossom(P, U, s.degree, spans[k], j));
        out.push_back(std::move(pc));
    }
    return out;
}

Sample evaluate(const Piece& pc, double t)
{
    int n = pc.degree();
    Sample out;
    if (n < 0)
        return out;
    D4 A = casteljau(pc.cv, t), A1(0.0), A2(0.0);
    if (n >= 1)
    {
        std::vector<D4> d1(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            d1[size_t(i)] = (pc.cv[size_t(i) + 1] - pc.cv[size_t(i)]) * double(n);
        A1 = casteljau(d1, t);
        if (n >= 2)
        {
            std::vector<D4> d2(size_t(n) - 1);
            for (int i = 0; i + 1 < n; ++i)
                d2[size_t(i)] = (d1[size_t(i) + 1] - d1[size_t(i)]) * double(n - 1);
            A2 = casteljau(d2, t);
        }
    }
    double w = A.w;
    if (std::abs(w) < kEps)
        return out;
    out.p = D3(A) / w;
    out.d1 = (D3(A1) - A1.w * out.p) / w;
    out.d2 = (D3(A2) - 2.0 * A1.w * out.d1 - A2.w * out.p) / w;
    return out;
}

Sample evaluate(const Spline& s, double u)
{
    int count = spanCount(s);
    if (count == 0)
        return {};
    int span = std::clamp(int(std::floor(u)), 0, count - 1);
    return evaluate(piece(s, span), std::clamp(u - span, 0.0, 1.0));
}

glm::dvec3 pointAt(const Spline& s, double u) { return evaluate(s, u).p; }

glm::dvec3 endTangent(const Piece& p, bool end)
{
    int n = p.degree();
    D3 a = end ? p.end() : p.start();
    for (int k = 1; k <= n; ++k)
    {
        D3 q = p.point(size_t(end ? n - k : k));
        D3 d = end ? a - q : q - a;
        if (glm::dot(d, d) > 1e-20)
            return glm::normalize(d);
    }
    return D3(0.0);
}

void split(const Piece& p, double t, Piece& left, Piece& right)
{
    size_t n = p.cv.size();
    left.cv.resize(n);
    right.cv.resize(n);
    left.span = right.span = p.span;
    std::vector<D4> q = p.cv;
    for (size_t r = 0; r < n; ++r)
    {
        left.cv[r] = q[0];
        right.cv[n - 1 - r] = q[n - 1 - r];
        for (size_t k = 0; k + r + 1 < n; ++k)
            q[k] = q[k] * (1.0 - t) + q[k + 1] * t;
    }
}

Piece subPiece(const Piece& p, double t0, double t1)
{
    Piece a, b;
    if (t1 < 1.0 - 1e-15)
    {
        split(p, t1, a, b);
    }
    else
        a = p;
    if (t0 <= 1e-15)
        return a;
    double tt = t1 > kEps ? t0 / t1 : 0.0;
    Piece c, d;
    split(a, tt, c, d);
    return d;
}

Piece elevate(const Piece& p, int degree)
{
    Piece q = p;
    while (q.degree() < degree)
    {
        int n = q.degree();
        std::vector<D4> cv(size_t(n) + 2);
        cv.front() = q.cv.front();
        cv.back() = q.cv.back();
        for (int i = 1; i <= n; ++i)
        {
            double a = double(i) / double(n + 1);
            cv[size_t(i)] = q.cv[size_t(i) - 1] * a + q.cv[size_t(i)] * (1.0 - a);
        }
        q.cv = std::move(cv);
    }
    return q;
}

bool isStraight(const Piece& p, double tolerance)
{
    D3 a = p.start(), b = p.end();
    D3 d = b - a;
    double len = glm::length(d);
    double tol = tolerance * std::max(1.0, len);
    if (len < kEps)
    {
        for (size_t i = 0; i < p.cv.size(); ++i)
            if (glm::length(p.point(i) - a) > tol)
                return false;
        return true;
    }
    d /= len;
    for (size_t i = 1; i + 1 < p.cv.size(); ++i)
    {
        D3 q = p.point(i) - a;
        double along = glm::dot(q, d);
        if (glm::length(q - d * along) > tol || along < -tol || along > len + tol)
            return false;
    }
    return true;
}

double curvature(const Sample& s)
{
    double l = glm::length(s.d1);
    return l > kEps ? glm::length(glm::cross(s.d1, s.d2)) / (l * l * l) : 0.0;
}

double knotParam(const Spline& s, double u)
{
    if (s.kind != Kind::Nurbs)
        return u;
    std::vector<double> U = knotVector(s);
    std::vector<int> spans = nurbsSpans(s, U);
    if (spans.empty())
        return 0.0;
    int k = std::clamp(int(std::floor(u)), 0, int(spans.size()) - 1);
    double t = u - k;
    size_t i = size_t(spans[size_t(k)]);
    return U[i] + t * (U[i + 1] - U[i]);
}

double spanParam(const Spline& s, double knot)
{
    if (s.kind != Kind::Nurbs)
        return knot;
    std::vector<double> U = knotVector(s);
    std::vector<int> spans = nurbsSpans(s, U);
    if (spans.empty())
        return 0.0;
    for (size_t k = 0; k < spans.size(); ++k)
    {
        size_t i = size_t(spans[k]);
        if (knot < U[i + 1] || k + 1 == spans.size())
            return double(k) + std::clamp((knot - U[i]) / (U[i + 1] - U[i]), 0.0, 1.0);
    }
    return 0.0;
}

std::vector<glm::dvec3> editPoints(const Spline& s)
{
    std::vector<D3> out;
    std::vector<Piece> ps = pieces(s);
    for (const Piece& p : ps)
        out.push_back(p.start());
    if (!ps.empty() && !s.closed)
        out.push_back(ps.back().end());
    return out;
}

namespace
{
/** Boehm insertion on an open NURBS, knot-vector parameter. */
void insertOpen(Spline& s, double u, int times)
{
    int p = s.degree;
    std::vector<double> U = knotVector(s);
    std::vector<D4> P = controlPoints(s);
    int n = int(P.size());
    for (int r = 0; r < times; ++r)
    {
        int k = detail::findSpan(U, u, p, n);
        int mult = int(std::count_if(U.begin(), U.end(), [&](double x) { return std::abs(x - u) < 1e-12; }));
        if (mult >= p)
            break;
        std::vector<D4> Q(size_t(n) + 1);
        for (int i = 0; i <= k - p; ++i)
            Q[size_t(i)] = P[size_t(i)];
        for (int i = k; i < n; ++i)
            Q[size_t(i) + 1] = P[size_t(i)];
        for (int i = k - p + 1; i <= k; ++i)
        {
            double a = (u - U[size_t(i)]) / (U[size_t(i + p)] - U[size_t(i)]);
            Q[size_t(i)] = P[size_t(i) - 1] * (1.0 - a) + P[size_t(i)] * a;
        }
        U.insert(U.begin() + k + 1, u);
        P = std::move(Q);
        ++n;
    }
    s.points.resize(P.size());
    s.weights.resize(P.size());
    for (size_t i = 0; i < P.size(); ++i)
    {
        s.weights[i] = P[i].w;
        s.points[i] = D3(P[i]) / P[i].w;
    }
    s.knots = U;
    if (!s.rational())
        s.weights.clear();
    s.tilt.clear();
    s.radius.clear();
    s.cornerRadius.clear();
}

/** Insertion into a periodic NURBS: solves for the refined control points, which reproduce the curve exactly. */
void insertClosed(Spline& s, double u, int times)
{
    int p = s.degree, n = int(s.size());
    std::vector<double> b = s.knots;
    if (int(b.size()) != n + 1)
    {
        b.resize(size_t(n) + 1);
        for (int i = 0; i <= n; ++i)
            b[size_t(i)] = double(i);
    }
    double T = b.back() - b.front();
    double v = b.front() + std::fmod(u - b.front() + T * 4.0, T);
    // A breakpoint can't be raised past the degree.
    int have = int(std::count_if(b.begin(), b.end() - 1, [&](double x) { return std::abs(x - v) < 1e-12; }));
    times = std::min(times, p - have);
    if (times <= 0)
        return;
    std::vector<double> nb = b;
    for (int r = 0; r < times; ++r)
        nb.insert(std::upper_bound(nb.begin(), nb.end() - 1, v), v);
    Spline fine = s;
    fine.knots = nb;
    int n2 = n + times;
    fine.points.assign(size_t(n2), D3(0.0));
    fine.weights.assign(size_t(n2), 1.0);
    std::vector<double> U2 = knotVector(fine);
    std::vector<double> U1 = knotVector(s);
    std::vector<D4> P1 = controlPoints(s);
    // Sample at n2 well spread parameters (Greville abscissae) and solve the collocation system in homogeneous space.
    std::vector<double> A(size_t(n2) * size_t(n2), 0.0);
    std::vector<D4> rhs(static_cast<size_t>(n2));
    std::vector<double> N(size_t(p) + 1);
    for (int r = 0; r < n2; ++r)
    {
        double g = 0.0;
        for (int k = 1; k <= p; ++k)
            g += U2[size_t(r + k)];
        g = p > 0 ? g / p : U2[size_t(r)];
        double x = U2[size_t(p)] + std::fmod(g - U2[size_t(p)] + T * 4.0, T);
        x = std::clamp(x, U2[size_t(p)], U2[size_t(p + n2)] - 1e-12 * T);
        int i2 = detail::findSpan(U2, x, p, n2 + p);
        detail::basisFuns(i2, x, p, U2, N.data());
        for (int k = 0; k <= p; ++k)
            A[size_t(r) * size_t(n2) + size_t((i2 - p + k) % n2)] += N[size_t(k)];
        int i1 = detail::findSpan(U1, x, p, n + p);
        detail::basisFuns(i1, x, p, U1, N.data());
        D4 val(0.0);
        for (int k = 0; k <= p; ++k)
            val += P1[size_t(i1 - p + k)] * N[size_t(k)];
        rhs[size_t(r)] = val;
    }
    if (!detail::solveDense(A, n2, rhs))
        return;
    s.knots = nb;
    s.points.resize(size_t(n2));
    s.weights.resize(size_t(n2));
    for (int i = 0; i < n2; ++i)
    {
        s.weights[size_t(i)] = rhs[size_t(i)].w;
        s.points[size_t(i)] = D3(rhs[size_t(i)]) / rhs[size_t(i)].w;
    }
    if (!s.rational())
        s.weights.clear();
    s.tilt.clear();
    s.radius.clear();
    s.cornerRadius.clear();
}
} // namespace

void insertKnot(Spline& s, double u, int times)
{
    if (s.kind != Kind::Nurbs)
        s = toNurbs(s);
    if (!isValid(s) || times < 1)
        return;
    double k = knotParam(s, u);
    if (s.closed)
        insertClosed(s, k, times);
    else
        insertOpen(s, k, times);
}

Spline toNurbs(const Spline& s)
{
    if (s.kind == Kind::Nurbs)
        return s;
    Spline out;
    out.kind = Kind::Nurbs;
    out.closed = s.closed;
    size_t n = s.size();
    if (s.kind == Kind::Poly)
    {
        out.degree = 1;
        out.points = s.points;
        out.tilt = s.tilt;
        out.radius = s.radius;
        out.cornerRadius = s.cornerRadius;
        return out;
    }
    out.degree = 3;
    if (n == 0)
        return out;
    auto perPoint = [&](const std::vector<double>& v, std::vector<double>& dst) {
        if (v.size() != n)
            return;
        for (size_t i = 0; i < n; ++i)
        {
            bool last = i + 1 == n && !s.closed;
            dst.push_back(v[i]);
            if (!last)
            {
                double next = v[(i + 1) % n];
                dst.push_back(v[i] + (next - v[i]) / 3.0);
                dst.push_back(v[i] + (next - v[i]) * 2.0 / 3.0);
            }
        }
    };
    for (size_t i = 0; i < n; ++i)
    {
        out.points.push_back(s.points[i]);
        if (i + 1 < n || s.closed)
        {
            out.points.push_back(s.handleR[i]);
            out.points.push_back(s.handleL[(i + 1) % n]);
        }
    }
    perPoint(s.tilt, out.tilt);
    perPoint(s.radius, out.radius);
    perPoint(s.cornerRadius, out.cornerRadius);
    size_t m = out.points.size();
    if (s.closed)
    {
        out.knots.resize(m + 1);
        for (size_t i = 0; i <= m; ++i)
            out.knots[i] = std::ceil(double(i) / 3.0);
    }
    else
    {
        out.knots = {0, 0, 0, 0};
        for (size_t i = 1; i + 1 < n; ++i)
            out.knots.insert(out.knots.end(), {double(i), double(i), double(i)});
        out.knots.insert(out.knots.end(), 4, double(n - 1));
    }
    return out;
}

Spline fromPieces(const std::vector<Piece>& input, bool closed, Kind preferred)
{
    Spline out;
    out.closed = closed;
    if (input.empty())
        return out;
    std::vector<Piece> ps;
    ps.reserve(input.size());
    int D = 1;
    bool rat = false;
    bool allStraight = true;
    for (const Piece& p : input)
    {
        Piece q = standardForm(p);
        D = std::max(D, q.degree());
        rat = rat || q.rational();
        allStraight = allStraight && isStraight(q);
        ps.push_back(std::move(q));
    }
    size_t k = ps.size();
    if (preferred == Kind::Poly && allStraight)
    {
        out.kind = Kind::Poly;
        for (const Piece& p : ps)
            out.points.push_back(p.start());
        if (!closed)
            out.points.push_back(ps.back().end());
        return out;
    }
    if (preferred != Kind::Nurbs && !rat && D <= 3)
    {
        out.kind = Kind::Bezier;
        for (Piece& p : ps)
            p = elevate(p, 3);
        size_t n = closed ? k : k + 1;
        out.points.resize(n);
        out.handleL.resize(n);
        out.handleR.resize(n);
        out.typeL.assign(n, HandleType::Free);
        out.typeR.assign(n, HandleType::Free);
        for (size_t i = 0; i < k; ++i)
        {
            out.points[i] = ps[i].start();
            out.handleR[i] = ps[i].point(1);
            out.handleL[(i + 1) % n] = ps[i].point(2);
        }
        if (!closed)
        {
            out.points[k] = ps[k - 1].end();
            out.handleL[0] = out.points[0] - (out.handleR[0] - out.points[0]);
            out.handleR[k] = out.points[k] - (out.handleL[k] - out.points[k]);
        }
        for (size_t i = 0; i < n; ++i)
        {
            D3 a = out.points[i] - out.handleL[i], b = out.handleR[i] - out.points[i];
            double la = glm::length(a), lb = glm::length(b);
            bool aligned = la < 1e-9 || lb < 1e-9 || glm::dot(a / la, b / lb) > 1.0 - 1e-9;
            if (aligned)
                out.typeL[i] = out.typeR[i] = HandleType::Aligned;
        }
        return out;
    }
    out.kind = Kind::Nurbs;
    out.degree = D;
    for (Piece& p : ps)
        p = elevate(p, D);
    auto push = [&](const D4& q) {
        out.points.push_back(D3(q) / q.w);
        out.weights.push_back(q.w);
    };
    if (closed)
    {
        for (const Piece& p : ps)
            for (int j = 0; j < D; ++j)
                push(p.cv[size_t(j)]);
        size_t m = out.points.size();
        out.knots.resize(m + 1);
        for (size_t i = 0; i <= m; ++i)
            out.knots[i] = std::ceil(double(i) / D);
    }
    else
    {
        push(ps.front().cv.front());
        for (const Piece& p : ps)
            for (int j = 1; j <= D; ++j)
                push(p.cv[size_t(j)]);
        out.knots.assign(size_t(D) + 1, 0.0);
        for (size_t i = 1; i < k; ++i)
            out.knots.insert(out.knots.end(), size_t(D), double(i));
        out.knots.insert(out.knots.end(), size_t(D) + 1, double(k));
    }
    if (!rat)
        out.weights.clear();
    return out;
}

Spline toBezier(const Spline& s, double tolerance)
{
    if (s.kind == Kind::Bezier)
        return s;
    Spline out;
    if (s.kind == Kind::Poly)
    {
        out.kind = Kind::Bezier;
        out.closed = s.closed;
        out.points = s.points;
        out.tilt = s.tilt;
        out.radius = s.radius;
        out.cornerRadius = s.cornerRadius;
        size_t n = s.size();
        out.handleL.assign(n, D3(0.0));
        out.handleR.assign(n, D3(0.0));
        out.typeL.assign(n, HandleType::Vector);
        out.typeR.assign(n, HandleType::Vector);
        updateHandles(out);
        return out;
    }
    std::vector<Piece> ps;
    for (const Piece& p : pieces(s))
    {
        if (!p.rational() && p.degree() <= 3)
        {
            ps.push_back(p);
            continue;
        }
        for (Piece& q : approximateCubic(p, tolerance))
            ps.push_back(std::move(q));
    }
    out = fromPieces(ps, s.closed, Kind::Bezier);
    return out;
}

Spline convert(const Spline& s, Kind kind, double tolerance)
{
    if (s.kind == kind)
        return s;
    switch (kind)
    {
    case Kind::Nurbs: return toNurbs(s);
    case Kind::Bezier: return toBezier(s, tolerance);
    case Kind::Poly:
    {
        TessOptions o;
        o.chordError = tolerance;
        Polyline pl = tessellate(s, o);
        Spline out;
        out.kind = Kind::Poly;
        out.closed = s.closed;
        out.points = pl.points;
        if (!s.tilt.empty() || !s.radius.empty())
            for (double u : pl.params)
            {
                if (!s.tilt.empty())
                    out.tilt.push_back(pointValueAt(s, s.tilt, 0.0, u));
                if (!s.radius.empty())
                    out.radius.push_back(pointValueAt(s, s.radius, 1.0, u));
            }
        return out;
    }
    }
    return s;
}

std::vector<Piece> arcPieces(const glm::dvec3& center, const glm::dvec3& axis, const glm::dvec3& start, double angle)
{
    std::vector<Piece> out;
    D3 ax = glm::length(axis) > kEps ? glm::normalize(axis) : D3(0, 1, 0);
    D3 rel = start - center;
    double h = glm::dot(rel, ax);
    D3 radial = rel - ax * h;
    double r = glm::length(radial);
    if (r < kEps || std::abs(angle) < 1e-12)
        return out;
    D3 base = center + ax * h;
    int k = std::max(1, int(std::ceil(std::abs(angle) / (glm::half_pi<double>() + 1e-9))));
    double d = angle / k;
    for (int i = 0; i < k; ++i)
    {
        double a0 = d * i, a1 = d * (i + 1), am = (a0 + a1) * 0.5;
        double w = std::cos(d * 0.5);
        Piece p;
        p.span = i;
        p.cv = {homo(base + glm::rotate(radial, a0, ax), 1.0), homo(base + glm::rotate(radial, am, ax) / w, w),
                homo(base + glm::rotate(radial, a1, ax), 1.0)};
        out.push_back(std::move(p));
    }
    return out;
}

Spline arc(const glm::dvec3& center, const glm::dvec3& axis, const glm::dvec3& start, double angle)
{
    return fromPieces(arcPieces(center, axis, start, angle), false, Kind::Nurbs);
}

Spline circle(const glm::dvec3& center, const glm::dvec3& normal, double radius)
{
    D3 nz = glm::length(normal) > kEps ? glm::normalize(normal) : D3(0, 1, 0);
    D3 ref = std::abs(nz.x) < 0.9 ? D3(1, 0, 0) : D3(0, 0, 1);
    D3 u = glm::normalize(ref - nz * glm::dot(ref, nz));
    D3 v = glm::cross(nz, u);
    Spline s;
    s.kind = Kind::Nurbs;
    s.degree = 2;
    s.closed = true;
    const double w = std::sqrt(0.5);
    const double cx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    const double cy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    for (int i = 0; i < 8; ++i)
    {
        s.points.push_back(center + (u * cx[i] + v * cy[i]) * radius);
        s.weights.push_back(i % 2 ? w : 1.0);
    }
    s.knots = {0, 1, 1, 2, 2, 3, 3, 4, 4};
    return s;
}

Piece linePiece(const glm::dvec3& a, const glm::dvec3& b)
{
    Piece p;
    p.cv = {homo(a, 1.0), homo(b, 1.0)};
    return p;
}

void updateHandles(Spline& s, const std::vector<std::pair<size_t, int>>& edited)
{
    if (s.kind != Kind::Bezier)
        return;
    size_t n = s.size();
    if (s.handleL.size() != n || s.handleR.size() != n || s.typeL.size() != n || s.typeR.size() != n)
    {
        s.handleL.resize(n, D3(0.0));
        s.handleR.resize(n, D3(0.0));
        s.typeL.resize(n, HandleType::Auto);
        s.typeR.resize(n, HandleType::Auto);
    }
    for (auto [i, side] : edited)
    {
        if (i >= n)
            continue;
        HandleType& t = side == 0 ? s.typeL[i] : s.typeR[i];
        HandleType& other = side == 0 ? s.typeR[i] : s.typeL[i];
        if (t == HandleType::Auto)
        {
            t = HandleType::Aligned;
            if (other == HandleType::Auto)
                other = HandleType::Aligned;
        }
        else if (t == HandleType::Vector)
            t = HandleType::Free;
    }
    auto neighbour = [&](size_t i, int dir) -> std::optional<D3> {
        if (s.closed)
            return s.points[(i + n + size_t(dir + int(n))) % n];
        if (dir < 0 && i == 0)
            return std::nullopt;
        if (dir > 0 && i + 1 >= n)
            return std::nullopt;
        return s.points[size_t(int(i) + dir)];
    };
    for (size_t i = 0; i < n; ++i)
    {
        const D3& p = s.points[i];
        std::optional<D3> prev = neighbour(i, -1), next = neighbour(i, 1);
        if (!prev && !next)
            continue;
        // A missing neighbour is the other one mirrored through the point, so an open end's auto handle runs straight.
        D3 a = prev ? *prev : p - (*next - p);
        D3 b = next ? *next : p + (p - *prev);
        D3 da = p - a, db = b - p;
        double la = glm::length(da), lb = glm::length(db);
        if (s.typeL[i] == HandleType::Auto || s.typeR[i] == HandleType::Auto)
        {
            D3 t = (la > kEps ? da / la : D3(0.0)) + (lb > kEps ? db / lb : D3(0.0));
            double tl = glm::length(t);
            t = tl > kEps ? t / tl : (lb > kEps ? db / lb : D3(1, 0, 0));
            // Handle length a chord over 2.5614 puts four auto points on a circle within 0.03%.
            if (s.typeL[i] == HandleType::Auto)
                s.handleL[i] = p - t * (la / 2.5614);
            if (s.typeR[i] == HandleType::Auto)
                s.handleR[i] = p + t * (lb / 2.5614);
        }
        if (s.typeL[i] == HandleType::Vector)
            s.handleL[i] = p - da / 3.0;
        if (s.typeR[i] == HandleType::Vector)
            s.handleR[i] = p + db / 3.0;
    }
    auto realign = [&](size_t i, int lead) {
        D3& h = lead == 0 ? s.handleL[i] : s.handleR[i];
        D3& o = lead == 0 ? s.handleR[i] : s.handleL[i];
        D3 dir = s.points[i] - h;
        double l = glm::length(dir), lo = glm::length(o - s.points[i]);
        if (l > kEps)
            o = s.points[i] + dir / l * lo;
    };
    for (auto [i, side] : edited)
        if (i < n && s.typeL[i] == HandleType::Aligned && s.typeR[i] == HandleType::Aligned)
            realign(i, side);
    // An aligned handle opposite an auto one follows it.
    for (size_t i = 0; i < n; ++i)
    {
        if (s.typeL[i] == HandleType::Auto && s.typeR[i] == HandleType::Aligned)
            realign(i, 0);
        else if (s.typeR[i] == HandleType::Auto && s.typeL[i] == HandleType::Aligned)
            realign(i, 1);
    }
}

void setHandleType(Spline& s, size_t i, HandleType t)
{
    if (s.kind != Kind::Bezier || i >= s.size())
        return;
    s.normalize();
    s.typeL[i] = s.typeR[i] = t;
    if (t == HandleType::Aligned)
    {
        // Line both up along the current mean direction.
        D3 dir = s.handleR[i] - s.handleL[i];
        double l = glm::length(dir);
        if (l > kEps)
        {
            dir /= l;
            s.handleL[i] = s.points[i] - dir * glm::length(s.handleL[i] - s.points[i]);
            s.handleR[i] = s.points[i] + dir * glm::length(s.handleR[i] - s.points[i]);
        }
    }
    updateHandles(s);
}

void transform(Spline& s, const glm::dmat4& m)
{
    auto xf = [&](D3& p) { p = D3(m * glm::dvec4(p, 1.0)); };
    for (D3& p : s.points)
        xf(p);
    for (D3& p : s.handleL)
        xf(p);
    for (D3& p : s.handleR)
        xf(p);
}

void reverse(Spline& s)
{
    size_t n = s.size();
    if (n == 0)
        return;
    std::ranges::reverse(s.points);
    std::ranges::reverse(s.weights);
    std::ranges::reverse(s.tilt);
    std::ranges::reverse(s.radius);
    std::ranges::reverse(s.cornerRadius);
    if (s.kind == Kind::Bezier)
    {
        std::ranges::reverse(s.handleL);
        std::ranges::reverse(s.handleR);
        std::ranges::reverse(s.typeL);
        std::ranges::reverse(s.typeR);
        std::swap(s.handleL, s.handleR);
        std::swap(s.typeL, s.typeR);
    }
    if (s.kind == Kind::Nurbs && !s.knots.empty())
    {
        if (!s.closed)
        {
            double a = s.knots.front(), b = s.knots.back();
            std::vector<double> k(s.knots.rbegin(), s.knots.rend());
            for (double& x : k)
                x = a + b - x;
            s.knots = k;
        }
        else
        {
            int p = s.degree;
            std::vector<double> d(n), nd(n);
            for (size_t j = 0; j < n; ++j)
                d[j] = s.knots[j + 1] - s.knots[j];
            for (size_t j = 0; j < n; ++j)
                nd[size_t((int(n) * 2 - 1 - int(j) - p) % int(n))] = d[j];
            std::vector<double> b(n + 1, s.knots.front());
            for (size_t j = 0; j < n; ++j)
                b[j + 1] = b[j] + nd[j];
            s.knots = b;
        }
    }
}

void bounds(const Spline& s, glm::dvec3& lo, glm::dvec3& hi)
{
    lo = D3(std::numeric_limits<double>::max());
    hi = D3(-std::numeric_limits<double>::max());
    auto add = [&](const D3& p) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    };
    for (const D3& p : s.points)
        add(p);
    for (const D3& p : s.handleL)
        add(p);
    for (const D3& p : s.handleR)
        add(p);
}

double pointValueAt(const Spline& s, const std::vector<double>& values, double def, double u)
{
    size_t n = values.size();
    if (n == 0 || n != s.size())
        return def;
    if (n == 1)
        return values[0];
    double x = u;
    if (s.kind == Kind::Nurbs)
    {
        int spans = std::max(1, spanCount(s));
        x = u / spans * double(s.closed ? n : n - 1);
    }
    double lim = double(s.closed ? n : n - 1);
    x = std::clamp(x, 0.0, lim);
    size_t i = std::min(size_t(std::floor(x)), s.closed ? n - 1 : n - 2);
    double t = x - double(i);
    return values[i] * (1.0 - t) + values[(i + 1) % n] * t;
}
} // namespace lila::curve
