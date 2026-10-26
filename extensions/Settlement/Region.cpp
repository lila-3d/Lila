// Settlement: where settlements go on a map, and many of them at once, joined by roads.

#include "SettlementCore.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace settlement
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

double segmentDistance(Vec2 q, Vec2 a, Vec2 b)
{
    const Vec2 d = b - a;
    const double len2 = glm::dot(d, d);
    const double t = len2 > 1e-12 ? std::clamp(glm::dot(q - a, d) / len2, 0.0, 1.0) : 0.0;
    return glm::length(a + d * t - q);
}

double edgeDistance(const Polygon& p, Vec2 q)
{
    double best = std::numeric_limits<double>::max();
    for (size_t k = 0; k < p.size(); ++k)
        best = std::min(best, segmentDistance(q, p.pts[k], p.pts[(k + 1) % p.size()]));
    return best;
}

/** Where rivers meet: an end of one line close to another line. */
std::vector<Vec2> confluences(std::span<const WaterLine> water)
{
    std::vector<Vec2> out;
    for (size_t i = 0; i < water.size(); ++i)
        for (const Vec2& end : {water[i].pts.front(), water[i].pts.back()})
            for (size_t j = 0; j < water.size(); ++j)
            {
                if (i == j)
                    continue;
                for (size_t k = 0; k + 1 < water[j].pts.size(); ++k)
                    if (segmentDistance(end, water[j].pts[k], water[j].pts[k + 1]) < std::max(water[i].width, water[j].width) * 2.0)
                    {
                        out.push_back(end);
                        k = water[j].pts.size();
                    }
            }
    return out;
}

std::vector<Vec2> circle(Vec2 c, double r, int n = 24)
{
    std::vector<Vec2> pts;
    for (int k = 0; k < n; ++k)
    {
        const double t = 2.0 * kPi * k / n;
        pts.push_back(c + Vec2(std::cos(t), std::sin(t)) * r);
    }
    return pts;
}
} // namespace

const char* tierName(Tier t)
{
    switch (t)
    {
    case Tier::City: return "city";
    case Tier::Town: return "town";
    case Tier::Village: return "village";
    }
    return "village";
}

std::vector<SiteScore> suggestSites(const Polygon& outline, const Ground& ground, std::span<const WaterLine> water, const SiteOptions& o)
{
    LILA_ZONE();

    std::vector<SiteScore> out;
    if (outline.pts.size() < 3)
        return out;
    const double a = area(outline);
    const int count = std::max(1, o.count);
    const double spacing = o.spacing > 0.0 ? o.spacing : std::sqrt(a / count) * 0.8;
    Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const Vec2& q : outline.pts)
    {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    const double cell = std::max({spacing / 8.0, (hi.x - lo.x) / 200.0, (hi.y - lo.y) / 200.0, 1000.0});
    const std::vector<Vec2> joins = confluences(water);
    const double probe = std::max(spacing * 0.08, 500.0);

    std::vector<SiteScore> all;
    for (double y = lo.y + cell * 0.5; y < hi.y; y += cell)
        for (double x = lo.x + cell * 0.5; x < hi.x; x += cell)
        {
            const Vec2 q(x, y);
            if (!contains(outline.pts, q) || !ground.solid(q))
                continue;
            double grade = ground.grade(q);
            for (int k = 0; k < 8; ++k)
            {
                const double t = 2.0 * kPi * k / 8.0;
                grade += ground.grade(q + Vec2(std::cos(t), std::sin(t)) * probe);
            }
            grade /= 9.0;
            const double flat = std::clamp(1.0 - grade / std::max(o.maxGrade, 1e-3), 0.0, 1.0);
            const double edge = std::clamp(edgeDistance(outline, q) / std::max(spacing * 0.25, 1.0), 0.0, 1.0);
            SiteScore s;
            s.p = q;
            s.reasons = {{"flat", flat}, {"gradePercent", grade * 100.0}, {"edge", edge}};
            double score = flat;
            if (!water.empty())
            {
                const double bank = bankDistance(q, water);
                double near = 0.0;
                if (bank >= o.floodMargin)
                {
                    const double d = (bank - o.floodMargin) / std::max(o.waterReach * 0.5, 1.0);
                    near = std::exp(-d * d);
                }
                double join = 0.0;
                for (const Vec2& j : joins)
                {
                    const double d = glm::length(j - q) / std::max(o.waterReach * 0.5, 1.0);
                    join = std::max(join, std::exp(-d * d));
                }
                score = 0.45 * flat + 0.4 * near + 0.15 * join;
                // Flooded ground keeps houses off the centre, whatever else it has.
                if (bank < o.floodMargin)
                    score *= 0.2;
                s.reasons["bankDistanceM"] = bank / 100.0;
                s.reasons["water"] = near;
                s.reasons["confluence"] = join;
                s.reasons["floodplain"] = bank < o.floodMargin;
            }
            s.score = score * edge + 1e-3 * unitRandom(hash64(o.seed) ^ hash64(uint64_t(all.size())));
            all.push_back(std::move(s));
        }
    std::sort(all.begin(), all.end(), [](const SiteScore& x, const SiteScore& y) { return x.score > y.score; });
    for (const SiteScore& s : all)
    {
        bool clear = true;
        for (const SiteScore& c : out)
            clear = clear && glm::length(c.p - s.p) >= spacing;
        if (clear)
            out.push_back(s);
        if (int(out.size()) >= count)
            break;
    }
    return out;
}

