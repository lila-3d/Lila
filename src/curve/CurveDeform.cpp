#include "curve/CurveDeform.h"

#include "core/Profile.h"
#include "curve/Along.h"
#include "curve/Fit.h"
#include "curve/Tessellate.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace lila::curve
{
namespace
{
using D3 = glm::dvec3;

constexpr double kEps = 1e-12;

TessOptions fineTess()
{
    TessOptions t;
    t.chordError = 1e-3;
    t.maxAngle = 0.02;
    return t;
}

bool masked(std::span<const uint8_t> mask, size_t i) { return mask.empty() || (i < mask.size() && mask[i]); }

D3 perp(const D3& v, const D3& t)
{
    D3 r = v - t * glm::dot(v, t);
    double l = glm::length(r);
    return l > 1e-9 ? r / l : D3(0.0);
}

/** Moves each masked point (and a Bezier point's handles) through fn(position, point index), then refreshes handles. */
Spline mapPoints(const Spline& s, std::span<const uint8_t> mask, const std::function<D3(const D3&, size_t)>& fn)
{
    Spline out = s;
    out.normalize();
    bool bezier = out.kind == Kind::Bezier;
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (!masked(mask, i))
            continue;
        out.points[i] = fn(s.points[i], i);
        if (bezier)
        {
            out.handleL[i] = fn(out.handleL[i], i);
            out.handleR[i] = fn(out.handleR[i], i);
        }
    }
    if (bezier)
        updateHandles(out);
    return out;
}

/** Arc length of the polyline point nearest p, and the segment and fraction it lies on. */
double arcOf(const Polyline& pl, const D3& p, size_t* segment = nullptr, double* fraction = nullptr)
{
    size_t n = pl.size(), segs = pl.closed ? n : n - 1;
    double best = std::numeric_limits<double>::max(), arc = 0.0;
    for (size_t k = 0; k < segs; ++k)
    {
        const D3 &a = pl.points[k], &b = pl.points[(k + 1) % n];
        D3 ab = b - a;
        double len2 = glm::dot(ab, ab);
        double f = len2 > kEps ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
        D3 q = a + ab * f;
        if (double d = glm::dot(p - q, p - q); d < best)
        {
            best = d;
            double end = k + 1 < n ? pl.arcLength[k + 1] : pl.length;
            arc = pl.arcLength[k] + (end - pl.arcLength[k]) * f;
            if (segment)
                *segment = k;
            if (fraction)
                *fraction = f;
        }
    }
    return arc;
}

/** Span parameter at arc length s along the tessellation. */
double paramAt(const Spline& s, const Polyline& pl, double arc)
{
    double f = 0.0;
    size_t i = pl.segmentAt(arc, &f);
    double u0 = pl.params[i], u1 = i + 1 < pl.size() ? pl.params[i + 1] : double(spanCount(s));
    return u0 + (u1 - u0) * f;
}

/** Points spread evenly along the polyline: count of them, the end included on open curves. */
std::vector<D3> evenSamples(const Polyline& pl, size_t count)
{
    std::vector<D3> pts;
    size_t div = pl.closed ? count : count - 1;
    for (size_t i = 0; i < count; ++i)
        pts.push_back(pl.pointAtLength(pl.length * double(i) / double(std::max<size_t>(1, div))));
    return pts;
}

/** Copies a per-point array onto new points at the given span parameters of the old spline. */
std::vector<double> resampleValues(const Spline& old, const std::vector<double>& values, double def, const std::vector<double>& params)
{
    std::vector<double> out;
    if (values.empty())
        return out;
    for (double u : params)
        out.push_back(pointValueAt(old, values, def, u));
    return out;
}

/** Handle length for a cubic spanning arc length l between tangents at angle theta: exact for circular arcs. */
double handleLength(double l, double theta) { return theta > 1e-6 ? l * (4.0 / 3.0) * std::tan(theta / 4.0) / theta : l / 3.0; }
/**
 * Handle lengths along the given end tangents for a cubic from a to b that best follows the polyline's stretch of arc
 * length l from arc (least squares at even arc-length parameters). Falls back to guess when the fit degenerates.
 */
std::pair<double, double> fitHandles(const Polyline& pl, double arc, double l, const D3& a, const D3& b, const D3& ta, const D3& tb, double guess)
{
    constexpr int kSamples = 24;
    double c00 = 0.0, c01 = 0.0, c11 = 0.0, x0 = 0.0, x1 = 0.0;
    for (int k = 1; k < kSamples; ++k)
    {
        double t = double(k) / kSamples, mt = 1.0 - t;
        double b0 = mt * mt * mt, b1 = 3.0 * t * mt * mt, b2 = 3.0 * t * t * mt, b3 = t * t * t;
        D3 d = pl.pointAtLength(arc + l * t);
        D3 a1 = ta * b1, a2 = -tb * b2;
        D3 rest = d - (a * (b0 + b1) + b * (b2 + b3));
        c00 += glm::dot(a1, a1);
        c01 += glm::dot(a1, a2);
        c11 += glm::dot(a2, a2);
        x0 += glm::dot(a1, rest);
        x1 += glm::dot(a2, rest);
    }
    double det = c00 * c11 - c01 * c01;
    if (std::abs(det) < 1e-12)
        return {guess, guess};
    double ha = (x0 * c11 - x1 * c01) / det, hb = (c00 * x1 - c01 * x0) / det;
    // Negative or runaway handles mean the stretch isn't one cubic's worth of curve: keep the arc guess.
    if (ha < 1e-6 * l || hb < 1e-6 * l || ha > l || hb > l)
        return {guess, guess};
    return {ha, hb};
}
} // namespace

