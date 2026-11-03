#include "curve/Intersect.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "curve/Ops.h"
#include "curve/Tessellate.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lila::curve
{
namespace
{
constexpr double kIntersectEps = 1e-12;

/** Flattens along a direction (unit, or zero for none). */
struct Projector
{
    D3 n{0.0};

    explicit Projector(const D3& dir)
    {
        if (glm::length(dir) > kIntersectEps)
            n = glm::normalize(dir);
    }
    D3 operator()(const D3& p) const { return p - n * glm::dot(p, n); }
};

/** Closest points of segments p0p1 and q0q1 as fractions along each. */
std::pair<double, double> segmentClosest(const D3& p0, const D3& p1, const D3& q0, const D3& q1)
{
    D3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    double a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    if (a <= kIntersectEps && e <= kIntersectEps)
        return {0.0, 0.0};
    if (a <= kIntersectEps)
        return {0.0, std::clamp(f / e, 0.0, 1.0)};
    double c = glm::dot(d1, r);
    if (e <= kIntersectEps)
        return {std::clamp(-c / a, 0.0, 1.0), 0.0};
    double b = glm::dot(d1, d2), den = a * e - b * b;
    double s = den > kIntersectEps * a * e ? std::clamp((b * f - c * e) / den, 0.0, 1.0) : 0.0;
    double t = (b * s + f) / e;
    if (t < 0.0)
        return {std::clamp(-c / a, 0.0, 1.0), 0.0};
    if (t > 1.0)
        return {std::clamp((b - c) / a, 0.0, 1.0), 1.0};
    return {s, t};
}

/** A curve as fine segments, flattened, with the span parameter at each point. */
struct Flat
{
    std::vector<D3> pts;
    std::vector<double> params;
    std::vector<D3> lo, hi; // per segment bounds
    double span = 0.0;      // spanCount

    size_t segments() const { return pts.size() > 1 ? pts.size() - 1 : 0; }
};

Flat flatten(const Spline& s, const Projector& proj, double tol)
{
    Flat f;
    TessOptions o;
    o.mode = TessMode::Adaptive;
    o.chordError = std::clamp(tol * 0.2, 1e-5, 0.05);
    o.maxAngle = 0.08;
    Polyline pl = tessellate(s, o);
    f.span = spanCount(s);
    for (size_t i = 0; i < pl.size(); ++i)
    {
        f.pts.push_back(proj(pl.points[i]));
        f.params.push_back(pl.params[i]);
    }
    if (pl.closed && !f.pts.empty())
    {
        f.pts.push_back(f.pts.front());
        f.params.push_back(f.span);
    }
    for (size_t i = 0; i < f.segments(); ++i)
    {
        f.lo.push_back(glm::min(f.pts[i], f.pts[i + 1]) - D3(tol));
        f.hi.push_back(glm::max(f.pts[i], f.pts[i + 1]) + D3(tol));
    }
    return f;
}

double wrapParam(const Spline& s, double u)
{
    double K = spanCount(s);
    if (!s.closed)
        return std::clamp(u, 0.0, K);
    u = std::fmod(u, K);
    return u < 0.0 ? u + K : u;
}

/** Gauss-Newton on the flattened gap between a(u) and b(v), from a coarse guess. */
void refine(const Spline& a, const Spline& b, const Projector& proj, double& u, double& v)
{
    for (int it = 0; it < 40; ++it)
    {
        Sample sa = evaluate(a, u), sb = evaluate(b, v);
        D3 F = proj(sa.p - sb.p), Ja = proj(sa.d1), Jb = -proj(sb.d1);
        double aa = glm::dot(Ja, Ja), ab = glm::dot(Ja, Jb), bb = glm::dot(Jb, Jb);
        double ra = -glm::dot(Ja, F), rb = -glm::dot(Jb, F);
        double det = aa * bb - ab * ab;
        if (std::abs(det) < 1e-14 * std::max(1.0, aa * bb))
            return;
        double du = (ra * bb - rb * ab) / det, dv = (aa * rb - ab * ra) / det;
        // Steps stay within a span so the clamped evaluation stays meaningful.
        du = std::clamp(du, -0.5, 0.5);
        dv = std::clamp(dv, -0.5, 0.5);
        double nu = wrapParam(a, u + du), nv = wrapParam(b, v + dv);
        if (std::abs(nu - u) < 1e-13 && std::abs(nv - v) < 1e-13)
            return;
        u = nu;
        v = nv;
    }
}

double paramGap(const Spline& s, double a, double b)
{
    double d = std::abs(a - b);
    if (s.closed)
        d = std::min(d, spanCount(s) - d);
    return d;
}

/** Distance from p to the flattened polyline. */
double distanceTo(const Flat& f, const D3& p)
{
    double best = std::numeric_limits<double>::max();
    for (size_t i = 0; i < f.segments(); ++i)
    {
        D3 d = f.pts[i + 1] - f.pts[i];
        double len2 = glm::dot(d, d);
        double t = len2 > kIntersectEps ? std::clamp(glm::dot(p - f.pts[i], d) / len2, 0.0, 1.0) : 0.0;
        best = std::min(best, glm::distance(f.pts[i] + d * t, p));
    }
    return best;
}

D4 hom(const D3& p, double w = 1.0) { return D4(p * w, w); }

/** Anchors whose handles line up become aligned, so a cut Bezier edits like the original. */
void tidyHandles(Spline& s)
{
    if (s.kind != Kind::Bezier || s.handleL.size() != s.size())
        return;
    for (size_t i = 0; i < s.size(); ++i)
    {
        D3 l = s.points[i] - s.handleL[i], r = s.handleR[i] - s.points[i];
        if (glm::length(l) > 1e-9 && glm::length(r) > 1e-9 && glm::dot(glm::normalize(l), glm::normalize(r)) > 1.0 - 1e-9)
            s.typeL[i] = s.typeR[i] = HandleType::Aligned;
    }
}

/** The part of an open NURBS between span parameters u0 < u1, its knots kept (knots inserted to full multiplicity at the cuts). */
Spline sliceNurbs(const Spline& src, double u0, double u1)
{
    Spline s = src;
    const int p = s.degree;
    const double a = knotParam(s, u0), b = knotParam(s, u1);
    std::vector<double> U0 = knotVector(s);
    const double lo = U0[size_t(p)], hi = U0[U0.size() - 1 - size_t(p)];
    const double eps = 1e-10 * std::max(1.0, hi - lo);
    auto multiplicity = [&](double k) {
        int m = 0;
        for (double x : knotVector(s))
            m += std::abs(x - k) <= eps;
        return m;
    };
    auto raise = [&](double k) {
        int m = multiplicity(k);
        if (m < p)
            insertKnot(s, spanParam(s, k), p - m);
    };
    if (a > lo + eps)
        raise(a);
    if (b < hi - eps)
        raise(b);
    std::vector<double> U = knotVector(s);
    const size_t n = s.size();
    auto first = [&](double k) {
        for (size_t i = 0; i < U.size(); ++i)
            if (std::abs(U[i] - k) <= eps)
                return i;
        return U.size();
    };
    size_t ia = a <= lo + eps ? 1 : first(a);
    size_t ib = b >= hi - eps ? n : first(b);
    if (ia == 0 || ia >= U.size() || ib > n || ib < ia)
        return src;
    Spline out;
    out.kind = Kind::Nurbs;
    out.degree = p;
    for (size_t i = ia - 1; i < ib; ++i)
    {
        out.points.push_back(s.points[i]);
        if (!s.weights.empty())
            out.weights.push_back(s.weight(i));
    }
    out.knots.push_back(U[ia]);
    const size_t last = ib + size_t(p) - 1;
    if (last >= U.size())
        return src;
    for (size_t i = ia; i <= last; ++i)
        out.knots.push_back(U[i]);
    out.knots.push_back(U[last]);
    return isValid(out) ? out : src;
}

/** Joins b onto the end of a, both open NURBS of a's degree with b starting where a ends: knots and points concatenated. */
Spline joinNurbs(const Spline& a, Spline b)
{
    const int p = a.degree;
    if (b.kind != Kind::Nurbs || b.degree != p)
    {
        std::vector<Piece> ps = pieces(b);
        for (Piece& pc : ps)
            pc = elevate(pc, p);
        b = fromPieces(ps, false, Kind::Nurbs);
        if (b.degree != p)
            return a;
    }
    std::vector<double> Ua = knotVector(a), Ub = knotVector(b);
    Spline out = a;
    out.knots.assign(Ua.begin(), Ua.end() - 1);
    const double shift = Ua.back() - Ub[size_t(p)];
    for (size_t i = size_t(p) + 1; i < Ub.size(); ++i)
        out.knots.push_back(Ub[i] + shift);
    const double scale = a.weight(a.size() - 1) / std::max(b.weight(0), kIntersectEps);
    const bool rational = a.rational() || b.rational() || std::abs(scale - 1.0) > 1e-12;
    if (rational && out.weights.empty())
        out.weights.assign(a.size(), 1.0);
    for (size_t i = 1; i < b.size(); ++i)
    {
        out.points.push_back(b.points[i]);
        if (rational)
            out.weights.push_back(b.weight(i) * scale);
    }
    out.tilt.clear();
    out.radius.clear();
    out.cornerRadius.clear();
    return isValid(out) ? out : a;
}

/** Continues the piece past t = 1 to tmax (de Casteljau extrapolates). */
Piece continued(const Piece& p, double tmax)
{
    Piece l, r, l2, r2;
    split(p, tmax, l, r);
    split(l, 1.0 / tmax, l2, r2);
    return r2;
}

double pieceChordLength(const Piece& p)
{
    double len = 0.0;
    D3 prev = evaluate(p, 0.0).p;
    for (int i = 1; i <= 48; ++i)
    {
        D3 q = evaluate(p, double(i) / 48).p;
        len += glm::distance(prev, q);
        prev = q;
    }
    return len;
}
} // namespace

std::vector<CurveHit> intersect(const Spline& a, const Spline& b, const IntersectOptions& o)
{
    LILA_ZONE();

    std::vector<CurveHit> out;
    if (!isValid(a) || !isValid(b) || spanCount(a) == 0 || spanCount(b) == 0)
        return out;
    const double tol = std::max(o.tolerance, 1e-7);
    Projector proj(o.direction);
    Flat fa = flatten(a, proj, tol), fb = flatten(b, proj, tol);
    // Segments of the flattening sit within the chord error of the curve, so candidates allow for both.
    const double reach = tol * 1.5 + std::clamp(tol * 0.2, 1e-5, 0.05) * 2.0;
    std::vector<CurveHit> cand;
    for (size_t i = 0; i < fa.segments(); ++i)
        for (size_t j = 0; j < fb.segments(); ++j)
        {
            if (fa.hi[i].x < fb.lo[j].x || fb.hi[j].x < fa.lo[i].x || fa.hi[i].y < fb.lo[j].y || fb.hi[j].y < fa.lo[i].y ||
                fa.hi[i].z < fb.lo[j].z || fb.hi[j].z < fa.lo[i].z)
                continue;
            auto [s, t] = segmentClosest(fa.pts[i], fa.pts[i + 1], fb.pts[j], fb.pts[j + 1]);
            D3 pa = glm::mix(fa.pts[i], fa.pts[i + 1], s), pb = glm::mix(fb.pts[j], fb.pts[j + 1], t);
            if (glm::distance(pa, pb) > reach)
                continue;
            CurveHit h;
            h.ua = glm::mix(fa.params[i], fa.params[i + 1], s);
            h.ub = glm::mix(fb.params[j], fb.params[j + 1], t);
            cand.push_back(h);
        }
    for (CurveHit& h : cand)
    {
        double u = h.ua, v = h.ub;
        refine(a, b, proj, u, v);
        D3 pa = pointAt(a, u), pb = pointAt(b, v);
        if (glm::length(proj(pa - pb)) > tol)
        {
            // Tangential contact: Newton stalls, so keep the coarse guess when it is close enough.
            u = h.ua;
            v = h.ub;
            pa = pointAt(a, u);
            pb = pointAt(b, v);
            if (glm::length(proj(pa - pb)) > tol)
                continue;
        }
        bool dup = false;
        for (const CurveHit& q : out)
            if (glm::distance(q.pa, pa) < std::max(tol, 1e-6) && glm::distance(q.pb, pb) < std::max(tol, 1e-6))
            {
                dup = true;
                break;
            }
        if (!dup)
            out.push_back({u, v, pa, pb});
    }
    std::sort(out.begin(), out.end(), [](const CurveHit& x, const CurveHit& y) { return x.ua < y.ua; });
    // A stretch where the curves run together keeps only its two ends.
    if (out.size() > 2)
    {
        std::vector<bool> inside(out.size(), false);
        for (size_t i = 0; i + 1 < out.size(); ++i)
        {
            double mid = (out[i].ua + out[i + 1].ua) * 0.5;
            bool together = distanceTo(fb, proj(pointAt(a, mid))) <= tol;
            if (together && i + 2 < out.size())
            {
                double mid2 = (out[i + 1].ua + out[i + 2].ua) * 0.5;
                if (distanceTo(fb, proj(pointAt(a, mid2))) <= tol)
                    inside[i + 1] = true;
            }
        }
        std::vector<CurveHit> kept;
        for (size_t i = 0; i < out.size(); ++i)
            if (!inside[i])
                kept.push_back(out[i]);
        out = std::move(kept);
    }
    return out;
}

std::vector<double> intersectionParams(const Spline& s, const Splines& others, const IntersectOptions& o)
{
    std::vector<double> us;
    for (const Spline& b : others)
        for (const CurveHit& h : intersect(s, b, o))
            us.push_back(h.ua);
    std::sort(us.begin(), us.end());
    std::vector<double> out;
    for (double u : us)
        if (out.empty() || paramGap(s, out.back(), u) > 1e-7)
            out.push_back(u);
    if (s.closed && out.size() > 1 && paramGap(s, out.front(), out.back()) <= 1e-7)
        out.pop_back();
    return out;
}

Spline subCurve(const Spline& s, double u0, double u1)
{
    LILA_ZONE();

    const int K = spanCount(s);
    if (K == 0)
        return s;
    u0 = std::clamp(u0, 0.0, double(K));
    u1 = std::clamp(u1, 0.0, double(K));
    if (!s.closed && u1 < u0)
        std::swap(u0, u1);
    if (!s.closed && u1 - u0 < 1e-12)
        return {};
    if (s.kind == Kind::Nurbs && !s.closed)
        return sliceNurbs(s, u0, u1);
    std::vector<Piece> ps = pieces(s), out;
    auto take = [&](double a, double b) {
        for (size_t k = size_t(std::floor(a)); k < size_t(K) && double(k) < b - 1e-12; ++k)
        {
            double t0 = std::max(a - double(k), 0.0), t1 = std::min(b - double(k), 1.0);
            if (t1 - t0 > 1e-12)
                out.push_back(subPiece(ps[k], t0, t1));
        }
    };
    if (s.closed && u1 <= u0)
    {
        take(u0, K);
        take(0.0, u1);
    }
    else
        take(u0, u1);
    if (out.empty())
        return {};
    Spline r = fromPieces(out, false, s.kind);
    tidyHandles(r);
    return r;
}

std::optional<CurveInterval> intervalAt(const Spline& s, const std::vector<double>& cuts0, double u)
{
    const double K = spanCount(s);
    if (K <= 0.0)
        return std::nullopt;
    std::vector<double> cuts;
    for (double c : cuts0)
    {
        c = s.closed ? wrapParam(s, c) : c;
        if (!s.closed && (c <= 1e-9 || c >= K - 1e-9))
            continue;
        cuts.push_back(c);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double a, double b) { return std::abs(a - b) < 1e-9; }), cuts.end());
    u = s.closed ? wrapParam(s, u) : std::clamp(u, 0.0, K);
    if (!s.closed)
    {
        double lo = 0.0, hi = K;
        for (double c : cuts)
        {
            if (c <= u)
                lo = c;
            else
            {
                hi = c;
                break;
            }
        }
        return CurveInterval{lo, hi};
    }
    if (cuts.empty())
        return CurveInterval{0.0, K};
    if (cuts.size() == 1)
        return CurveInterval{cuts[0], cuts[0]};
    for (size_t i = 0; i + 1 < cuts.size(); ++i)
        if (u >= cuts[i] && u < cuts[i + 1])
            return CurveInterval{cuts[i], cuts[i + 1]};
    return CurveInterval{cuts.back(), cuts.front()};
}

