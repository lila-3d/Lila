// Settlement: generator verification and incremental updates. Metric targets per built-in profile, a hash of a whole
// layout (determinism checks), and blocks, lots and envelopes remade only where the street graph changed.

#include "Generators.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace settlement
{
namespace
{
struct Targets
{
    const char* name;
    double entropy[2], deadEnd[2], fourWay[2], block[2];
};

// Measured over seeds 1 to 20 on a site of the style's own scale (400 m for villages and towns, 1.2 to 1.5 km for
// cities, 3 km for the megacity), then widened so every fair layout of the style passes.
constexpr Targets kTargets[] = {
    {"hamlet", {2.3, 3.6}, {0.0, 0.3}, {0.0, 0.4}, {2000.0, 60000.0}},
    {"streetVillage", {2.0, 3.2}, {0.0, 0.35}, {0.0, 0.6}, {5000.0, 60000.0}},
    {"medievalTown", {2.8, 3.6}, {0.0, 0.2}, {0.0, 0.35}, {600.0, 6000.0}},
    {"usTown", {0.0, 1.6}, {0.0, 0.1}, {0.8, 1.0}, {8000.0, 30000.0}},
    {"manhattan", {0.0, 1.6}, {0.0, 0.1}, {0.8, 1.0}, {6000.0, 25000.0}},
    {"europeanCity", {2.2, 2.9}, {0.0, 0.1}, {0.15, 0.45}, {1500.0, 5000.0}},
    {"modernCity", {0.0, 1.6}, {0.0, 0.1}, {0.8, 1.0}, {10000.0, 35000.0}},
    {"scifiMegacity", {3.0, 3.6}, {0.0, 0.1}, {0.05, 0.35}, {25000.0, 90000.0}},
};

uint64_t mix(uint64_t h, uint64_t v) { return hash64(h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))); }
uint64_t mixd(uint64_t h, double v) { return mix(h, uint64_t(std::llround(std::isfinite(v) ? v * 100.0 : 0.0))); }

uint64_t mixPoly(uint64_t h, const Polygon& p)
{
    h = mix(h, p.pts.size());
    for (size_t k = 0; k < p.pts.size(); ++k)
    {
        h = mixd(mixd(h, p.pts[k].x), p.pts[k].y);
        h = mix(h, uint64_t(int64_t(k < p.tag.size() ? p.tag[k] : 0)));
    }
    return h;
}

uint64_t mixGraph(uint64_t h, const StreetGraph& g)
{
    for (const StreetNode& n : g.nodes)
        h = mix(mixd(mixd(h, n.p.x), n.p.y), n.alive);
    for (const StreetEdge& e : g.edges)
    {
        if (!e.alive)
            continue;
        h = mix(mix(h, uint64_t(e.a)), uint64_t(e.b));
        h = mix(mix(mix(h, uint64_t(e.cls)), uint64_t(int64_t(e.era))), uint64_t(int64_t(e.layer)));
        h = mixd(mix(mix(h, e.street), e.moat), e.width);
    }
    return h;
}

/** A point just inside the face, off its longest side. */
Vec2 insidePoint(const Polygon& f)
{
    const double sign = signedArea(f.pts) >= 0.0 ? 1.0 : -1.0;
    size_t best = 0;
    double len = -1.0;
    for (size_t k = 0; k < f.size(); ++k)
        if (const double d = glm::length(f.pts[(k + 1) % f.size()] - f.pts[k]); d > len)
        {
            len = d;
            best = k;
        }
    const Vec2 a = f.pts[best], b = f.pts[(best + 1) % f.size()];
    return (a + b) * 0.5 + leftNormal((b - a) / std::max(len, 1e-12)) * sign * std::min(1.0, 0.01 * len);
}

bool overlaps(const Polygon& p, Vec2 lo, Vec2 hi)
{
    Vec2 a(std::numeric_limits<double>::max()), b(-std::numeric_limits<double>::max());
    for (const Vec2& q : p.pts)
    {
        a = glm::min(a, q);
        b = glm::max(b, q);
    }
    return a.x <= hi.x && b.x >= lo.x && a.y <= hi.y && b.y >= lo.y;
}
} // namespace

