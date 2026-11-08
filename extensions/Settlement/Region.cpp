// Settlement: where settlements go on a map, and many of them at once, joined by roads.

#include "SettlementCore.h"
#include "SettlementTerrain.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace settlement
{
namespace
{
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

    const double ring = std::max(spacing * 0.15, 2000.0);
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
            SiteFacts f;
            f.p = q;
            const double flat = std::clamp(1.0 - grade / std::max(o.maxGrade, 1e-3), 0.0, 1.0);
            f.set(SiteTerm::Flat, flat, std::format("mean grade {:.1f}%", grade * 100.0));
            f.edge = std::clamp(edgeDistance(outline, q) / std::max(spacing * 0.25, 1.0), 0.0, 1.0);
            json legacy = {{"flat", flat}, {"gradePercent", grade * 100.0}, {"edge", f.edge}};
            if (!ground.isPlane())
            {
                // High ground over its surroundings is easier to hold.
                double around = 0.0;
                for (int k = 0; k < 8; ++k)
                {
                    const double t = 2.0 * kPi * k / 8.0;
                    around += ground.height(q + Vec2(std::cos(t), std::sin(t)) * ring);
                }
                const double rise = ground.height(q) - around / 8.0;
                f.set(SiteTerm::Defense, std::clamp(rise / (0.08 * ring), 0.0, 1.0), std::format("{:.0f} m above the ground around", rise / 100.0));
            }
            if (!water.empty())
            {
                const double bank = bankDistance(q, water);
                double near = 0.0;
                if (bank >= o.floodMargin)
                {
                    const double d = (bank - o.floodMargin) / std::max(o.waterReach * 0.5, 1.0);
                    near = std::exp(-d * d);
                }
                double join = 0.0, joinAt = std::numeric_limits<double>::max();
                for (const Vec2& j : joins)
                {
                    const double d = glm::length(j - q) / std::max(o.waterReach * 0.5, 1.0);
                    join = std::max(join, std::exp(-d * d));
                    joinAt = std::min(joinAt, glm::length(j - q));
                }
                const bool flooded = bank < o.floodMargin;
                f.set(SiteTerm::Water, near, flooded ? std::format("{:.0f} m from the bank, inside the flood margin", bank / 100.0)
                                                     : std::format("river bank {:.0f} m away", bank / 100.0));
                f.set(SiteTerm::Trade, join, joins.empty() ? std::string("no confluence on the map") : std::format("confluence {:.0f} m away", joinAt / 100.0));
                f.set(SiteTerm::Flood, flooded ? 1.0 : 0.0, flooded ? "floodplain" : "out of the flood margin");
                legacy["bankDistanceM"] = bank / 100.0;
                legacy["water"] = near;
                legacy["confluence"] = join;
                legacy["floodplain"] = flooded;
            }
            SiteScore s = scoreSite(f, o.rules, 1e-3 * unitRandom(hash64(o.seed) ^ hash64(uint64_t(all.size()))));
            for (auto& [k, v] : legacy.items())
                s.reasons[k] = v;
            all.push_back(std::move(s));
        }
    out = pickSites(std::move(all), count, spacing);
    return out;
}

std::vector<Vec2> roadBridges(std::span<const std::vector<Vec2>> roads, std::span<const WaterLine> water)
{
    std::vector<Vec2> out;
    for (const std::vector<Vec2>& r : roads)
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
                        out.push_back(p + e * t);
                }
    return out;
}

RegionPlan planRegion(const Polygon& outline, const Ground& ground, std::span<const WaterLine> water, const RegionOptions& o,
                      const std::vector<SiteScore>* given)
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
    const std::vector<SiteScore> candidates = given ? *given : suggestSites(outline, ground, water, so);
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
    plan.bridges = roadBridges(plan.roads, water);

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
