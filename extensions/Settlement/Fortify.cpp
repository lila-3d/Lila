// Settlement: fortifications. A wall round the core along defensible ground (high ground and breaks of slope) or a
// given line, or a bastioned star trace; gates where main roads cross it, towers by spacing and at corners, a moat
// beyond it as water. Walls close blocks like the outline does; streets other than main roads end at them.

#include "Generators.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace settlement
{
namespace
{
/** q pulled towards c until it is inside the outline and out of every exclusion. */
Vec2 pullInside(const Layout& l, Vec2 c, Vec2 q)
{
    for (int i = 0; i < 40; ++i)
    {
        if (contains(l.outline.pts, q) && !excluded(l, q))
            return q;
        q = c + (q - c) * 0.95;
    }
    return q;
}

/** Arc length along the closed line to the point on it nearest q. */
double arcAt(std::span<const Vec2> ring, Vec2 q, Vec2* tangent)
{
    double best = std::numeric_limits<double>::max(), at = 0.0, acc = 0.0;
    const size_t n = ring.size();
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2 a = ring[k], d = ring[(k + 1) % n] - a;
        const double len = glm::length(d);
        if (len < 1e-9)
            continue;
        const double t = std::clamp(glm::dot(q - a, d) / (len * len), 0.0, 1.0);
        const double dist = glm::length(a + d * t - q);
        if (dist < best)
        {
            best = dist;
            at = acc + t * len;
            if (tangent)
                *tangent = d / len;
        }
        acc += len;
    }
    return at;
}

double perimeter(std::span<const Vec2> ring)
{
    double len = 0.0;
    for (size_t k = 0; k < ring.size(); ++k)
        len += glm::length(ring[(k + 1) % ring.size()] - ring[k]);
    return len;
}

Vec2 pointAlong(std::span<const Vec2> ring, double s, Vec2* tangent)
{
    const size_t n = ring.size();
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2 a = ring[k], d = ring[(k + 1) % n] - a;
        const double len = glm::length(d);
        if (s <= len || k + 1 == n)
        {
            if (tangent && len > 1e-9)
                *tangent = d / len;
            return a + d * (len > 1e-9 ? std::clamp(s / len, 0.0, 1.0) : 0.0);
        }
        s -= len;
    }
    return ring.empty() ? Vec2(0.0) : ring.front();
}
} // namespace

std::vector<Vec2> starTrace(Vec2 c, double r, int n, double rotation)
{
    n = std::max(n, 3);
    std::vector<Vec2> v;
    for (int k = 0; k < n; ++k)
    {
        const double t = rotation + 2.0 * kPi * k / n;
        v.push_back(c + Vec2(std::cos(t), std::sin(t)) * r);
    }
    std::vector<Vec2> out;
    for (int k = 0; k < n; ++k)
    {
        const Vec2 corner = v[size_t(k)], prev = v[size_t((k + n - 1) % n)], next = v[size_t((k + 1) % n)];
        // Each corner becomes a bastion: two faces meeting at a point thrust out past the curtain walls.
        out.push_back(corner + (prev - corner) * 0.22);
        out.push_back(c + (corner - c) * 1.25);
        out.push_back(corner + (next - corner) * 0.22);
    }
    return out;
}