Spline bendCurve(const Spline& s, const BendCurveOptions& o, std::span<const uint8_t> mask)
{
    LILA_ZONE();

    if (!isValid(s) || std::abs(o.angle) < 1e-9)
        return s;
    Polyline pl = tessellate(s);
    if (pl.size() < 2)
        return s;
    D3 origin = pl.points.front(), d(1, 0, 0);
    double chord = s.closed ? 0.0 : glm::distance(pl.points.back(), origin);
    if (chord > 1e-9)
        d = (pl.points.back() - origin) / chord;
    else
    {
        D3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
        for (const D3& p : pl.points)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        D3 size = hi - lo;
        int axis = size.x >= size.y && size.x >= size.z ? 0 : size.y >= size.z ? 1 : 2;
        d = D3(0.0);
        d[axis] = 1.0;
        chord = size[axis];
        origin = (lo + hi) * 0.5 - d * (chord * 0.5);
    }
    if (chord < 1e-9)
        return s;
    D3 axis = perp(o.axis, d);
    if (glm::length(axis) < 0.5)
    {
        D3 pn = planeNormal(pl.points);
        if (glm::dot(pn, D3(0, 1, 0)) < 0.0)
            pn = -pn;
        axis = perp(pn, d);
    }
    if (glm::length(axis) < 0.5)
        axis = perp(D3(0, 1, 0), d);
    if (glm::length(axis) < 0.5)
        axis = perp(D3(0, 0, 1), d);
    D3 k = glm::cross(axis, d);
    if (o.fromCenter)
        origin += d * (chord * 0.5);
    double R = chord / o.angle;
    return mapPoints(s, mask, [&](const D3& p, size_t) {
        D3 q = p - origin;
        double x = glm::dot(q, d), y = glm::dot(q, k), z = glm::dot(q, axis);
        double th = x / R, r = R - y;
        return origin + d * (r * std::sin(th)) + k * (R - r * std::cos(th)) + axis * z;
    });
}