Splines trimAt(const Spline& s, const std::vector<double>& cuts, double u)
{
    Splines out;
    std::optional<CurveInterval> iv = intervalAt(s, cuts, u);
    if (!iv)
        return {s};
    const double K = spanCount(s);
    auto keep = [&](Spline piece) {
        if (isValid(piece) && length(piece) > 1e-9)
            out.push_back(std::move(piece));
    };
    if (!s.closed)
    {
        if (iv->u0 > 1e-9)
            keep(subCurve(s, 0.0, iv->u0));
        if (iv->u1 < K - 1e-9)
            keep(subCurve(s, iv->u1, K));
        return out;
    }
    // A loop with fewer than two cuts has nothing left once its one stretch goes.
    if (std::abs(iv->u0 - iv->u1) < 1e-12 || (iv->u0 == 0.0 && iv->u1 == K))
        return out;
    keep(subCurve(s, iv->u1, iv->u0));
    return out;
}

Splines splitAt(const Spline& s, std::vector<double> cuts)
{
    const double K = spanCount(s);
    std::vector<double> c;
    for (double x : cuts)
    {
        x = s.closed ? wrapParam(s, x) : x;
        if (!s.closed && (x <= 1e-9 || x >= K - 1e-9))
            continue;
        c.push_back(x);
    }
    std::sort(c.begin(), c.end());
    c.erase(std::unique(c.begin(), c.end(), [](double a, double b) { return std::abs(a - b) < 1e-9; }), c.end());
    Splines out;
    auto keep = [&](Spline piece) {
        if (isValid(piece) && length(piece) > 1e-9)
            out.push_back(std::move(piece));
    };
    if (c.empty())
        return {s};
    if (!s.closed)
    {
        double from = 0.0;
        for (double x : c)
        {
            keep(subCurve(s, from, x));
            from = x;
        }
        keep(subCurve(s, from, K));
        return out;
    }
    for (size_t i = 0; i < c.size(); ++i)
        keep(subCurve(s, c[i], c[(i + 1) % c.size()]));
    return out;
}