void applyBuiltinTargets(Profile& p)
{
    for (const Targets& t : kTargets)
        if (p.name == t.name)
        {
            p.targetEntropyMin = t.entropy[0], p.targetEntropyMax = t.entropy[1];
            p.targetDeadEndMin = t.deadEnd[0], p.targetDeadEndMax = t.deadEnd[1];
            p.targetFourWayMin = t.fourWay[0], p.targetFourWayMax = t.fourWay[1];
            p.targetBlockMin = t.block[0], p.targetBlockMax = t.block[1];
        }
}

json targetReport(const Layout& l)
{
    LILA_ZONE();
    const json m = metrics(l);
    const Profile& p = l.profile;
    // Junctions inside the settlement: streets ending at its edge, an exclusion or a wall are not dead ends.
    const StreetGraph& g = l.graph;
    int counts[6] = {}, nodes = 0;
    for (size_t n = 0; n < g.nodes.size(); ++n)
    {
        const int d = g.degree(int(n));
        if (d == 0)
            continue;
        bool edge = false;
        for (int e : g.incident(int(n)))
            edge = edge || (g.edges[size_t(e)].alive && g.edges[size_t(e)].cls >= StreetClass::Boundary);
        if (edge)
            continue;
        ++counts[std::min(d, 5)];
        ++nodes;
    }
    const int junctions = counts[3] + counts[4] + counts[5];
    const double deadEnd = nodes ? double(counts[1]) / nodes : 0.0;
    const double fourWay = junctions ? double(counts[4] + counts[5]) / junctions : 0.0;
    json out = json::object();
    bool ok = true;
    auto check = [&](const char* name, double v, double lo, double hi) {
        const bool in = v >= lo && v <= hi;
        ok = ok && in;
        out[name] = {{"value", v}, {"min", lo}, {"max", hi}, {"ok", in}};
    };
    check("orientationEntropy", m["orientationEntropy"].get<double>(), p.targetEntropyMin, p.targetEntropyMax);
    check("deadEndRatio", deadEnd, p.targetDeadEndMin, p.targetDeadEndMax);
    check("fourWayShare", fourWay, p.targetFourWayMin, p.targetFourWayMax);
    check("blockAreaMedianM2", m["blockAreaMedianM2"].get<double>(), p.targetBlockMin, p.targetBlockMax);
    out["ok"] = ok;
    return out;
}

uint64_t layoutHash(const Layout& l)
{
    LILA_ZONE();
    uint64_t h = mixGraph(0x5E77ull, l.graph);
    for (const Block& b : l.blocks)
        h = mix(mix(mixPoly(h, b.outline), uint64_t(b.use)), uint64_t(int64_t(b.era)));
    for (const Lot& lot : l.lots)
        h = mix(mix(mixPoly(h, lot.outline), uint64_t(lot.use)), uint64_t(int64_t(lot.era)));
    for (const Envelope& e : l.envelopes)
        h = mixd(mixd(mix(mix(mixPoly(h, e.footprint), uint64_t(e.floors)), uint64_t(int64_t(e.era))), e.base), e.top);
    if (l.gen)
    {
        for (const WallLine& w : l.gen->walls)
            for (const Vec2& q : w.pts)
                h = mixd(mixd(h, q.x), q.y);
        for (const Gate& g : l.gen->gates)
            h = mixd(mixd(h, g.p.x), g.p.y);
        for (const Tower& t : l.gen->towers)
            h = mixd(mixd(h, t.p.x), t.p.y);
        for (const Level& v : l.gen->levels)
        {
            h = mixGraph(mixd(h, v.height), v.graph);
            for (const auto& line : v.lines3)
                for (const glm::dvec3& q : line)
                    h = mixd(mixd(mixd(h, q.x), q.y), q.z);
        }
        for (const Ramp& r : l.gen->ramps)
            for (const glm::dvec3& q : r.pts)
                h = mixd(mixd(mixd(h, q.x), q.y), q.z);
        h = mix(mix(h, l.gen->air.nodes.size()), l.gen->air.links.size());
        for (const Megablock& m : l.gen->megablocks)
            h = mixd(mixPoly(h, m.footprint), m.top);
    }
    return h;
}