std::vector<Vec2> wallLine(const Layout& l, const Ground& g, const WallSpec& spec, double r)
{
    LILA_ZONE();
    constexpr int kRays = 64;
    std::vector<double> radii(kRays, r);
    if (!g.isPlane() && spec.contour)
    {
        const double drop = std::max(g.cellSize() * 2.0, r * 0.05);
        const double h0 = g.height(l.center);
        for (int k = 0; k < kRays; ++k)
        {
            const double a = 2.0 * kPi * k / kRays;
            const Vec2 d(std::cos(a), std::sin(a));
            double best = -std::numeric_limits<double>::max();
            for (double f = 0.7; f <= 1.3001; f += 0.025)
            {
                const Vec2 q = l.center + d * (r * f);
                if (!g.solid(q))
                    continue;
                // Defensible: the ground falls away outside the wall, and the wall stands high.
                const double fall = (g.height(q) - g.height(q + d * drop)) / drop;
                const double score = 10.0 * fall + 2.0 * (g.height(q) - h0) / std::max(r, 1.0) - 1.5 * std::abs(f - 1.0);
                if (score > best)
                {
                    best = score;
                    radii[size_t(k)] = r * f;
                }
            }
        }
        for (int pass = 0; pass < 3; ++pass)
        {
            std::vector<double> next(radii.size());
            for (int k = 0; k < kRays; ++k)
                next[size_t(k)] = 0.25 * radii[size_t((k + kRays - 1) % kRays)] + 0.5 * radii[size_t(k)] + 0.25 * radii[size_t((k + 1) % kRays)];
            radii = std::move(next);
        }
    }
    std::vector<Vec2> out;
    const int n = std::max(16, std::min(kRays, int(2.0 * kPi * r / 1500.0)));
    for (int k = 0; k < n; ++k)
    {
        const double a = 2.0 * kPi * k / n;
        const double fk = double(k) * kRays / n;
        const int k0 = int(fk) % kRays, k1 = (k0 + 1) % kRays;
        const double rr = radii[size_t(k0)] + (radii[size_t(k1)] - radii[size_t(k0)]) * (fk - std::floor(fk));
        out.push_back(pullInside(l, l.center, l.center + Vec2(std::cos(a), std::sin(a)) * rr));
    }
    return out;
}

void layWalls(Layout& l, const Ground& g, const Profile& p, const WallSpec& spec, int era, double radius)
{
    LILA_ZONE();

    if (!l.gen)
        l.gen = std::make_shared<GenLayout>();
    GenLayout& gen = *l.gen;
    std::vector<Vec2> pts;
    if (!spec.polygon.empty())
        pts = spec.polygon;
    else if (spec.starFort)
    {
        const int n = spec.bastions > 0 ? spec.bastions : 5;
        for (const Vec2& q : starTrace(l.center, radius, n, 0.5 * kPi))
            pts.push_back(pullInside(l, l.center, q));
    }
    else
        pts = wallLine(l, g, spec, radius);
    Polygon poly = makePolygon(pts);
    if (poly.pts.size() < 3)
    {
        gen.warnings.push_back("walls: the wall line has no area; no wall laid");
        return;
    }
    l.graph.addPolyline(poly.pts, StreetClass::Boundary, era, true, spec.thickness);
    gen.walls.push_back({poly.pts, era, -1, spec.thickness, spec.height, spec.starFort, spec.towerSpacing});
    if (spec.moat > 0.0)
    {
        Polygon outer = grow(poly, spec.moatGap + 0.5 * (spec.thickness + spec.moat));
        if (outer.pts.size() >= 3)
        {
            std::vector<Vec2> ring = outer.pts;
            ring.push_back(ring.front());
            const size_t before = l.graph.edges.size();
            for (const std::vector<Vec2>& run : gen::clip(ring, l.outline, l.center, 0.0, 0.0))
            {
                l.graph.addPolyline(run, StreetClass::Water, era, false, spec.moat);
                l.water.push_back({run, spec.moat, "moat"});
            }
            for (size_t e = before; e < l.graph.edges.size(); ++e)
                if (l.graph.edges[e].cls == StreetClass::Water)
                    l.graph.edges[e].moat = true;
        }
        else
            gen.warnings.push_back("walls: no room for the moat outside the wall; left out");
    }
    (void)p;
}