const char* extendModeName(ExtendMode m)
{
    switch (m)
    {
    case ExtendMode::Line: return "line";
    case ExtendMode::Arc: return "arc";
    case ExtendMode::Smooth: return "smooth";
    }
    return "smooth";
}

bool parseExtendMode(std::string_view name, ExtendMode* out)
{
    for (ExtendMode m : {ExtendMode::Line, ExtendMode::Arc, ExtendMode::Smooth})
        if (name == extendModeName(m))
        {
            *out = m;
            return true;
        }
    return false;
}

std::optional<Spline> extension(const Spline& src, bool atEnd, ExtendMode mode, double len)
{
    LILA_ZONE();

    if (src.closed || !isValid(src) || !(len > 0.0) || !std::isfinite(len))
        return std::nullopt;
    Spline s = src;
    if (!atEnd)
        reverse(s);
    std::vector<Piece> ps = pieces(s);
    if (ps.empty())
        return std::nullopt;
    const Piece& last = ps.back();
    const D3 end = last.end(), T = endTangent(last, true);
    if (glm::length(T) < 0.5)
        return std::nullopt;
    Sample e = evaluate(last, 1.0);
    std::vector<Piece> ext;
    D3 bend = e.d2 - T * glm::dot(e.d2, T);
    double speed2 = glm::dot(e.d1, e.d1);
    double kappa = speed2 > kIntersectEps ? glm::length(bend) / speed2 : 0.0;
    if (mode == ExtendMode::Arc && kappa * len > 1e-6)
    {
        double r = 1.0 / kappa;
        D3 N = glm::normalize(bend);
        double angle = std::min(len / r, glm::two_pi<double>() * 0.98);
        ext = arcPieces(end + N * r, glm::normalize(glm::cross(T, N)), end, angle);
    }
    else if (mode == ExtendMode::Smooth && !isStraight(last))
    {
        // The longest continuation whose weights stay positive, then the parameter giving the wanted length.
        double tmax = 2.0;
        auto ok = [&](double t) {
            Piece q = continued(last, t);
            return std::ranges::all_of(q.cv, [](const D4& c) { return c.w > kIntersectEps; });
        };
        while (tmax < 256.0 && ok(tmax * 2.0) && pieceChordLength(continued(last, tmax)) < len * 1.05)
            tmax *= 2.0;
        while (!ok(tmax) && tmax > 1.0 + 1e-6)
            tmax = 1.0 + (tmax - 1.0) * 0.5;
        double lo = 1.0, hi = tmax;
        if (pieceChordLength(continued(last, hi)) > len)
            for (int it = 0; it < 50; ++it)
            {
                double mid = (lo + hi) * 0.5;
                (pieceChordLength(continued(last, mid)) < len ? lo : hi) = mid;
            }
        ext = {continued(last, hi)};
    }
    else
        ext = {linePiece(end, end + T * len)};
    // Exact on the end, whatever rounding the construction did.
    ext.front().cv.front() = hom(end, ext.front().cv.front().w);
    Kind kind = src.kind == Kind::Poly && mode != ExtendMode::Line && ext.size() == 1 && isStraight(ext.front()) ? Kind::Poly : src.kind;
    Spline out = fromPieces(ext, false, kind);
    if (!isValid(out))
        return std::nullopt;
    double have = length(out);
    if (have > len * (1.0 + 1e-9))
        out = subCurve(out, 0.0, paramAtLength(out, len));
    return isValid(out) ? std::optional<Spline>(out) : std::nullopt;
}

