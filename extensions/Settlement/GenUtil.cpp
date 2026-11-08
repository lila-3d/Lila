// Settlement generators: shared helpers (clipping, laying lines, eras on new edges) and the generators JSON.

#include "Generators.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace settlement
{
namespace gen
{
Rng::Rng(uint32_t seed, uint64_t stream) : base(hash64(hash64(seed) ^ (stream * 0x9E3779B97F4A7C15ull))) {}

double Rng::next() { return unitRandom(base + n++); }

Vec2 rotate(Vec2 d, double radians)
{
    const double c = std::cos(radians), s = std::sin(radians);
    return {d.x * c - d.y * s, d.x * s + d.y * c};
}

double length(std::span<const Vec2> pts)
{
    double len = 0.0;
    for (size_t k = 0; k + 1 < pts.size(); ++k)
        len += glm::length(pts[k + 1] - pts[k]);
    return len;
}

namespace
{
void circleHits(Vec2 a, Vec2 b, Vec2 c, double r, std::vector<double>& out)
{
    if (r <= 0.0)
        return;
    const Vec2 d = b - a, f = a - c;
    const double A = glm::dot(d, d), B = 2.0 * glm::dot(f, d), C = glm::dot(f, f) - r * r;
    const double disc = B * B - 4 * A * C;
    if (A < 1e-12 || disc < 0.0)
        return;
    const double s = std::sqrt(disc);
    for (double t : {(-B - s) / (2 * A), (-B + s) / (2 * A)})
        if (t > 0.0 && t < 1.0)
            out.push_back(t);
}

void polygonHits(Vec2 a, Vec2 b, std::span<const Vec2> poly, std::vector<double>& out)
{
    const size_t n = poly.size();
    for (size_t j = 0; j < n; ++j)
    {
        const Vec2 p = poly[j], q = poly[(j + 1) % n];
        const double den = cross(b - a, q - p);
        if (std::abs(den) <= 1e-9 * glm::length(b - a) * glm::length(q - p))
            continue;
        const double t = cross(p - a, q - p) / den, u = cross(p - a, b - a) / den;
        if (t > 0.0 && t < 1.0 && u >= 0.0 && u <= 1.0)
            out.push_back(t);
    }
}
} // namespace

std::vector<std::vector<Vec2>> clip(std::span<const Vec2> pts, const Polygon& outline, Vec2 c, double inner, double outer,
                                    std::span<const Polygon> holes)
{
    std::vector<std::vector<Vec2>> runs;
    std::vector<Vec2> run;
    auto keep = [&](Vec2 q) {
        const double d = glm::length(q - c);
        if (!contains(outline.pts, q) || (inner > 0.0 && d < inner) || (outer > 0.0 && d > outer))
            return false;
        for (const Polygon& h : holes)
            if (contains(h.pts, q))
                return false;
        return true;
    };
    for (size_t k = 0; k + 1 < pts.size(); ++k)
    {
        const Vec2 a = pts[k], b = pts[k + 1];
        std::vector<double> ts = {0.0, 1.0};
        polygonHits(a, b, outline.pts, ts);
        for (const Polygon& h : holes)
            polygonHits(a, b, h.pts, ts);
        circleHits(a, b, c, inner, ts);
        circleHits(a, b, c, outer, ts);
        std::sort(ts.begin(), ts.end());
        for (size_t i = 0; i + 1 < ts.size(); ++i)
        {
            if (ts[i + 1] - ts[i] < 1e-9)
                continue;
            const Vec2 p0 = a + (b - a) * ts[i], p1 = a + (b - a) * ts[i + 1];
            if (keep((p0 + p1) * 0.5))
            {
                if (run.empty())
                    run.push_back(p0);
                run.push_back(p1);
            }
            else if (!run.empty())
            {
                if (run.size() >= 2)
                    runs.push_back(std::move(run));
                run.clear();
            }
        }
    }
    if (run.size() >= 2)
        runs.push_back(std::move(run));
    return runs;
}

std::vector<std::vector<Vec2>> climbable(const std::vector<Vec2>& run, const Ground& g, double maxGrade, double step)
{
    if (g.isPlane())
        return {run};
    std::vector<std::vector<Vec2>> out;
    std::vector<Vec2> cur;
    for (size_t k = 0; k + 1 < run.size(); ++k)
    {
        const Vec2 a = run[k], b = run[k + 1];
        const int n = std::max(1, int(glm::length(b - a) / step));
        for (int i = 0; i < n; ++i)
        {
            const Vec2 p = a + (b - a) * (double(i) / n), q = a + (b - a) * (double(i + 1) / n);
            if (g.gradeAlong(p, q) <= maxGrade)
            {
                if (cur.empty())
                    cur.push_back(p);
                cur.push_back(q);
            }
            else if (!cur.empty())
            {
                if (cur.size() >= 2)
                    out.push_back(std::move(cur));
                cur.clear();
            }
        }
    }
    if (cur.size() >= 2)
        out.push_back(std::move(cur));
    return out;
}

std::vector<std::vector<Vec2>> dry(const std::vector<Vec2>& run, std::span<const WaterLine> water)
{
    if (water.empty() || run.size() < 2)
        return {run};
    std::vector<std::pair<double, double>> gaps;
    double at = 0.0;
    for (size_t k = 0; k + 1 < run.size(); ++k)
    {
        const Vec2 a = run[k], b = run[k + 1];
        const double len = glm::length(b - a);
        for (const WaterLine& w : water)
            for (size_t j = 0; j + 1 < w.pts.size(); ++j)
            {
                const Vec2 c = w.pts[j], d = w.pts[j + 1];
                const double den = cross(b - a, d - c);
                if (std::abs(den) <= 1e-9 * len * glm::length(d - c))
                    continue;
                const double t = cross(c - a, d - c) / den, u = cross(c - a, b - a) / den;
                if (t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0)
                    gaps.push_back({at + t * len, 0.5 * w.width + 300.0});
            }
        at += len;
    }
    if (gaps.empty())
        return {run};
    std::vector<std::pair<double, double>> keep = {{0.0, at}};
    for (const auto& [c, half] : gaps)
    {
        std::vector<std::pair<double, double>> next;
        for (const auto& [lo, hi] : keep)
        {
            if (c - half > lo)
                next.push_back({lo, std::min(hi, c - half)});
            if (c + half < hi)
                next.push_back({std::max(lo, c + half), hi});
        }
        keep = std::move(next);
    }
    auto pointAt = [&](double d) {
        double acc = 0.0;
        for (size_t k = 0; k + 1 < run.size(); ++k)
        {
            const double len = glm::length(run[k + 1] - run[k]);
            if (acc + len >= d || k + 2 == run.size())
                return run[k] + (run[k + 1] - run[k]) * (len > 1e-12 ? std::clamp((d - acc) / len, 0.0, 1.0) : 0.0);
            acc += len;
        }
        return run.back();
    };
    std::vector<std::vector<Vec2>> out;
    for (const auto& [lo, hi] : keep)
    {
        if (hi - lo < 100.0)
            continue;
        std::vector<Vec2> piece = {pointAt(lo)};
        double acc = 0.0;
        for (size_t k = 0; k + 1 < run.size(); ++k)
        {
            acc += glm::length(run[k + 1] - run[k]);
            if (acc > lo && acc < hi)
                piece.push_back(run[k + 1]);
        }
        piece.push_back(pointAt(hi));
        out.push_back(std::move(piece));
    }
    return out;
}

std::vector<int> lay(Layout& l, const Ground& g, const Profile& p, std::span<const Vec2> pts, StreetClass cls, int era,
                     double inner, double outer, double width)
{
    std::vector<int> made;
    const double step = std::max(p.segment * 0.25, 200.0);
    for (const std::vector<Vec2>& run : clip(pts, l.outline, l.center, inner, outer, l.exclusions))
        for (const std::vector<Vec2>& part : climbable(run, g, p.maxGrade * 1.5, step))
        {
            if (cls == StreetClass::Arterial)
            {
                for (int e : l.graph.addPolyline(part, cls, era, false, width))
                    made.push_back(e);
                continue;
            }
            for (const std::vector<Vec2>& piece : dry(part, l.water))
                for (int e : l.graph.addPolyline(piece, cls, era, false, width))
                    made.push_back(e);
        }
    return made;
}

std::vector<Vec2> circle(Vec2 c, double r, int n)
{
    std::vector<Vec2> out;
    n = std::max(n, 8);
    for (int s = 0; s <= n; ++s)
    {
        const double t = 2.0 * kPi * s / n;
        out.push_back(c + Vec2(std::cos(t), std::sin(t)) * r);
    }
    return out;
}

std::vector<Vec2> smoothClosed(std::vector<Vec2> pts, int iterations)
{
    for (int it = 0; it < iterations && pts.size() > 2; ++it)
    {
        std::vector<Vec2> out;
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const Vec2 a = pts[k], b = pts[(k + 1) % pts.size()];
            out.push_back(a * 0.75 + b * 0.25);
            out.push_back(a * 0.25 + b * 0.75);
        }
        pts = std::move(out);
    }
    return pts;
}

Polygon intersectConvex(const Polygon& p, std::span<const Vec2> convex)
{
    Polygon out = p;
    const size_t n = convex.size();
    const double sign = signedArea(convex) >= 0.0 ? 1.0 : -1.0;
    for (size_t k = 0; k < n && !out.pts.empty(); ++k)
    {
        const Vec2 a = convex[k], b = convex[(k + 1) % n];
        if (glm::length(b - a) < 1e-9)
            continue;
        out = clipHalfPlane(out, a, leftNormal(b - a) * sign);
    }
    return out;
}

double lineDistance(std::span<const Vec2> line, Vec2 q, Vec2* tangent, bool closed)
{
    double best = std::numeric_limits<double>::max();
    const size_t n = line.size();
    const size_t segs = closed ? n : (n > 0 ? n - 1 : 0);
    for (size_t k = 0; k < segs; ++k)
    {
        const Vec2 a = line[k], d = line[(k + 1) % n] - a;
        const double len2 = glm::dot(d, d);
        if (len2 < 1e-12)
            continue;
        const double t = std::clamp(glm::dot(q - a, d) / len2, 0.0, 1.0);
        const double dist = glm::length(a + d * t - q);
        if (dist < best)
        {
            best = dist;
            if (tangent)
                *tangent = d / std::sqrt(len2);
        }
    }
    return best;
}

void crossings(std::span<const Vec2> ring, Vec2 p, Vec2 q, std::vector<std::pair<double, double>>& out)
{
    double at = 0.0;
    const size_t n = ring.size();
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2 a = ring[k], b = ring[(k + 1) % n];
        const double len = glm::length(b - a);
        const double den = cross(q - p, b - a);
        if (std::abs(den) > 1e-9 * glm::length(q - p) * len)
        {
            const double t = cross(a - p, b - a) / den, u = cross(a - p, q - p) / den;
            if (t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0)
                out.push_back({t, at + u * len});
        }
        at += len;
    }
}