RegionPlan planRegion(const Polygon& outline, const Ground& ground, std::span<const WaterLine> water, const RegionOptions& o)
{
    LILA_ZONE();

    RegionPlan plan;
    const int total = std::max(1, o.cities + o.towns + o.villages);
    const double base = std::sqrt(area(outline) / total);
    struct TierSpec
    {
        Tier tier;
        int count;
        double radius, spacing;
        std::string profile;
    };
    // Central places: cities far apart on the best sites, towns between them, villages filling in.
    const TierSpec tiers[] = {{Tier::City, o.cities, o.cityRadius, base * 1.5, o.cityProfile},
                              {Tier::Town, o.towns, o.townRadius, base * 0.9, o.townProfile},
                              {Tier::Village, o.villages, o.villageRadius, base * 0.45, o.villageProfile}};
    SiteOptions so;
    so.count = total * 12;
    so.spacing = base * 0.3;
    so.seed = o.seed;
    const std::vector<SiteScore> candidates = suggestSites(outline, ground, water, so);
    for (const TierSpec& t : tiers)
    {
        const double radius = std::min(t.radius, t.spacing * 0.45);
        int placed = 0;
        for (const SiteScore& c : candidates)
        {
            if (placed >= t.count)
                break;
            // Room for the whole settlement inside the map.
            if (edgeDistance(outline, c.p) < radius * 0.8)
                continue;
            bool clear = true;
            for (const RegionSettlement& s : plan.settlements)
                clear = clear && glm::length(s.score.p - c.p) >= std::max(t.spacing, s.radius + radius + 5000.0);
            if (!clear)
                continue;
            RegionSettlement s;
            s.tier = t.tier;
            s.profile = t.profile;
            s.radius = radius;
            s.score = c;
            plan.settlements.push_back(std::move(s));
            ++placed;
        }
    }

    // Roads: the relative neighbourhood graph, each edge routed over the ground and across water at a cost.
    Layout map;
    map.outline = outline;
    map.center = centroid(outline);
    map.water.assign(water.begin(), water.end());
    Profile roadProfile;
    roadProfile.widthArterial = std::max(1000.0, base / 200.0);
    roadProfile.maxGrade = 0.1;
    const size_t n = plan.settlements.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
        {
            const Vec2 a = plan.settlements[i].score.p, b = plan.settlements[j].score.p;
            const double dij = glm::length(b - a);
            bool keep = true;
            for (size_t k = 0; k < n && keep; ++k)
                if (k != i && k != j)
                {
                    const Vec2 c = plan.settlements[k].score.p;
                    keep = std::max(glm::length(c - a), glm::length(c - b)) >= dij;
                }
            if (!keep)
                continue;
            std::vector<Vec2> path = route(map, ground, roadProfile, a, b);
            if (path.empty())
                path = {a, b};
            plan.roads.push_back(std::move(path));
        }
    for (const std::vector<Vec2>& r : plan.roads)
        for (size_t k = 0; k + 1 < r.size(); ++k)
            for (const WaterLine& w : water)
                for (size_t m = 0; m + 1 < w.pts.size(); ++m)
                {
                    const Vec2 p = r[k], e = r[k + 1] - p, c = w.pts[m], f = w.pts[m + 1] - c;
                    const double den = cross(e, f);
                    if (std::abs(den) <= 1e-9 * glm::length(e) * glm::length(f))
                        continue;
                    const double t = cross(c - p, f) / den, u = cross(c - p, e) / den;
                    if (t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0)
                        plan.bridges.push_back(p + e * t);
                }

    for (RegionSettlement& s : plan.settlements)
    {
        double r = s.radius;
        std::vector<Vec2> ring = circle(s.score.p, r);
        for (int shrink = 0; shrink < 12; ++shrink)
        {
            bool inside = true;
            for (const Vec2& q : ring)
                inside = inside && contains(outline.pts, q);
            if (inside)
                break;
            r *= 0.85;
            ring = circle(s.score.p, r);
        }
        s.radius = r;
        s.site.outline = std::move(ring);
        s.site.attractors = {s.score.p};
        s.site.hasCenter = true;
        s.site.roads = plan.roads;
        s.site.water.assign(water.begin(), water.end());
    }
    return plan;
}
} // namespace settlement