int updateBlocks(Layout& l, const Ground& g, const Options& in, Vec2 lo, Vec2 hi)
{
    LILA_ZONE();

    Options o = in;
    o.profile = l.profile;
    const Profile& p = o.profile;
    std::vector<Polygon> faces;
    for (Face& f : planarFaces(l.graph))
        if (overlaps(f.outline, lo, hi))
        {
            const Vec2 q = insidePoint(f.outline);
            if (contains(l.outline.pts, q) && !excluded(l, q))
                faces.push_back(std::move(f.outline));
        }

    // Out go the blocks the changed faces now cover, and any left in the region from streets that are gone.
    std::vector<char> drop(l.blocks.size(), 0);
    for (size_t b = 0; b < l.blocks.size(); ++b)
    {
        const Polygon& out = l.blocks[b].outline;
        if (overlaps(out, lo, hi))
            drop[b] = 1;
        else
            for (const Polygon& f : faces)
                if (contains(f.pts, insidePoint(out)))
                {
                    drop[b] = 1;
                    break;
                }
    }
    std::vector<Block> blocks;
    std::vector<int> blockMap(l.blocks.size(), -1);
    for (size_t b = 0; b < l.blocks.size(); ++b)
        if (!drop[b])
        {
            blockMap[b] = int(blocks.size());
            blocks.push_back(std::move(l.blocks[b]));
            blocks.back().id = blockMap[b];
        }
    std::vector<Lot> lots;
    std::vector<int> lotMap(l.lots.size(), -1);
    for (size_t k = 0; k < l.lots.size(); ++k)
        if (const int b = l.lots[k].block; b >= 0 && b < int(blockMap.size()) && blockMap[size_t(b)] >= 0)
        {
            lotMap[k] = int(lots.size());
            lots.push_back(std::move(l.lots[k]));
            lots.back().id = lotMap[k];
            lots.back().block = blockMap[size_t(b)];
        }
    std::vector<Envelope> envelopes;
    for (Envelope& e : l.envelopes)
        if (e.lot >= 0 && e.lot < int(lotMap.size()) && lotMap[size_t(e.lot)] >= 0)
        {
            e.lot = lotMap[size_t(e.lot)];
            envelopes.push_back(std::move(e));
        }

    int made = 0;
    for (Polygon& src : faces)
    {
        std::vector<double> d(src.size());
        for (size_t k = 0; k < src.size(); ++k)
        {
            const StreetClass c = l.graph.edges[size_t(src.tag[k])].cls;
            d[k] = 0.5 * edgeWidth(l, src.tag[k]);
            src.tag[k] = c >= StreetClass::Boundary ? -1 : int(c);
        }
        Polygon inner = inset(src, d);
        if (inner.pts.empty() || area(inner) < p.minLot * kM2)
            continue;
        Block b;
        b.id = int(blocks.size());
        b.area = area(inner) / kM2;
        b.outline = std::move(inner);
        ++made;
        if (p.plaza && contains(b.outline.pts, l.center))
        {
            b.use = Use::Plaza;
            Lot lot;
            lot.id = int(lots.size());
            lot.block = b.id;
            lot.outline = b.outline;
            lot.area = b.area;
            lot.use = Use::Plaza;
            lots.push_back(std::move(lot));
            blocks.push_back(std::move(b));
            continue;
        }
        for (Lot& lot : subdivide(b, o, int(lots.size())))
        {
            lot.block = b.id;
            const Vec2 c = centroid(lot.outline);
            const double dist = glm::length(c - l.center) / std::max(l.radius, 1.0);
            Vec2 a(std::numeric_limits<double>::max()), z(-std::numeric_limits<double>::max());
            for (const Vec2& q : lot.outline.pts)
            {
                a = glm::min(a, q);
                z = glm::max(z, q);
            }
            const double fill = lot.area * kM2 / std::max((z.x - a.x) * (z.y - a.y), 1e-9);
            if (lot.area < p.minLot || lot.frontages == 0 || fill < 0.3)
                lot.use = Use::Open;
            else if (lot.frontClass <= StreetClass::Collector && dist < p.commercial)
                lot.use = Use::Commercial;
            else
                lot.use = Use::Residential;
            lot.id = int(lots.size());
            lots.push_back(lot);
            if (o.buildings && lot.use != Use::Open)
                if (Envelope env = envelopeFor(lot, l, g, o); !env.footprint.pts.empty())
                    envelopes.push_back(std::move(env));
        }
        blocks.push_back(std::move(b));
    }
    l.blocks = std::move(blocks);
    l.lots = std::move(lots);
    l.envelopes = std::move(envelopes);
    return made;
}
} // namespace settlement