void withEra(Layout& l, int era, const std::function<void()>& f)
{
    // Older edges are shifted out of the way, so pieces split off them stay recognisable by their era.
    constexpr int kOld = 1 << 20;
    for (StreetEdge& e : l.graph.edges)
        e.era += kOld;
    f();
    for (StreetEdge& e : l.graph.edges)
        e.era = e.era >= kOld / 2 ? e.era - kOld : era;
}

void closeEnds(Layout& l, const Ground& g, const Profile& p, std::span<const int> edges, double reach)
{
    for (int e : edges)
    {
        if (e >= int(l.graph.edges.size()) || !l.graph.isStreet(e))
            continue;
        for (int end : {l.graph.edges[size_t(e)].a, l.graph.edges[size_t(e)].b})
        {
            if (l.graph.degree(end) != 1 || !l.graph.isStreet(e))
                continue;
            const StreetEdge x = l.graph.edges[size_t(e)];
            const Vec2 at = l.graph.nodes[size_t(end)].p;
            const Vec2 dir = at - l.graph.nodes[size_t(x.a == end ? x.b : x.a)].p;
            if (glm::length(dir) < 1e-6)
                continue;
            const Vec2 u = glm::normalize(dir);
            Vec2 hit{0.0};
            double s = 0.0;
            const int crossed = l.graph.firstCrossing(at + u * 1.0, at + u * reach, &hit, &s, end);
            if (crossed < 0 || l.graph.edges[size_t(crossed)].cls == StreetClass::Water || crossesExclusion(l, at, hit) ||
                !contains(l.outline.pts, (at + hit) * 0.5) || g.gradeAlong(at, hit) > p.maxGrade * 1.5)
                continue;
            l.graph.addStreet(at, hit, x);
        }
    }
}

Profile resolveProfile(const Profile& base, const std::string& name, const json& overrides)
{
    Profile p = base;
    if (!name.empty() && !builtinProfile(name, &p))
        throw std::runtime_error("unknown profile '" + name + "'");
    if (overrides.is_object() && !overrides.empty())
        applyJson(p, overrides);
    return p;
}
} // namespace gen
} // namespace settlement
