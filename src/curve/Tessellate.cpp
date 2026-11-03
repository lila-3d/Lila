#include "curve/Tessellate.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lila::curve
{
namespace
{
constexpr double kKinkAngle = 1e-3; // radians: tangent breaks smaller than this are numerical noise

/** Arc length of a piece from t0 to t1. */
double pieceLength(const Piece& p, double t0, double t1, int pieces = 4)
{
    double total = 0.0;
    double h = (t1 - t0) / pieces;
    for (int k = 0; k < pieces; ++k)
    {
        double a = t0 + h * k, b = a + h;
        for (int g = 0; g < 8; ++g)
        {
            double t = (a + b) * 0.5 + (b - a) * 0.5 * kGx[g];
            total += kGw[g] * (b - a) * 0.5 * glm::length(evaluate(p, t).d1);
        }
    }
    return total;
}

D3 unitTangent(const Piece& p, double t)
{
    Sample s = evaluate(p, t);
    double l = glm::length(s.d1);
    if (l > 1e-9 * std::max(1.0, glm::length(p.end() - p.start())))
        return s.d1 / l;
    if (t <= 1e-9)
        return endTangent(p, false);
    if (t >= 1.0 - 1e-9)
        return endTangent(p, true);
    // A cusp inside the piece: look slightly ahead.
    Sample q = evaluate(p, std::min(1.0, t + 1e-6));
    return glm::length(q.d1) > kEps ? glm::normalize(q.d1) : endTangent(p, false);
}

void adaptive(const Piece& p, double a, double b, const D3& pa, const D3& pb, const TessOptions& o, int depth, std::vector<double>& out)
{
    double m = (a + b) * 0.5;
    bool split = false;
    if (depth < 16)
    {
        D3 chord = pb - pa;
        double cl = glm::length(chord);
        auto dev = [&](double t) {
            D3 q = evaluate(p, t).p - pa;
            if (cl < kEps)
                return glm::length(q);
            double along = std::clamp(glm::dot(q, chord) / (cl * cl), 0.0, 1.0);
            return glm::length(q - chord * along);
        };
        double err = std::max({dev(m), dev(a + (b - a) * 0.25), dev(a + (b - a) * 0.75)});
        double turn = angleBetween(unitTangent(p, a), unitTangent(p, b));
        split = err > o.chordError || turn > o.maxAngle;
    }
    if (!split)
    {
        out.push_back(a);
        return;
    }
    D3 pm = evaluate(p, m).p;
    adaptive(p, a, m, pa, pm, o, depth + 1, out);
    adaptive(p, m, b, pm, pb, o, depth + 1, out);
}

/** Local t values starting each segment of the piece (always 0 first). */
std::vector<double> pieceTs(const Piece& p, const TessOptions& o)
{
    if (isStraight(p))
        return {0.0};
    std::vector<double> ts;
    switch (o.mode)
    {
    case TessMode::Fixed:
    {
        int n = std::clamp(o.perSpan, 1, std::max(1, o.maxPerSpan));
        for (int i = 0; i < n; ++i)
            ts.push_back(double(i) / n);
        break;
    }
    case TessMode::Adaptive:
        adaptive(p, 0.0, 1.0, p.start(), p.end(), o, 0, ts);
        if (int(ts.size()) > o.maxPerSpan)
        {
            std::vector<double> thin;
            for (size_t i = 0; i < ts.size(); i += (ts.size() + size_t(o.maxPerSpan) - 1) / size_t(o.maxPerSpan))
                thin.push_back(ts[i]);
            ts = thin;
        }
        break;
    case TessMode::Game:
    {
        constexpr int kSamples = 64;
        double turn = 0.0;
        std::vector<double> acc(kSamples + 1, 0.0);
        D3 prevT = unitTangent(p, 0.0), prevP = p.start();
        for (int i = 1; i <= kSamples; ++i)
        {
            double t = double(i) / kSamples;
            D3 tt = unitTangent(p, t), pp = evaluate(p, t).p;
            turn += angleBetween(prevT, tt);
            acc[size_t(i)] = acc[size_t(i) - 1] + glm::distance(prevP, pp);
            prevT = tt;
            prevP = pp;
        }
        int n = std::clamp(int(std::ceil(turn / std::max(o.maxAngle, 1e-3) - 1e-9)), 1, std::max(1, o.maxPerSpan));
        double total = acc.back();
        for (int k = 0; k < n; ++k)
        {
            double target = total * k / n;
            size_t j = size_t(std::lower_bound(acc.begin(), acc.end(), target) - acc.begin());
            j = std::clamp<size_t>(j, 1, kSamples);
            double seg = acc[j] - acc[j - 1];
            double f = seg > kEps ? (target - acc[j - 1]) / seg : 0.0;
            ts.push_back((double(j - 1) + f) / kSamples);
        }
        ts.front() = 0.0;
        break;
    }
    }
    return ts;
}
} // namespace

const char* tessModeName(TessMode m)
{
    switch (m)
    {
    case TessMode::Fixed: return "fixed";
    case TessMode::Adaptive: return "adaptive";
    case TessMode::Game: return "game";
    }
    return "adaptive";
}

bool parseTessMode(std::string_view name, TessMode* out)
{
    for (TessMode m : {TessMode::Fixed, TessMode::Adaptive, TessMode::Game})
        if (name == tessModeName(m))
        {
            *out = m;
            return true;
        }
    return false;
}

const char* frameModeName(FrameMode m)
{
    switch (m)
    {
    case FrameMode::Rmf: return "rmf";
    case FrameMode::FixedUp: return "fixedUp";
    case FrameMode::SurfaceNormal: return "surfaceNormal";
    case FrameMode::Frenet: return "frenet";
    }
    return "rmf";
}

bool parseFrameMode(std::string_view name, FrameMode* out)
{
    for (FrameMode m : {FrameMode::Rmf, FrameMode::FixedUp, FrameMode::SurfaceNormal, FrameMode::Frenet})
        if (name == frameModeName(m))
        {
            *out = m;
            return true;
        }
    return false;
}

glm::dvec3 Polyline::tangent(size_t i) const
{
    D3 t = tangentIn[i] + tangentOut[i];
    double l = glm::length(t);
    return l > 1e-9 ? t / l : tangentOut[i];
}

size_t Polyline::segmentAt(double s, double* frac) const
{
    size_t n = points.size();
    if (n < 2)
    {
        *frac = 0.0;
        return 0;
    }
    if (closed)
        s = length > 0.0 ? std::fmod(std::fmod(s, length) + length, length) : 0.0;
    else
        s = std::clamp(s, 0.0, length);
    size_t segs = closed ? n : n - 1;
    size_t i = size_t(std::upper_bound(arcLength.begin(), arcLength.end(), s) - arcLength.begin());
    i = std::min(i == 0 ? 0 : i - 1, segs - 1);
    double end = i + 1 < n ? arcLength[i + 1] : length;
    double len = end - arcLength[i];
    *frac = len > kEps ? std::clamp((s - arcLength[i]) / len, 0.0, 1.0) : 0.0;
    return i;
}

glm::dvec3 Polyline::pointAtLength(double s) const
{
    if (points.empty())
        return D3(0.0);
    double f = 0.0;
    size_t i = segmentAt(s, &f);
    if (points.size() < 2)
        return points[0];
    return glm::mix(points[i], points[(i + 1) % points.size()], f);
}

Polyline tessellate(const Spline& s, const TessOptions& o)
{
    LILA_ZONE();

    Polyline pl;
    pl.closed = s.closed;
    std::vector<Piece> ps = pieces(s);
    if (ps.empty())
        return pl;
    size_t k = ps.size();
    for (size_t pi = 0; pi < k; ++pi)
    {
        const Piece& p = ps[pi];
        std::vector<double> ts = s.kind == Kind::Poly ? std::vector<double>{0.0} : pieceTs(p, o);
        for (size_t j = 0; j < ts.size(); ++j)
        {
            double t = ts[j];
            pl.points.push_back(j == 0 ? p.start() : evaluate(p, t).p);
            pl.params.push_back(double(p.span) + t);
            D3 tan = unitTangent(p, t);
            D3 tin = tan;
            bool kink = false;
            if (j == 0 && (pi > 0 || s.closed))
            {
                tin = endTangent(ps[(pi + k - 1) % k], true);
                kink = angleBetween(tin, tan) > kKinkAngle;
                if (!kink)
                    tin = tan = glm::normalize(tin + tan);
            }
            pl.tangentIn.push_back(tin);
            pl.tangentOut.push_back(tan);
            pl.kink.push_back(kink ? 1 : 0);
        }
    }
    if (!s.closed)
    {
        D3 t = endTangent(ps.back(), true);
        pl.points.push_back(ps.back().end());
        pl.params.push_back(double(k));
        pl.tangentIn.push_back(t);
        pl.tangentOut.push_back(t);
        pl.kink.push_back(0);
    }
    pl.arcLength.assign(pl.points.size(), 0.0);
    for (size_t i = 1; i < pl.points.size(); ++i)
        pl.arcLength[i] = pl.arcLength[i - 1] + glm::distance(pl.points[i - 1], pl.points[i]);
    pl.length = pl.arcLength.back() + (s.closed ? glm::distance(pl.points.back(), pl.points.front()) : 0.0);
    return pl;
}

Polyline polylineFrom(std::span<const glm::dvec3> pts, bool closed, double kinkAngle)
{
    Polyline pl;
    pl.closed = closed;
    size_t n = pts.size();
    pl.points.assign(pts.begin(), pts.end());
    pl.params.resize(n);
    pl.kink.assign(n, 0);
    pl.tangentIn.assign(n, D3(0, 0, 1));
    pl.tangentOut.assign(n, D3(0, 0, 1));
    pl.arcLength.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i)
        pl.params[i] = double(i);
    if (n < 2)
        return pl;
    auto seg = [&](size_t i) {
        D3 d = pts[(i + 1) % n] - pts[i];
        double l = glm::length(d);
        return l > kEps ? d / l : D3(0.0);
    };
    for (size_t i = 0; i < n; ++i)
    {
        bool hasIn = closed || i > 0, hasOut = closed || i + 1 < n;
        D3 tin = hasIn ? seg((i + n - 1) % n) : seg(i);
        D3 tout = hasOut ? seg(i) : tin;
        if (!hasIn)
            tin = tout;
        if (hasIn && hasOut && angleBetween(tin, tout) > kinkAngle)
        {
            pl.kink[i] = 1;
            pl.tangentIn[i] = tin;
            pl.tangentOut[i] = tout;
        }
        else
        {
            D3 avg = tin + tout;
            D3 t = glm::length(avg) > kEps ? glm::normalize(avg) : tout;
            pl.tangentIn[i] = pl.tangentOut[i] = t;
        }
    }
    for (size_t i = 1; i < n; ++i)
        pl.arcLength[i] = pl.arcLength[i - 1] + glm::distance(pts[i - 1], pts[i]);
    pl.length = pl.arcLength.back() + (closed ? glm::distance(pts.back(), pts.front()) : 0.0);
    return pl;
}