namespace
{
/** s with ext (leaving its end, or its start when !atEnd) joined on. */
Spline joined(const Spline& s, const Spline& ext, bool atEnd)
{
    Spline base = s;
    if (!atEnd)
        reverse(base);
    Spline out = base.kind == Kind::Nurbs ? joinNurbs(base, ext) : attach(base, ext, false, AttachEnds::EndStart, 1e-6);
    if (!atEnd)
        reverse(out);
    return out;
}
} // namespace

ExtendResult extendTo(const Spline& s, bool atEnd, const Splines& boundaries, const ExtendOptions& o)
{
    LILA_ZONE();

    ExtendResult r{s, false, 0.0};
    if (s.closed || !isValid(s) || boundaries.empty())
        return r;
    D3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const Spline& b : boundaries)
    {
        D3 bl, bh;
        bounds(b, bl, bh);
        lo = glm::min(lo, bl);
        hi = glm::max(hi, bh);
    }
    D3 sl, sh;
    bounds(s, sl, sh);
    lo = glm::min(lo, sl);
    hi = glm::max(hi, sh);
    double reach = o.maxLength > 0.0 ? o.maxLength : std::max(1.0, glm::distance(lo, hi) * 10.0);
    std::optional<Spline> ext = extension(s, atEnd, o.mode, reach);
    if (!ext)
        return r;
    const D3 start = pointAt(*ext, 0.0);
    const double guard = std::max(o.intersect.tolerance * 2.0, 1e-6);
    double best = std::numeric_limits<double>::max();
    for (const Spline& b : boundaries)
        for (const CurveHit& h : intersect(*ext, b, o.intersect))
            if (glm::distance(h.pa, start) > guard && h.ua < best)
                best = h.ua;
    if (best == std::numeric_limits<double>::max())
        return r;
    Spline piece = subCurve(*ext, 0.0, best);
    if (!isValid(piece))
        return r;
    r.curve = joined(s, piece, atEnd);
    r.hit = true;
    r.added = length(piece);
    return r;
}

Spline extendBy(const Spline& s, bool atEnd, ExtendMode mode, double len)
{
    std::optional<Spline> ext = extension(s, atEnd, mode, len);
    return ext ? joined(s, *ext, atEnd) : s;
}
} // namespace lila::curve
