// Settlement: networks above the ground. Decks (flat levels with their own streets), elevated roads, ramps between
// levels and the ground, skybridges between tall buildings, megablock footprints and air-traffic lane layers. Each level
// is its own planar street graph, so streets on different levels never split each other; every edge carries its layer.

#include "Generators.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace settlement
{
namespace
{
double highestUnder(const Ground& g, const Polygon& poly)
{
    Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const Vec2& q : poly.pts)
    {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    double top = -std::numeric_limits<double>::max();
    for (int i = 0; i <= 8; ++i)
        for (int j = 0; j <= 8; ++j)
        {
            const Vec2 q = lo + (hi - lo) * Vec2(i / 8.0, j / 8.0);
            if (contains(poly.pts, q) || i % 8 == 0 || j % 8 == 0)
                top = std::max(top, g.height(q));
        }
    return top;
}

/** A ramp from top (on a level at height) out along dir down to the ground, joined to the ground's streets at its foot. */
void addRamp(Layout& l, const Ground& g, Vec2 top, double height, Vec2 dir, int layer, StreetClass cls)
{
    const double grade = std::clamp(l.profile.maxGrade, 0.03, 0.08);
    const double rise = std::max(0.0, height - g.height(top));
    const double run = std::max(rise / grade, 500.0);
    // Straight on where it lands inside the site, else turned aside, else back the way it came.
    Vec2 foot = top + dir * run;
    for (Vec2 d : {dir, leftNormal(dir), -leftNormal(dir), -dir})
        if (contains(l.outline.pts, top + d * run))
        {
            dir = d;
            foot = top + d * run;
            break;
        }
    Ramp r;
    r.from = 0;
    r.to = layer;
    r.cls = cls;
    for (int s = 0; s <= 8; ++s)
    {
        const Vec2 q = top + (foot - top) * (s / 8.0);
        const double f = s / 8.0;
        r.pts.push_back({q.x, height + (g.height(foot) - height) * f, q.y});
    }
    l.gen->ramps.push_back(std::move(r));
    Vec2 at{0.0};
    if (const int e = l.graph.nearestEdge(foot, 3.0 * run, &at); e >= 0 && l.graph.isStreet(e))
    {
        if (glm::length(at - foot) > 1.0)
            l.graph.addStreet(foot, at, cls);
        else
            l.graph.addStreet(foot, foot + (foot - top) / std::max(glm::length(foot - top), 1e-9) * l.graph.snap(), cls);
    }
    else
        l.gen->warnings.push_back("levels: a ramp's foot has no ground street within reach; it ends unconnected");
}

void layAir(Layout& l, const Ground& g, const LevelsSpec& spec)
{
    LILA_ZONE();
    AirLanes& air = l.gen->air;
    Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const Vec2& q : l.outline.pts)
    {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    const double top = highestUnder(g, l.outline);
    const int nx = std::max(1, int((hi.x - lo.x) / spec.airSpacing)), nz = std::max(1, int((hi.y - lo.y) / spec.airSpacing));
    std::vector<std::vector<int>> grids;
    for (double h : spec.airHeights)
    {
        air.heights.push_back(top + h);
        std::vector<int> ids(size_t(nx + 1) * size_t(nz + 1), -1);
        for (int i = 0; i <= nx; ++i)
            for (int j = 0; j <= nz; ++j)
            {
                const Vec2 q = lo + Vec2((hi.x - lo.x) * i / nx, (hi.y - lo.y) * j / nz);
                if (!contains(l.outline.pts, q))
                    continue;
                ids[size_t(i) * size_t(nz + 1) + size_t(j)] = int(air.nodes.size());
                air.nodes.push_back({q.x, top + h, q.y});
            }
        // Lanes both ways along every row and column.
        for (int i = 0; i <= nx; ++i)
            for (int j = 0; j <= nz; ++j)
            {
                const int a = ids[size_t(i) * size_t(nz + 1) + size_t(j)];
                if (a < 0)
                    continue;
                if (i < nx)
                    if (const int b = ids[size_t(i + 1) * size_t(nz + 1) + size_t(j)]; b >= 0)
                    {
                        air.links.push_back({a, b});
                        air.links.push_back({b, a});
                    }
                if (j < nz)
                    if (const int b = ids[size_t(i) * size_t(nz + 1) + size_t(j + 1)]; b >= 0)
                    {
                        air.links.push_back({a, b});
                        air.links.push_back({b, a});
                    }
            }
        grids.push_back(std::move(ids));
    }
    // Climbs and descents between altitudes at every third crossing.
    for (size_t k = 0; k + 1 < grids.size(); ++k)
        for (size_t c = 0; c < grids[k].size(); ++c)
        {
            const int i = int(c / size_t(nz + 1)), j = int(c % size_t(nz + 1));
            if ((i + j) % 3 != 0 || grids[k][c] < 0 || grids[k + 1][c] < 0)
                continue;
            air.links.push_back({grids[k][c], grids[k + 1][c]});
            air.links.push_back({grids[k + 1][c], grids[k][c]});
        }
}
} // namespace

void layLevels(Layout& l, const Ground& g, const Options& o, const LevelsSpec& spec)
{
    LILA_ZONE();

    GenLayout& gen = *l.gen;
    for (const DeckSpec& d : spec.decks)
    {
        Polygon poly = makePolygon(d.polygon);
        if (poly.pts.size() < 3)
        {
            gen.warnings.push_back("levels: deck " + d.name + " has no area; left out");
            continue;
        }
        const int layer = int(gen.levels.size()) + 1;
        Level lv;
        lv.name = d.name.empty() ? "deck" + std::to_string(layer) : d.name;
        lv.kind = "deck";
        lv.height = highestUnder(g, poly) + d.height;
        lv.outline = poly;
        const Ground deck = Ground::plane(lv.height);
        Layout dl{StreetGraph(l.graph.snap(), std::max(o.profile.segment, 1000.0))};
        dl.profile = l.profile;
        dl.roads = l.roads;
        dl.outline = poly;
        dl.center = centroid(poly);
        for (const Vec2& q : poly.pts)
            dl.radius = std::max(dl.radius, glm::length(q - dl.center));
        dl.graph.addPolyline(poly.pts, StreetClass::Boundary, 0, true);
        const Pattern pattern = d.pattern ? *d.pattern : Pattern::Grid;
        if (pattern == Pattern::Radial)
            layRadial(dl, deck, o);
        else if (pattern == Pattern::Tensor)
            layTensor(dl, deck, o, TensorSpec{});
        else if (pattern != Pattern::None)
            layGrid(dl, deck, o, 0.0);
        lv.graph = std::move(dl.graph);
        for (StreetEdge& e : lv.graph.edges)
            e.layer = layer;
        gen.levels.push_back(std::move(lv));
        // Ramps leave from the deck's edge, spread round it, towards the ground streets beyond.
        const Level& made = gen.levels.back();
        for (int k = 0; k < d.ramps; ++k)
        {
            const double t = 2.0 * kPi * (k + 0.25) / d.ramps;
            const Vec2 dir(std::cos(t), std::sin(t));
            Vec2 edge = made.outline.pts.front();
            double best = -std::numeric_limits<double>::max();
            for (const Vec2& q : made.outline.pts)
                if (const double s = glm::dot(q - centroid(made.outline), dir); s > best)
                {
                    best = s;
                    edge = q;
                }
            addRamp(l, g, edge, made.height, dir, layer, StreetClass::Collector);
        }
    }
    for (const ElevatedSpec& e : spec.elevated)
    {
        const int layer = int(gen.levels.size()) + 1;
        Level lv;
        lv.name = "elevated" + std::to_string(layer);
        lv.kind = "elevated";
        lv.graph = StreetGraph(l.graph.snap(), std::max(o.profile.segment, 1000.0));
        std::vector<glm::dvec3> line;
        double sum = 0.0;
        for (const Vec2& q : e.pts)
        {
            line.push_back({q.x, g.height(q) + e.height, q.y});
            sum += line.back().y;
        }
        lv.height = sum / double(line.size());
        lv.graph.addPolyline(e.pts, e.cls, 0);
        for (StreetEdge& x : lv.graph.edges)
            x.layer = layer;
        lv.lines3.push_back(line);
        gen.levels.push_back(std::move(lv));
        if (e.ramps)
            for (bool start : {true, false})
            {
                const Vec2 end = start ? e.pts.front() : e.pts.back(), prev = start ? e.pts[1] : e.pts[e.pts.size() - 2];
                const Vec2 d = end - prev;
                if (glm::length(d) > 1e-9)
                    addRamp(l, g, end, g.height(end) + e.height, glm::normalize(d), layer, e.cls);
            }
    }
    if (!spec.airHeights.empty())
        layAir(l, g, spec);
}

void finishLevels(Layout& l, const Ground& g, const Options& o, const LevelsSpec& spec)
{
    LILA_ZONE();

    GenLayout& gen = *l.gen;
    gen.megablocks.clear();
    if (spec.megablock > 0.0)
        for (const Block& b : l.blocks)
        {
            if (b.area < spec.megablock || b.use == Use::Plaza)
                continue;
            const std::vector<double> d(b.outline.size(), std::max(l.profile.frontSetback, 0.0));
            Polygon fp = inset(b.outline, d);
            if (fp.pts.size() < 3)
                continue;
            double base = std::numeric_limits<double>::max();
            for (const Vec2& q : fp.pts)
                base = std::min(base, g.height(q));
            gen.megablocks.push_back({b.id, fp, base, base + l.profile.maxFloors * l.profile.floorHeight * std::max(o.heightScale, 0.0)});
        }
    if (spec.skybridgeSpan <= 0.0)
        return;
    struct Tall
    {
        Polygon fp;
        double ground, top;
    };
    std::vector<Tall> tall;
    const double minHeight = 6.0 * l.profile.floorHeight;
    for (const Envelope& e : l.envelopes)
        if (e.top - e.ground >= minHeight)
            tall.push_back({e.footprint, e.ground, e.top});
    for (const Megablock& m : gen.megablocks)
        tall.push_back({m.footprint, m.base, m.top});
    auto closest = [](const Polygon& a, const Polygon& b, Vec2* pa, Vec2* pb) {
        double best = std::numeric_limits<double>::max();
        for (const Vec2& p : a.pts)
            for (size_t k = 0; k < b.size(); ++k)
            {
                const Vec2 s = b.pts[k], d = b.pts[(k + 1) % b.size()] - s;
                const double t = std::clamp(glm::dot(p - s, d) / std::max(glm::dot(d, d), 1e-12), 0.0, 1.0);
                const double dist = glm::length(s + d * t - p);
                if (dist < best)
                {
                    best = dist;
                    *pa = p;
                    *pb = s + d * t;
                }
            }
        return best;
    };
    std::map<std::pair<int, int>, bool> joined;
    Level sky;
    sky.name = "skybridges";
    sky.kind = "skybridge";
    for (size_t i = 0; i < tall.size(); ++i)
    {
        int bestJ = -1;
        double best = spec.skybridgeSpan;
        Vec2 a{0.0}, b{0.0};
        for (size_t j = 0; j < tall.size(); ++j)
        {
            if (j == i)
                continue;
            Vec2 pa{0.0}, pb{0.0};
            const double d = closest(tall[i].fp, tall[j].fp, &pa, &pb);
            if (d > 1.0 && d < best)
            {
                best = d;
                bestJ = int(j);
                a = pa;
                b = pb;
            }
        }
        if (bestJ < 0 || joined[{std::min(int(i), bestJ), std::max(int(i), bestJ)}])
            continue;
        joined[{std::min(int(i), bestJ), std::max(int(i), bestJ)}] = true;
        const Tall& lower = tall[i].top - tall[i].ground < tall[size_t(bestJ)].top - tall[size_t(bestJ)].ground ? tall[i] : tall[size_t(bestJ)];
        const double y = lower.ground + (lower.top - lower.ground) * spec.skybridgeHeight;
        sky.lines3.push_back({{a.x, y, a.y}, {b.x, y, b.y}});
    }
    std::erase_if(gen.levels, [](const Level& v) { return v.kind == "skybridge"; });
    if (!sky.lines3.empty())
    {
        double sum = 0.0;
        for (const auto& line : sky.lines3)
            sum += line.front().y;
        sky.height = sum / double(sky.lines3.size());
        gen.levels.push_back(std::move(sky));
    }
}

std::vector<LevelLine> levelLines(const Layout& l, const Ground& g)
{
    LILA_ZONE();
    std::vector<LevelLine> out;
    if (!l.gen)
        return out;
    const GenLayout& gen = *l.gen;
    for (size_t k = 0; k < gen.levels.size(); ++k)
    {
        const Level& lv = gen.levels[k];
        const int layer = int(k) + 1;
        if (lv.kind == "deck")
            for (size_t e = 0; e < lv.graph.edges.size(); ++e)
            {
                if (!lv.graph.isStreet(int(e)))
                    continue;
                const Vec2 a = lv.graph.nodes[size_t(lv.graph.edges[e].a)].p, b = lv.graph.nodes[size_t(lv.graph.edges[e].b)].p;
                out.push_back({{{a.x, lv.height, a.y}, {b.x, lv.height, b.y}}, lv.graph.edges[e].cls, layer, lv.kind});
            }
        else
            for (const auto& line : lv.lines3)
                out.push_back({line, lv.kind == "skybridge" ? StreetClass::Path : StreetClass::Arterial, layer, lv.kind});
    }
    for (const Ramp& r : gen.ramps)
        out.push_back({r.pts, r.cls, r.to, "ramp"});
    const int airLayer = int(gen.levels.size()) + 1;
    for (const auto& [a, b] : gen.air.links)
        if (a < b)
        {
            const glm::dvec3 pa = gen.air.nodes[size_t(a)], pb = gen.air.nodes[size_t(b)];
            int alt = 0;
            for (size_t h = 0; h < gen.air.heights.size(); ++h)
                if (std::abs(gen.air.heights[h] - std::min(pa.y, pb.y)) < 1.0)
                    alt = int(h);
            out.push_back({{pa, pb}, StreetClass::Arterial, airLayer + alt, "air"});
        }
    (void)g;
    return out;
}

RoadNetwork levelNetwork(const Layout& l, int level)
{
    LILA_ZONE();
    if (!l.gen || level < 0 || level >= int(l.gen->levels.size()))
        return {};
    const Level& lv = l.gen->levels[size_t(level)];
    Layout dl{lv.graph};
    dl.profile = l.profile;
    dl.roads = l.roads;
    dl.outline = lv.outline.pts.empty() ? l.outline : lv.outline;
    assignStreetIds(dl);
    return roadNetwork(dl, Ground::plane(lv.height));
}
} // namespace settlement