double length(const Spline& s)
{
    double total = 0.0;
    for (const Piece& p : pieces(s))
        total += isStraight(p) ? glm::distance(p.start(), p.end()) : pieceLength(p, 0.0, 1.0, 8);
    return total;
}

double paramAtLength(const Spline& s, double len)
{
    std::vector<Piece> ps = pieces(s);
    if (ps.empty())
        return 0.0;
    double acc = 0.0;
    for (size_t k = 0; k < ps.size(); ++k)
    {
        double l = pieceLength(ps[k], 0.0, 1.0, 8);
        if (acc + l >= len || k + 1 == ps.size())
        {
            double target = std::clamp(len - acc, 0.0, l);
            double t = l > kEps ? target / l : 0.0;
            for (int it = 0; it < 30; ++it)
            {
                double f = pieceLength(ps[k], 0.0, t, 2) - target;
                double d = glm::length(evaluate(ps[k], t).d1);
                if (std::abs(f) < 1e-9 || d < kEps)
                    break;
                t = std::clamp(t - f / d, 0.0, 1.0);
            }
            return double(k) + t;
        }
        acc += l;
    }
    return double(ps.size());
}

double closestParam(const Spline& s, const glm::dvec3& p, glm::dvec3* at)
{
    std::vector<Piece> ps = pieces(s);
    double bestU = 0.0, bestD = std::numeric_limits<double>::max();
    D3 bestP(0.0);
    for (size_t k = 0; k < ps.size(); ++k)
    {
        const Piece& pc = ps[k];
        constexpr int kSamples = 24;
        double bt = 0.0, bd = std::numeric_limits<double>::max();
        for (int i = 0; i <= kSamples; ++i)
        {
            double t = double(i) / kSamples;
            double d = glm::distance(evaluate(pc, t).p, p);
            if (d < bd)
            {
                bd = d;
                bt = t;
            }
        }
        for (int it = 0; it < 20; ++it)
        {
            Sample e = evaluate(pc, bt);
            D3 r = e.p - p;
            double f = glm::dot(r, e.d1);
            double df = glm::dot(e.d1, e.d1) + glm::dot(r, e.d2);
            if (std::abs(df) < kEps)
                break;
            double nt = std::clamp(bt - f / df, 0.0, 1.0);
            if (std::abs(nt - bt) < 1e-12)
                break;
            bt = nt;
        }
        D3 q = evaluate(pc, bt).p;
        double d = glm::distance(q, p);
        if (d < bestD)
        {
            bestD = d;
            bestU = double(k) + bt;
            bestP = q;
        }
    }
    if (at)
        *at = bestP;
    return bestU;
}