Spline curlCurve(const Spline& s, const CurlCurveOptions& o, std::span<const uint8_t> mask)
{
    LILA_ZONE();

    Rail rail(s);
    if (!rail.valid() || o.amplitude == 0.0 || o.turns == 0.0)
        return s;
    const double L = rail.length();
    auto ease = [&](double t) {
        if (!o.keepEnds || s.closed)
            return 1.0;
        auto smooth = [](double x) {
            x = std::clamp(x / 0.1, 0.0, 1.0);
            return x * x * (3.0 - 2.0 * x);
        };
        return smooth(t) * smooth(1.0 - t);
    };
    auto curled = [&](double arc, const D3& p) {
        Rail::Station st = rail.at(arc, false);
        double t = arc / L, phi = glm::two_pi<double>() * o.turns * t;
        return p + (st.f.n * std::cos(phi) + st.f.b * std::sin(phi)) * (o.amplitude * ease(t));
    };
    if (mask.empty() && o.samplesPerTurn > 0)
    {
        size_t n = size_t(std::max(8.0, std::ceil(std::abs(o.turns) * o.samplesPerTurn))) + (s.closed ? 0 : 1);
        std::vector<D3> pts;
        size_t div = s.closed ? n : n - 1;
        for (size_t i = 0; i < n; ++i)
        {
            double arc = L * double(i) / double(div);
            pts.push_back(curled(arc, rail.at(arc, false).p));
        }
        Spline out;
        if (s.kind == Kind::Poly)
        {
            out.kind = Kind::Poly;
            out.closed = s.closed;
            out.points = pts;
        }
        else if (s.kind == Kind::Bezier)
            out = catmullRom(pts, s.closed);
        else
            out = interpolate(pts, std::clamp(s.degree, 2, 3), s.closed);
        out.normalize();
        return out;
    }
    Polyline pl = tessellate(s, fineTess());
    return mapPoints(s, mask, [&](const D3& p, size_t i) {
        // Handles ride with their anchor.
        double arc = arcOf(pl, s.points[i]);
        return curled(arc, p);
    });
}

Spline scaleCurvature(const Spline& s, double factor, std::span<const uint8_t> mask)
{
    LILA_ZONE();

    if (!isValid(s) || std::abs(factor - 1.0) < 1e-12)
        return s;
    Polyline pl = tessellate(s, fineTess());
    size_t n = pl.size();
    if (n < 2)
        return s;
    size_t segs = pl.closed ? n : n - 1;
    std::vector<D3> dir(segs);
    std::vector<double> len(segs);
    for (size_t i = 0; i < segs; ++i)
    {
        D3 e = pl.points[(i + 1) % n] - pl.points[i];
        len[i] = glm::length(e);
        dir[i] = len[i] > kEps ? e / len[i] : (i > 0 ? dir[i - 1] : pl.tangentOut[0]);
    }
    // Each segment's turn from the last is scaled; M carries the original frame onto the new one.
    std::vector<glm::dmat3> M(segs, glm::dmat3(1.0));
    std::vector<D3> start(segs + 1), newDir(segs);
    start[0] = pl.points[0];
    newDir[0] = dir[0];
    for (size_t i = 1; i < segs; ++i)
    {
        D3 ax = glm::cross(dir[i - 1], dir[i]);
        double sl = glm::length(ax);
        M[i] = M[i - 1];
        if (sl > 1e-12)
        {
            ax /= sl;
            double ang = std::atan2(sl, glm::dot(dir[i - 1], dir[i]));
            glm::dmat3 rOld(glm::rotate(glm::dmat4(1.0), ang, ax));
            glm::dmat3 rNew(glm::rotate(glm::dmat4(1.0), factor * ang, M[i - 1] * ax));
            M[i] = rNew * M[i - 1] * glm::transpose(rOld);
        }
        newDir[i] = glm::normalize(M[i] * dir[i]);
    }
    for (size_t i = 0; i < segs; ++i)
        start[i + 1] = start[i] + newDir[i] * len[i];

    Spline out = s;
    out.normalize();
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (!masked(mask, i))
            continue;
        size_t j = 0;
        double f = 0.0;
        arcOf(pl, s.points[i], &j, &f);
        D3 was = pl.points[j] + (pl.points[(j + 1) % n] - pl.points[j]) * f;
        D3 now = start[j] + newDir[j] * (len[j] * f);
        auto move = [&](const D3& p) { return now + M[j] * (p - was); };
        out.points[i] = move(s.points[i]);
        if (out.kind == Kind::Bezier)
        {
            out.handleL[i] = move(out.handleL[i]);
            out.handleR[i] = move(out.handleR[i]);
        }
    }
    if (out.kind == Kind::Bezier)
        updateHandles(out);
    return out;
}

