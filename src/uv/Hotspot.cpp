#include "uv/Hotspot.h"

#include "core/Profile.h"
#include "geo/Attribute.h"
#include "uv/Uv.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lila::uv
{
namespace
{
/** Turns the chart's uvs a quarter turn k times about their centroid. */
void quarterTurns(Chart& chart, int k)
{
    k = ((k % 4) + 4) % 4;
    if (k == 0)
        return;
    const glm::dvec2 c = chart.uvCentroid();
    for (glm::dvec2& p : chart.uv)
    {
        const glm::dvec2 d = p - c;
        const glm::dvec2 r = k == 1 ? glm::dvec2(-d.y, d.x) : k == 2 ? -d : glm::dvec2(d.y, -d.x);
        p = c + r;
    }
}

void rotateUv(Chart& chart, double angle)
{
    const glm::dvec2 c = chart.uvCentroid();
    const double cs = std::cos(angle), sn = std::sin(angle);
    for (glm::dvec2& p : chart.uv)
    {
        const glm::dvec2 d = p - c;
        p = c + glm::dvec2(cs * d.x - sn * d.y, sn * d.x + cs * d.y);
    }
}

/** Area weighted direction of increasing world height in uv, over the chart's triangles. */
glm::dvec2 upGradient(const Chart& chart, const glm::dvec3& up)
{
    glm::dvec2 sum(0.0);
    for (const auto& t : chart.tris)
    {
        const glm::dvec2 e1 = chart.uv[t[1]] - chart.uv[t[0]], e2 = chart.uv[t[2]] - chart.uv[t[0]];
        const double det = e1.x * e2.y - e2.x * e1.y;
        if (std::abs(det) < 1e-20)
            continue;
        const double f0 = glm::dot(chart.pos[t[0]], up);
        const double d1 = glm::dot(chart.pos[t[1]], up) - f0, d2 = glm::dot(chart.pos[t[2]], up) - f0;
        const glm::dvec2 g((d1 * e2.y - d2 * e1.y) / det, (d2 * e1.x - d1 * e2.x) / det);
        sum += g * std::abs(det);
    }
    return sum;
}

glm::dvec3 chartNormal(const Chart& chart)
{
    glm::dvec3 n(0.0);
    for (const auto& t : chart.tris)
        n += glm::cross(chart.pos[t[1]] - chart.pos[t[0]], chart.pos[t[2]] - chart.pos[t[0]]);
    const double len = glm::length(n);
    return len > 1e-20 ? n / len : glm::dvec3(0.0);
}

/** A disk whose boundary sits at an even distance from its centre and fills the circle: caps, discs, round plates. */
bool looksRadial(const Chart& chart)
{
    if (chart.euler() != 1)
        return false;
    const std::vector<std::vector<Index>> loops = chart.boundaryLoops();
    if (loops.size() != 1 || loops[0].size() < 8)
        return false;
    const glm::dvec2 c = chart.uvCentroid();
    double mean = 0.0;
    for (Index v : loops[0])
        mean += glm::length(chart.uv[v] - c);
    mean /= double(loops[0].size());
    if (mean <= 0.0)
        return false;
    double var = 0.0;
    for (Index v : loops[0])
    {
        const double d = glm::length(chart.uv[v] - c) - mean;
        var += d * d;
    }
    const double cv = std::sqrt(var / double(loops[0].size())) / mean;
    const double fill = std::abs(chart.areaUv()) / (std::numbers::pi * mean * mean);
    return cv < 0.08 && fill > 0.85;
}

bool hasTag(const HotspotTrim& t, const std::string& tag) { return std::ranges::find(t.tags, tag) != t.tags.end(); }

uint64_t splitmix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
} // namespace

double hotspotRandom(uint32_t seed, uint64_t a, uint64_t b)
{
    const uint64_t h = splitmix(splitmix(splitmix(seed) ^ a) ^ (b * 0x2545F4914F6CDD1Dull));
    return double(h >> 11) * (1.0 / 9007199254740992.0);
}

IslandMeasure measureChart(Chart& chart, const HotspotOptions& opts)
{
    IslandMeasure m;
    m.area3d = chart.area3d();
    m.normal = chartNormal(chart);
    if (chart.uv.size() != chart.pos.size() || m.area3d <= 0.0)
        return m;

    // Mirrored islands would put the trim on backwards.
    if (chart.areaUv() < 0.0)
        for (glm::dvec2& p : chart.uv)
            p.x = -p.x;

    m.radial = looksRadial(chart);
    if (!m.radial)
    {
        m.rectified = straightenChart(chart);
        if (m.rectified == StraightenResult::Failed)
        {
            std::vector<glm::dvec2> pts(chart.uv.begin(), chart.uv.end());
            rotateUv(chart, -minAreaRectAngle(pts));
        }
    }

    const glm::dvec3 up = glm::length(opts.up) > 0.0 ? glm::normalize(opts.up) : glm::dvec3(0.0, 1.0, 0.0);
    const bool flat = std::abs(glm::dot(m.normal, up)) > std::cos(glm::radians(opts.flatTolerance));
    HotspotOrient orient = opts.orient;
    if (orient == HotspotOrient::WorldUp && (flat || m.radial))
        orient = HotspotOrient::LongestU;
    if (orient == HotspotOrient::WorldUp)
    {
        const glm::dvec2 g = upGradient(chart, up);
        // The quarter turn that brings the up gradient closest to +V.
        int best = 0;
        double bestDot = -1e300;
        for (int k = 0; k < 4; ++k)
        {
            const glm::dvec2 r = k == 0 ? g : k == 1 ? glm::dvec2(-g.y, g.x) : k == 2 ? -g : glm::dvec2(g.y, -g.x);
            if (r.y > bestDot)
            {
                bestDot = r.y;
                best = k;
            }
        }
        quarterTurns(chart, best);
    }
    else if (orient == HotspotOrient::LongestU)
    {
        const glm::dvec2 size = chart.uvMax() - chart.uvMin();
        if (size.y > size.x)
            quarterTurns(chart, 1);
    }

    const double areaUv = std::abs(chart.areaUv());
    if (areaUv > 0.0)
    {
        const double s = std::sqrt(m.area3d / areaUv);
        for (glm::dvec2& p : chart.uv)
            p *= s;
    }
    const glm::dvec2 size = chart.uvMax() - chart.uvMin();
    m.width = size.x;
    m.height = size.y;
    m.fill = size.x > 0.0 && size.y > 0.0 ? std::abs(chart.areaUv()) / (size.x * size.y) : 0.0;
    return m;
}

std::vector<TrimCandidate> rankTrims(const IslandMeasure& island, std::span<const HotspotTrim> trims, const HotspotOptions& opts)
{
    std::vector<TrimCandidate> out;
    if (island.width <= 0.0 || island.height <= 0.0)
        return out;

    auto usable = [&](const HotspotTrim& t) {
        if (t.reserved)
            return false;
        if (!opts.tag.empty())
            return hasTag(t, opts.tag);
        return t.kind != TrimKind::Unique && t.kind != TrimKind::Flat;
    };
    const bool anyRadial = std::ranges::any_of(trims, [&](const HotspotTrim& t) { return t.kind == TrimKind::Radial && usable(t); });
    const bool wantRadial = island.radial && anyRadial;
    const bool keepsUp = opts.orient == HotspotOrient::WorldUp;

    for (size_t i = 0; i < trims.size(); ++i)
    {
        const HotspotTrim& t = trims[i];
        if (!usable(t) || (t.kind == TrimKind::Radial) != wantRadial)
            continue;
        const double tw = std::max(t.worldSize.x, 1e-9), th = std::max(t.worldSize.y, 1e-9);
        TrimCandidate best;
        best.score = 1e300;
        for (int r = 0; r < 2; ++r)
        {
            if (r == 1 && !(opts.allowRotate && t.allowRotate))
                break;
            const double w = r ? island.height : island.width, h = r ? island.width : island.height;
            double aspect = 0.0, size = 0.0;
            switch (t.kind)
            {
            case TrimKind::StripU:
                size = std::abs(std::log2(h / th));
                // Tiling strips suit long islands; a tall one only reaches them by squashing.
                aspect = 0.5 * std::max(0.0, std::log2(h / w));
                break;
            case TrimKind::StripV:
                size = std::abs(std::log2(w / tw));
                aspect = 0.5 * std::max(0.0, std::log2(w / h));
                break;
            default:
                aspect = std::abs(std::log2((w / h) / (tw / th)));
                size = std::abs(std::log2(std::sqrt(w * h) / std::sqrt(tw * th)));
                break;
            }
            double facing = 0.0;
            if (glm::length(t.normal) > 0.0 && glm::length(island.normal) > 0.0)
                facing = 0.5 * (1.0 - glm::dot(glm::normalize(t.normal), island.normal));
            double score = opts.aspectWeight * aspect + opts.sizeWeight * size + opts.normalWeight * facing;
            // A quarter turn undoes world-up orientation, so it has to win clearly.
            if (r == 1 && keepsUp && !island.radial)
                score += 0.25;
            if (score < best.score)
                best = {int(i), score, size, r == 1};
        }
        if (best.trim >= 0)
            out.push_back(best);
    }
    std::ranges::stable_sort(out, [&](const TrimCandidate& a, const TrimCandidate& b) {
        const bool ra = a.sizeError > opts.maxSizeError, rb = b.sizeError > opts.maxSizeError;
        if (ra != rb)
            return !ra;
        return a.score < b.score;
    });
    return out;
}

void placeChart(Chart& chart, const HotspotTrim& t, bool rotate, const HotspotOptions& opts, double rand0, double rand1, double rand2)
{
    if (chart.uv.empty())
        return;
    if (rotate)
        quarterTurns(chart, 1);
    if (opts.randomFlip && t.allowRotate && rand1 < 0.5)
        quarterTurns(chart, 2);
    if (opts.randomMirror && rand2 < 0.5)
        for (glm::dvec2& p : chart.uv)
            p.x = -p.x;

    const glm::dvec2 lo = chart.uvMin(), hi = chart.uvMax();
    const glm::dvec2 size = glm::max(hi - lo, glm::dvec2(1e-12));
    const glm::dvec2 rmin = t.min + glm::dvec2(t.inset), rmax = glm::max(t.max - glm::dvec2(t.inset), rmin + glm::dvec2(1e-9));
    const glm::dvec2 room = rmax - rmin;

    TrimFit fit = t.fit;
    TrimKind kind = t.kind;
    if (kind == TrimKind::StripU || kind == TrimKind::StripV)
        fit = TrimFit::Seamless;
    else if (fit == TrimFit::Seamless)
        kind = size.x >= size.y ? TrimKind::StripU : TrimKind::StripV;

    glm::dvec2 scale(1.0), origin(rmin);
    if (fit == TrimFit::Fill)
        scale = room / size;
    else if (fit == TrimFit::Seamless && kind == TrimKind::StripU)
    {
        scale = glm::dvec2(room.y / size.y);
        origin = glm::dvec2(rmin.x + (opts.randomOffset ? rand0 * (t.max.x - t.min.x) : 0.0), rmin.y);
    }
    else if (fit == TrimFit::Seamless && kind == TrimKind::StripV)
    {
        scale = glm::dvec2(room.x / size.x);
        origin = glm::dvec2(rmin.x, rmin.y + (opts.randomOffset ? rand0 * (t.max.y - t.min.y) : 0.0));
    }
    else
    {
        scale = glm::dvec2(std::min(room.x / size.x, room.y / size.y));
        origin = rmin + (room - size * scale) * 0.5;
    }
    for (glm::dvec2& p : chart.uv)
        p = (p - lo) * scale + origin;
}

Index markSeamsByAngle(geo::Mesh& mesh, std::span<const Index> faces, double degrees)
{
    LILA_ZONE();

    const std::vector<Index> live = liveFaces(mesh, faces);
    std::vector<uint8_t> in(mesh.faceCapacity(), 0);
    for (Index f : live)
        in[f] = 1;
    std::span<bool> seam = mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    Index added = 0;
    for (Index f : live)
        mesh.forEachFaceCorner(f, [&](Index c) {
            const Index e = mesh.cornerEdge(c);
            if (seam[e])
                return;
            bool cut = !mesh.isManifoldEdge(e);
            if (!cut)
            {
                const Index other = mesh.oppositeFace(e, f);
                cut = other == kInvalid || !in[other] || (degrees > 0.0 && dihedralAngle(mesh, e) > degrees);
            }
            if (cut)
            {
                seam[e] = true;
                ++added;
            }
        });
    return added;
}

HotspotResult hotspot(geo::Mesh& mesh, std::span<const Index> faceIds, std::span<const HotspotTrim> trims, std::string_view uvSet,
                      const HotspotOptions& opts)
{
    LILA_ZONE();

    HotspotResult res;
    const std::vector<Index> faces = liveFaces(mesh, faceIds);
    if (faces.empty() || trims.empty())
        return res;
    if (opts.splitAngle > 0.0)
        res.seamsAdded = markSeamsByAngle(mesh, faces, opts.splitAngle);

    UnwrapFacesOptions uo;
    uo.keepPlacement = false;
    uo.pack = false;
    uo.unwrap.usePins = false;
    res.seamsAdded += unwrapFaces(mesh, faces, uvSet, uo).seamsAdded;

    ChartOptions co;
    co.uvSet = uvSet;
    co.pinAttr = {};
    std::vector<Chart> charts = buildCharts(mesh, faces, co);
    res.islands.resize(charts.size());

    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        Chart& ch = charts[i];
        HotspotIsland& out = res.islands[i];
        out.faces = ch.faces;
        out.measure = measureChart(ch, opts);
        const std::vector<TrimCandidate> ranked = rankTrims(out.measure, trims, opts);
        if (ranked.empty())
            return;

        // The island's first face keys its randomness, so it does not depend on the other islands.
        const uint64_t key = ch.faces.empty() ? i : ch.faces.front();
        size_t pick = 0;
        if (opts.cycle > 0)
            pick = size_t(opts.cycle) % ranked.size();
        else
        {
            std::vector<size_t> pool;
            const TrimCandidate& best = ranked.front();
            const int group = trims[size_t(best.trim)].altGroup;
            for (size_t k = 0; k < ranked.size(); ++k)
            {
                const TrimCandidate& c = ranked[k];
                const bool tied = c.score <= best.score + opts.tieTolerance && (c.sizeError > opts.maxSizeError) == (best.sizeError > opts.maxSizeError);
                const bool sameGroup = group >= 0 && trims[size_t(c.trim)].altGroup == group;
                if (tied || sameGroup)
                    pool.push_back(k);
            }
            pick = pool[std::min(pool.size() - 1, size_t(hotspotRandom(opts.seed, key, 0) * double(pool.size())))];
        }
        const TrimCandidate& c = ranked[pick];
        placeChart(ch, trims[size_t(c.trim)], c.rotated, opts, hotspotRandom(opts.seed, key, 1), hotspotRandom(opts.seed, key, 2),
                   hotspotRandom(opts.seed, key, 3));
        out.trim = c.trim;
        out.score = c.score;
        out.sizeError = c.sizeError;
        out.rotated = c.rotated;
        out.poor = c.sizeError > opts.maxSizeError;
    });

    writeCharts(mesh, charts, uvSet);
    for (const HotspotIsland& is : res.islands)
    {
        if (is.trim < 0)
            ++res.failed;
        else
        {
            ++res.matched;
            if (is.poor)
                ++res.poor;
        }
    }
    return res;
}
} // namespace lila::uv