glm::dvec3 planeNormal(std::span<const glm::dvec3> pts, double tolerance)
{
    if (pts.size() < 3)
        return D3(0.0);
    D3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const D3& p : pts)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    double size = std::max(glm::length(hi - lo), 1e-9);
    const D3& o = pts[0];
    size_t far = 0;
    double fd = 0.0;
    for (size_t i = 1; i < pts.size(); ++i)
        if (double d = glm::distance(pts[i], o); d > fd)
        {
            fd = d;
            far = i;
        }
    if (fd < kEps)
        return D3(0.0);
    D3 a = pts[far] - o;
    D3 best(0.0);
    double bl = 0.0;
    for (const D3& p : pts)
    {
        D3 c = glm::cross(a, p - o);
        if (double l = glm::length(c); l > bl)
        {
            bl = l;
            best = c;
        }
    }
    if (bl < 1e-9 * size * size)
        return D3(0.0);
    D3 nrm = best / bl;
    for (const D3& p : pts)
        if (std::abs(glm::dot(p - o, nrm)) > tolerance * size)
            return D3(0.0);
    return nrm;
}

std::vector<Frame> frames(const Polyline& pl, const FrameOptions& o, std::span<const double> tilt)
{
    LILA_ZONE();

    size_t n = pl.size();
    std::vector<Frame> out(n);
    if (n == 0)
        return out;
    std::vector<D3> T(n);
    for (size_t i = 0; i < n; ++i)
        T[i] = pl.tangent(i);
    auto perp = [](const D3& v, const D3& t) {
        D3 r = v - t * glm::dot(v, t);
        double l = glm::length(r);
        return l > 1e-9 ? r / l : D3(0.0);
    };
    auto fallbackNormal = [&](const D3& t) {
        D3 r = perp(o.up, t);
        if (glm::length(r) < 0.5)
            r = perp(std::abs(t.x) < 0.9 ? D3(1, 0, 0) : D3(0, 0, 1), t);
        return r;
    };
    // Double reflection (Wang et al. 2008): carries a normal from point i to i + 1 with no rotation about the curve.
    auto transport = [&](const D3& r, size_t i, size_t j) {
        D3 v1 = pl.points[j] - pl.points[i];
        double c1 = glm::dot(v1, v1);
        if (c1 < 1e-24)
        {
            D3 q = perp(r, T[j]);
            return glm::length(q) > 0.5 ? q : fallbackNormal(T[j]);
        }
        D3 rL = r - (2.0 / c1) * glm::dot(v1, r) * v1;
        D3 tL = T[i] - (2.0 / c1) * glm::dot(v1, T[i]) * v1;
        D3 v2 = T[j] - tL;
        double c2 = glm::dot(v2, v2);
        D3 rn = c2 < 1e-24 ? rL : rL - (2.0 / c2) * glm::dot(v2, rL) * v2;
        D3 q = perp(rn, T[j]);
        return glm::length(q) > 0.5 ? q : fallbackNormal(T[j]);
    };

    D3 r0 = perp(o.startNormal, T[0]);
    if (glm::length(r0) < 0.5)
    {
        D3 pn = planeNormal(pl.points);
        if (glm::length(pn) > 0.5)
        {
            if (glm::dot(pn, o.up) < 0.0)
                pn = -pn;
            r0 = perp(pn, T[0]);
        }
    }
    if (glm::length(r0) < 0.5)
        r0 = fallbackNormal(T[0]);

    std::vector<D3> N(n);
    switch (o.mode)
    {
    case FrameMode::Rmf:
    case FrameMode::Frenet:
    case FrameMode::SurfaceNormal:
    {
        N[0] = r0;
        if (o.mode == FrameMode::SurfaceNormal && o.surfaceNormal)
            if (D3 sn = perp(o.surfaceNormal(pl.points[0]), T[0]); glm::length(sn) > 0.5)
                N[0] = sn;
        for (size_t i = 1; i < n; ++i)
        {
            N[i] = transport(N[i - 1], i - 1, i);
            if (o.mode == FrameMode::SurfaceNormal && o.surfaceNormal)
            {
                if (D3 sn = perp(o.surfaceNormal(pl.points[i]), T[i]); glm::length(sn) > 0.5)
                    N[i] = sn;
            }
        }
        if (o.mode == FrameMode::Frenet)
        {
            // Principal normal toward the centre of curvature; straight stretches keep the transported one.
            for (size_t i = 0; i < n; ++i)
            {
                bool hasPrev = pl.closed || i > 0, hasNext = pl.closed || i + 1 < n;
                if (!hasPrev || !hasNext)
                    continue;
                D3 k = T[(i + 1) % n] - T[(i + n - 1) % n];
                D3 pn = perp(k, T[i]);
                if (glm::length(k) > 1e-6 && glm::length(pn) > 0.5)
                    N[i] = pn;
                else if (i > 0)
                    N[i] = transport(N[i - 1], i - 1, i);
            }
        }
        if (o.mode == FrameMode::Rmf && pl.closed && n > 2 && (o.closeLoops || o.extraTurns != 0))
        {
            D3 back = transport(N[n - 1], n - 1, 0);
            double phi = o.closeLoops ? std::atan2(glm::dot(glm::cross(back, N[0]), T[0]), glm::dot(back, N[0])) : 0.0;
            phi += glm::two_pi<double>() * o.extraTurns;
            double L = pl.length > kEps ? pl.length : 1.0;
            for (size_t i = 1; i < n; ++i)
                N[i] = glm::rotate(N[i], phi * pl.arcLength[i] / L, T[i]);
        }
        break;
    }
    case FrameMode::FixedUp:
        for (size_t i = 0; i < n; ++i)
        {
            D3 r = perp(o.up, T[i]);
            N[i] = glm::length(r) > 0.5 ? r : (i > 0 ? transport(N[i - 1], i - 1, i) : fallbackNormal(T[i]));
        }
        break;
    }
    for (size_t i = 0; i < n; ++i)
    {
        D3 nn = N[i];
        if (i < tilt.size() && tilt[i] != 0.0)
            nn = glm::rotate(nn, tilt[i], T[i]);
        out[i].t = T[i];
        out[i].n = glm::normalize(nn);
        out[i].b = glm::normalize(glm::cross(T[i], out[i].n));
    }
    return out;
}

std::vector<double> tiltAlong(const Spline& s, const Polyline& pl)
{
    std::vector<double> out;
    if (s.tilt.empty())
        return out;
    out.reserve(pl.size());
    for (double u : pl.params)
        out.push_back(pointValueAt(s, s.tilt, 0.0, u));
    return out;
}

std::vector<CombTooth> curvatureComb(const Spline& s, int samples, double scale)
{
    std::vector<CombTooth> out;
    samples = std::max(1, samples);
    std::vector<Piece> ps = pieces(s);
    for (size_t k = 0; k < ps.size(); ++k)
    {
        bool last = k + 1 == ps.size() && !s.closed;
        for (int i = 0; i < samples + (last ? 1 : 0); ++i)
        {
            double t = double(i) / samples;
            Sample e = evaluate(ps[k], t);
            double l = glm::length(e.d1);
            if (l < kEps)
                continue;
            D3 tan = e.d1 / l;
            D3 nn = e.d2 - tan * glm::dot(e.d2, tan);
            double kappa = curvature(e);
            D3 dir = glm::length(nn) > kEps ? glm::normalize(nn) : D3(0.0);
            out.push_back({e.p, e.p - dir * kappa * scale, kappa});
        }
    }
    return out;
}
} // namespace lila::curve