Spline smoothCurve(const Spline& s, const SmoothCurveOptions& o, std::span<const uint8_t> mask)
{
    LILA_ZONE();

    Spline out = s;
    out.normalize();
    size_t n = out.size();
    if (n < 3 || o.strength <= 0.0)
        return out;
    double k = std::clamp(o.strength, 0.0, 1.0);
    for (int it = 0; it < std::max(1, o.iterations); ++it)
    {
        std::vector<D3> was = out.points;
        for (size_t i = 0; i < n; ++i)
        {
            if (!masked(mask, i))
                continue;
            bool end = !out.closed && (i == 0 || i + 1 == n);
            if (end && o.keepEnds)
                continue;
            D3 target;
            if (!end)
                target = (was[(i + n - 1) % n] + was[(i + 1) % n]) * 0.5;
            else
                target = i == 0 ? was[1] : was[n - 2];
            D3 delta = (target - was[i]) * k;
            out.points[i] += delta;
            if (out.kind == Kind::Bezier)
            {
                out.handleL[i] += delta;
                out.handleR[i] += delta;
            }
        }
    }
    if (out.kind == Kind::Bezier)
        updateHandles(out);
    return out;
}

Spline evenSpacing(const Spline& s, bool keepCorners, size_t count)
{
    LILA_ZONE();

    if (!isValid(s) || s.size() < 2)
        return s;
    Polyline pl = tessellate(s, fineTess());
    if (pl.size() < 2 || pl.length < 1e-9)
        return s;
    const size_t least = s.kind == Kind::Nurbs ? size_t(s.degree) + (s.closed ? 0 : 1) : s.closed ? 3 : 2;
    const size_t n = std::max(count ? count : s.size(), least);
    const double L = pl.length;

    if (s.kind == Kind::Nurbs)
    {
        int spans = s.closed ? int(n) : int(n) - s.degree;
        if (spans < 1)
            return s;
        std::vector<D3> pts = evenSamples(pl, std::max<size_t>(64, n * 16) + (s.closed ? 0 : 1));
        Spline r = fitNurbs(pts, s.degree, spans, s.closed);
        std::vector<double> params;
        for (size_t i = 0; i < r.size(); ++i)
            params.push_back(paramAt(s, pl, arcOf(pl, r.points[i])));
        r.tilt = resampleValues(s, s.tilt, 0.0, params);
        r.radius = resampleValues(s, s.radius, 1.0, params);
        r.normalize();
        return r;
    }

    // Fixed stations: the ends of an open curve and, with keepCorners, every kink; the rest spread over the runs between.
    struct Station
    {
        double arc;
        int kinkIndex; // polyline index of a kink, else -1
    };
    std::vector<Station> fixed;
    if (!s.closed)
        fixed.push_back({0.0, -1});
    if (keepCorners)
        for (size_t i = 0; i < pl.size(); ++i)
            if (pl.kink[i] && (s.closed || (i > 0 && i + 1 < pl.size())))
                fixed.push_back({pl.arcLength[i], int(i)});
    if (!s.closed)
        fixed.push_back({L, -1});
    if (fixed.size() > n)
        return count ? evenSpacing(s, keepCorners, fixed.size()) : s;
    std::vector<Station> stations;
    if (fixed.empty())
        for (size_t i = 0; i < n; ++i)
            stations.push_back({L * double(i) / double(n), -1});
    else
    {
        size_t runs = s.closed ? fixed.size() : fixed.size() - 1;
        std::vector<double> runLen(runs);
        for (size_t r = 0; r < runs; ++r)
            runLen[r] = (r + 1 < fixed.size() ? fixed[r + 1].arc : fixed[0].arc + L) - fixed[r].arc;
        size_t free = n - fixed.size();
        // Largest remainder: whole shares first, then the runs with the biggest leftovers.
        std::vector<size_t> share(runs);
        std::vector<std::pair<double, size_t>> rest;
        size_t given = 0;
        for (size_t r = 0; r < runs; ++r)
        {
            double want = double(free) * runLen[r] / L;
            share[r] = size_t(std::floor(want));
            given += share[r];
            rest.push_back({want - std::floor(want), r});
        }
        std::sort(rest.begin(), rest.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (size_t k = 0; given < free; ++k, ++given)
            ++share[rest[k % rest.size()].second];
        for (size_t r = 0; r < runs; ++r)
        {
            stations.push_back(fixed[r]);
            for (size_t j = 1; j <= share[r]; ++j)
                stations.push_back({fixed[r].arc + runLen[r] * double(j) / double(share[r] + 1), -1});
        }
        if (!s.closed)
            stations.push_back(fixed.back());
    }

    Spline out;
    out.kind = s.kind;
    out.closed = s.closed;
    std::vector<double> params;
    for (const Station& st : stations)
    {
        double arc = s.closed ? std::fmod(st.arc, L) : st.arc;
        out.points.push_back(st.kinkIndex >= 0 ? pl.points[size_t(st.kinkIndex)] : pl.pointAtLength(arc));
        params.push_back(st.kinkIndex >= 0 ? pl.params[size_t(st.kinkIndex)] : paramAt(s, pl, arc));
    }
    if (s.kind == Kind::Bezier)
    {
        size_t m = out.size();
        std::vector<D3> tIn(m), tOut(m);
        for (size_t i = 0; i < m; ++i)
        {
            if (stations[i].kinkIndex >= 0)
            {
                tIn[i] = pl.tangentIn[size_t(stations[i].kinkIndex)];
                tOut[i] = pl.tangentOut[size_t(stations[i].kinkIndex)];
                continue;
            }
            Sample e = evaluate(s, params[i]);
            double l = glm::length(e.d1);
            D3 t = l > kEps ? e.d1 / l : pl.tangent(std::min(pl.size() - 1, size_t(i)));
            tIn[i] = tOut[i] = t;
        }
        out.handleL.assign(m, D3(0.0));
        out.handleR.assign(m, D3(0.0));
        out.typeL.assign(m, HandleType::Aligned);
        out.typeR.assign(m, HandleType::Aligned);
        size_t segs = s.closed ? m : m - 1;
        for (size_t i = 0; i < segs; ++i)
        {
            size_t j = (i + 1) % m;
            double l = stations[j].arc - stations[i].arc;
            if (l <= 0.0)
                l += L;
            double theta = std::acos(std::clamp(glm::dot(tOut[i], tIn[j]), -1.0, 1.0));
            auto [ha, hb] = fitHandles(pl, stations[i].arc, l, out.points[i], out.points[j], tOut[i], tIn[j], handleLength(l, theta));
            out.handleR[i] = out.points[i] + tOut[i] * ha;
            out.handleL[j] = out.points[j] - tIn[j] * hb;
        }
        if (!s.closed)
        {
            out.handleL[0] = out.points[0] - (out.handleR[0] - out.points[0]);
            out.handleR[m - 1] = out.points[m - 1] + (out.points[m - 1] - out.handleL[m - 1]);
        }
        for (size_t i = 0; i < m; ++i)
            if (stations[i].kinkIndex >= 0)
                out.typeL[i] = out.typeR[i] = HandleType::Free;
    }
    out.tilt = resampleValues(s, s.tilt, 0.0, params);
    out.radius = resampleValues(s, s.radius, 1.0, params);
    out.cornerRadius = resampleValues(s, s.cornerRadius, 0.0, params);
    out.normalize();
    return out;
}

double shapeDeviation(const Spline& a, const Spline& b, int samplesPerSpan)
{
    double worst = 0.0;
    int spans = spanCount(a);
    int k = std::max(1, samplesPerSpan);
    for (int i = 0; i <= spans * k; ++i)
    {
        D3 p = pointAt(a, double(i) / k), at;
        closestParam(b, p, &at);
        worst = std::max(worst, glm::distance(p, at));
    }
    return worst;
}
} // namespace lila::curve