void finishWalls(Layout& l, const Ground& g)
{
    LILA_ZONE();

    GenLayout& gen = *l.gen;
    StreetGraph& graph = l.graph;
    gen.gates.clear();
    gen.towers.clear();
    for (size_t wi = 0; wi < gen.walls.size(); ++wi)
    {
        const WallLine& w = gen.walls[wi];
        if (w.demolished >= 0)
            continue;
        const double tol = graph.snap() + 1.0;
        std::vector<double> gateArcs;
        for (size_t n = 0; n < graph.nodes.size(); ++n)
        {
            if (graph.degree(int(n)) == 0)
                continue;
            const Vec2 at = graph.nodes[n].p;
            if (gen::lineDistance(w.pts, at, nullptr, true) > tol)
                continue;
            std::vector<int> inside, outside;
            for (int e : graph.incident(int(n)))
            {
                if (!graph.isStreet(e))
                    continue;
                const StreetEdge& x = graph.edges[size_t(e)];
                const Vec2 far = graph.nodes[size_t(x.a == int(n) ? x.b : x.a)].p;
                (contains(w.pts, far) ? inside : outside).push_back(e);
            }
            if (inside.empty() || outside.empty())
                continue;
            int main = -1;
            for (int e : inside)
                if (main < 0 || graph.edges[size_t(e)].cls < graph.edges[size_t(main)].cls)
                    main = e;
            for (int e : outside)
                if (graph.edges[size_t(e)].cls < graph.edges[size_t(main)].cls)
                    main = e;
            if (graph.edges[size_t(main)].cls <= StreetClass::Collector)
            {
                Vec2 tan(1.0, 0.0);
                Gate gate;
                gate.p = at;
                gate.wall = int(wi);
                gate.at = arcAt(w.pts, at, &tan);
                gate.cls = graph.edges[size_t(main)].cls;
                for (int e : inside)
                    gate.width = std::max(gate.width, edgeWidth(l, e));
                for (int e : outside)
                    gate.width = std::max(gate.width, edgeWidth(l, e));
                const Vec2 out = at - l.center;
                gate.dir = glm::length(out) > 1e-9 ? glm::normalize(out) : Vec2(1.0, 0.0);
                gen.gates.push_back(gate);
                gateArcs.push_back(gate.at);
                for (double side : {-1.0, 1.0})
                    gen.towers.push_back({at + tan * (side * (0.5 * gate.width + w.thickness)), int(wi), true, false});
            }
            else
                for (int e : outside)
                    graph.removeEdge(e);
        }
        const double total = perimeter(w.pts);
        auto nearGate = [&](double s, double r) {
            for (double a : gateArcs)
                if (std::min(std::abs(a - s), total - std::abs(a - s)) < r)
                    return true;
            return false;
        };
        const double clearOfGates = 2.0 * w.thickness + 400.0;
        // Corners first (bastion points on a star trace), then towers between them by spacing.
        std::vector<double> placed;
        double acc = 0.0;
        const size_t n = w.pts.size();
        for (size_t k = 0; k < n; ++k)
        {
            const Vec2 prev = w.pts[(k + n - 1) % n], cur = w.pts[k], next = w.pts[(k + 1) % n];
            const Vec2 a = cur - prev, b = next - cur;
            if (glm::length(a) > 1e-9 && glm::length(b) > 1e-9)
            {
                const double turn = std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0));
                if (turn > 35.0 * kPi / 180.0 && !nearGate(acc, clearOfGates))
                {
                    gen.towers.push_back({cur, int(wi), false, w.starFort && cross(a, b) > 0.0});
                    placed.push_back(acc);
                }
            }
            acc += glm::length(b);
        }
        const double spacing = w.towerSpacing;
        if (spacing > 0.0)
        {
            const int count = int(std::floor(total / spacing));
            for (int i = 0; i < count; ++i)
            {
                const double s = (i + 0.5) * total / std::max(count, 1);
                bool close = nearGate(s, std::max(clearOfGates, 0.3 * spacing));
                for (double q : placed)
                    close = close || std::min(std::abs(q - s), total - std::abs(q - s)) < 0.4 * spacing;
                if (close)
                    continue;
                gen.towers.push_back({pointAlong(w.pts, s, nullptr), int(wi), false, false});
            }
        }
    }
    (void)g;
}
} // namespace settlement
