// Water on terrains: rivers carved from drawn, imported or flow-derived lines with monotonic beds, lakes at their spill
// or a set level, sea level, and the masks and placement data (splines and locators) the exporters hand on.

#include "Water.h"

#include "Erosion.h"
#include "ErosionGpu.h"
#include "Stamps.h"

#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/UnrealLink.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>

namespace terrain
{
using json = nlohmann::json;

namespace water_detail
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kRad = 0.017453292519943295;
// Opposite directions are k and k ^ 1, as in Erosion.cpp.
constexpr int kDx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr int kDz[8] = {0, 0, 1, -1, 1, -1, -1, 1};
/** Filled minus ground past which a sample sits in a basin rather than on a flat the fill only tilted. */
constexpr float kBasinEps = 0.05f;

double smoothstep(double a, double b, double x)
{
    const double t = std::clamp((x - a) / std::max(1e-12, b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

uint32_t cellOf(const Heightfield& h, glm::dvec2 p)
{
    const int x = std::clamp(int(std::lround((p.x - h.origin.x) / h.cell)), 0, h.nx - 1);
    const int z = std::clamp(int(std::lround((p.y - h.origin.y) / h.cell)), 0, h.nz - 1);
    return uint32_t(h.index(x, z));
}

bool onEdge(const Heightfield& h, uint32_t i)
{
    const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
    return x == 0 || z == 0 || x == h.nx - 1 || z == h.nz - 1;
}

glm::dvec2 posOf(const Heightfield& h, uint32_t i) { return h.samplePos(int(i % uint32_t(h.nx)), int(i / uint32_t(h.nx))); }

json ringJson(const std::vector<glm::dvec2>& r)
{
    json a = json::array();
    for (const glm::dvec2& p : r)
        a.push_back({p.x, p.y});
    return a;
}

std::vector<glm::dvec2> ringFrom(const json& j)
{
    std::vector<glm::dvec2> r;
    if (j.is_array())
        for (const json& p : j)
            r.push_back({p.at(0).get<double>(), p.at(1).get<double>()});
    return r;
}

double num(const json& j, const char* k, double def)
{
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : def;
}

/** Points every step along the line, its ends kept. */
std::vector<glm::dvec2> resampleLine(const std::vector<glm::dvec2>& line, double step)
{
    std::vector<glm::dvec2> out;
    if (line.size() < 2)
        return line;
    double total = 0.0;
    for (size_t k = 1; k < line.size(); ++k)
        total += glm::length(line[k] - line[k - 1]);
    if (total <= 1e-9)
        return {line.front()};
    const int count = std::max(1, int(std::ceil(total / step)));
    const double ds = total / count;
    out.push_back(line.front());
    size_t seg = 1;
    double segStart = 0.0;
    for (int k = 1; k < count; ++k)
    {
        const double s = ds * k;
        while (seg + 1 < line.size() && segStart + glm::length(line[seg] - line[seg - 1]) < s)
        {
            segStart += glm::length(line[seg] - line[seg - 1]);
            ++seg;
        }
        const double len = glm::length(line[seg] - line[seg - 1]);
        const double t = len > 1e-12 ? std::clamp((s - segStart) / len, 0.0, 1.0) : 0.0;
        out.push_back(line[seg - 1] + (line[seg] - line[seg - 1]) * t);
    }
    out.push_back(line.back());
    return out;
}

/** Corner cutting that rounds a cell staircase into a curve; the ends stay put. */
std::vector<glm::dvec2> chaikin(std::vector<glm::dvec2> line, int iterations)
{
    for (int it = 0; it < iterations && line.size() > 2; ++it)
    {
        std::vector<glm::dvec2> out = {line.front()};
        for (size_t k = 0; k + 1 < line.size(); ++k)
        {
            out.push_back(line[k] * 0.75 + line[k + 1] * 0.25);
            out.push_back(line[k] * 0.25 + line[k + 1] * 0.75);
        }
        out.push_back(line.back());
        line = std::move(out);
    }
    return line;
}

/** Hydraulic geometry from drainage area (m2), matching the analysis: discharge m3/s, width and depth cm. */
double dischargeOf(double areaM2) { return 0.02 * areaM2 * 1e-6; }

/** The largest drainage area (m2) within radius cells of p. */
double areaNear(const Heightfield& h, const Drainage& d, glm::dvec2 p, int radius)
{
    const uint32_t c = cellOf(h, p);
    const int cx = int(c % uint32_t(h.nx)), cz = int(c / uint32_t(h.nx));
    float best = 0.0f;
    for (int z = std::max(0, cz - radius); z <= std::min(h.nz - 1, cz + radius); ++z)
        for (int x = std::max(0, cx - radius); x <= std::min(h.nx - 1, cx + radius); ++x)
            best = std::max(best, d.area[h.index(x, z)]);
    const double cm = h.cell * 0.01;
    return double(best) * cm * cm;
}

double slopeDegAt(const Heightfield& h, int x, int z)
{
    const int x0 = std::max(0, x - 1), x1 = std::min(h.nx - 1, x + 1), z0 = std::max(0, z - 1), z1 = std::min(h.nz - 1, z + 1);
    const double gx = (h.at(x1, z) - h.at(x0, z)) / (std::max(1, x1 - x0) * h.cell);
    const double gz = (h.at(x, z1) - h.at(x, z0)) / (std::max(1, z1 - z0) * h.cell);
    return std::atan(std::sqrt(gx * gx + gz * gz)) / kRad;
}

/** A river line before carving: its points and where it came from. */
struct RiverLine
{
    std::string id;
    size_t spec = 0;
    std::vector<glm::dvec2> points;
    bool spring = true;
    bool reversed = false;
    /** Auto rivers: the reach it came from and the reach it flows into. */
    int reach = -1;
    int downstreamReach = -1;
};

/** Everything along a river the carve needs, per point. */
struct Profile
{
    std::vector<glm::dvec2> p;
    std::vector<double> s, ground, area, width, depth, bed, level, curvature, flat, reach, speed;
    struct Fall
    {
        size_t top = 0, bottom = 0;
    };
    std::vector<Fall> falls;
};

Profile profileOf(const Heightfield& h0, const Drainage& d0, const RiverSpec& sp, const RiverLine& line)
{
    Profile pr;
    const double step = h0.cell;
    pr.p = resampleLine(line.points, step);
    auto measure = [&](Profile& q) {
        const size_t m = q.p.size();
        q.s.assign(m, 0.0);
        for (size_t k = 1; k < m; ++k)
            q.s[k] = q.s[k - 1] + glm::length(q.p[k] - q.p[k - 1]);
        q.ground.resize(m);
        q.area.resize(m);
        double running = 0.0;
        for (size_t k = 0; k < m; ++k)
        {
            q.ground[k] = h0.heightAt(q.p[k].x, q.p[k].y);
            running = std::max(running, areaNear(h0, d0, q.p[k], 1));
            q.area[k] = running + std::max(0.0, sp.upstreamArea);
        }
        q.width.resize(m);
        q.depth.resize(m);
        const double lead = line.spring ? 3.0 : 0.0;
        for (size_t k = 0; k < m; ++k)
        {
            const double qd = dischargeOf(q.area[k]);
            double w = sp.width > 0.0 ? sp.width : std::max(sp.minWidth, sp.widthScale * 300.0 * std::sqrt(qd));
            double dp = sp.depth > 0.0 ? sp.depth : std::max(sp.minDepth, sp.depthScale * 30.0 * std::pow(qd, 0.4));
            // A spring starts small and grows to full size over its first few widths.
            if (lead > 0.0)
            {
                const double grow = 0.35 + 0.65 * smoothstep(0.0, lead * w, q.s[k]);
                w *= grow;
                dp *= grow;
            }
            q.width[k] = w;
            q.depth[k] = dp;
        }
        // Flat ground meanders and grows bars; steep ground runs straight.
        q.flat.resize(m);
        for (size_t k = 0; k < m; ++k)
        {
            const size_t a = k >= 4 ? k - 4 : 0, b = std::min(m - 1, k + 4);
            const double run = std::max(1e-9, q.s[b] - q.s[a]);
            const double grade = std::abs(q.ground[a] - q.ground[b]) / run;
            q.flat[k] = std::clamp(1.0 - grade / 0.04, 0.0, 1.0);
        }
    };
    measure(pr);
    if (sp.meander > 0.0 && pr.p.size() > 4)
    {
        // Sideways offset whose wavelength follows the width, faded in at both ends and held back on steep ground.
        const double phase0 = double(sp.seed % 1000u) * 0.0062831853;
        const size_t m = pr.p.size();
        std::vector<glm::dvec2> moved(m);
        double phase = phase0;
        for (size_t k = 0; k < m; ++k)
        {
            if (k > 0)
                phase += 6.283185307179586 * (pr.s[k] - pr.s[k - 1]) / std::max(1.0, sp.meanderLength * pr.width[k]);
            const glm::dvec2 a = pr.p[k > 0 ? k - 1 : 0], b = pr.p[std::min(m - 1, k + 1)];
            glm::dvec2 t = b - a;
            t = glm::length(t) > 1e-12 ? glm::normalize(t) : glm::dvec2(1.0, 0.0);
            const double fade = smoothstep(0.0, sp.meanderLength * pr.width[k], pr.s[k]) * smoothstep(0.0, sp.meanderLength * pr.width[k], pr.s.back() - pr.s[k]);
            const double amp = sp.meander * pr.width[k] * fade * pr.flat[k];
            moved[k] = pr.p[k] + glm::dvec2(-t.y, t.x) * (amp * std::sin(phase));
        }
        pr.p = resampleLine(chaikin(moved, 1), step);
        measure(pr);
    }
    const size_t m = pr.p.size();
    pr.bed.resize(m);
    pr.level.resize(m);
    for (size_t k = 0; k < m; ++k)
    {
        pr.bed[k] = pr.ground[k] - pr.depth[k];
        // Depth grows downstream (springs, gathering area), so on a flat bed the bed digs in to keep the water from rising.
        if (k > 0)
            pr.bed[k] = std::min({pr.bed[k], pr.bed[k - 1], pr.level[k - 1] - pr.depth[k]});
        pr.level[k] = pr.bed[k] + pr.depth[k];
    }
    // Waterfalls: runs where the ground drops steeper than the cliff angle by at least waterfallHeight.
    if (sp.waterfalls)
    {
        const double steep = std::tan(std::clamp(sp.cliffAngle, 5.0, 89.0) * kRad);
        size_t k = 0;
        while (k + 1 < m)
        {
            auto falls = [&](size_t i) { return (pr.ground[i] - pr.ground[i + 1]) / std::max(1e-9, pr.s[i + 1] - pr.s[i]) > steep; };
            if (!falls(k))
            {
                ++k;
                continue;
            }
            size_t e = k;
            while (e + 1 < m && falls(e))
                ++e;
            if (pr.level[k] - pr.level[e] >= sp.waterfallHeight)
                pr.falls.push_back({k, e});
            k = e + 1;
        }
    }
    // Signed curvature, positive turning left, smoothed over a few points.
    std::vector<double> raw(m, 0.0);
    for (size_t k = 1; k + 1 < m; ++k)
    {
        const glm::dvec2 a = pr.p[k] - pr.p[k - 1], b = pr.p[k + 1] - pr.p[k];
        const double la = glm::length(a), lb = glm::length(b);
        if (la < 1e-9 || lb < 1e-9)
            continue;
        const double turn = std::atan2(a.x * b.y - a.y * b.x, glm::dot(a, b));
        raw[k] = turn / (0.5 * (la + lb));
    }
    pr.curvature.assign(m, 0.0);
    for (size_t k = 0; k < m; ++k)
    {
        double sum = 0.0;
        int count = 0;
        for (size_t j = k >= 3 ? k - 3 : 0; j <= std::min(m - 1, k + 3); ++j, ++count)
            sum += raw[j];
        pr.curvature[k] = sum / std::max(1, count);
    }
    pr.reach.resize(m);
    pr.speed.resize(m);
    for (size_t k = 0; k < m; ++k)
    {
        pr.reach[k] = sp.bankWidth > 0.0 ? sp.bankWidth : std::max(4.0 * pr.width[k], 2.0 * h0.cell);
        if (sp.flowSpeed > 0.0)
            pr.speed[k] = sp.flowSpeed;
        else
        {
            // Manning on a wide channel: v = R^(2/3) S^(1/2) / n, R the depth in metres, n 0.035 for a natural bed.
            const size_t a = k >= 3 ? k - 3 : 0, b = std::min(m - 1, k + 3);
            const double slope = std::max(1e-4, (pr.bed[a] - pr.bed[b]) / std::max(1e-9, pr.s[b] - pr.s[a]));
            pr.speed[k] = std::clamp(std::pow(pr.depth[k] * 0.01, 2.0 / 3.0) * std::sqrt(slope) / 0.035, 0.05, 10.0);
        }
    }
    return pr;
}

/** What carving the rivers left per sample. */
struct CarveState
{
    std::vector<float> target, bedMask, bankMask, barMask, fallMask, band;
    /** The river owning each channel sample (the deepest there), or -1. */
    std::vector<int32_t> channel;
    std::vector<float> channelY;

    explicit CarveState(size_t n)
        : target(n, std::numeric_limits<float>::infinity()), bedMask(n, 0.0f), bankMask(n, 0.0f), barMask(n, 0.0f), fallMask(n, 0.0f), band(n, 0.0f),
          channel(n, -1), channelY(n, std::numeric_limits<float>::infinity())
    {
    }
};

/** Cuts one river's channel, banks and bars into the carve targets; samples are touched row by row in parallel. */
void carveRiver(const Heightfield& h, const RiverSpec& sp, const Profile& pr, int river, bool erodeBand, CarveState& st)
{
    LILA_ZONE();

    const size_t m = pr.p.size();
    if (m < 2)
        return;
    const double tanBank = std::tan(std::clamp(sp.bankAngle, 5.0, 85.0) * kRad);
    struct Seg
    {
        int x0, x1, z0, z1;
    };
    std::vector<Seg> segs(m - 1);
    std::vector<double> radius(m - 1);
    int bx0 = h.nx, bx1 = -1, bz0 = h.nz, bz1 = -1;
    for (size_t k = 0; k + 1 < m; ++k)
    {
        const double halfW = std::max(0.5 * std::max(pr.width[k], pr.width[k + 1]), 0.75 * h.cell);
        const double reach = std::max(pr.reach[k], pr.reach[k + 1]);
        const double fall = std::max(2.0 * std::max(pr.width[k], pr.width[k + 1]), 4.0 * h.cell);
        radius[k] = halfW + reach + fall;
        const glm::dvec2 lo = glm::min(pr.p[k], pr.p[k + 1]) - radius[k], hi = glm::max(pr.p[k], pr.p[k + 1]) + radius[k];
        Seg s;
        s.x0 = std::max(0, int(std::floor((lo.x - h.origin.x) / h.cell)));
        s.x1 = std::min(h.nx - 1, int(std::ceil((hi.x - h.origin.x) / h.cell)));
        s.z0 = std::max(0, int(std::floor((lo.y - h.origin.y) / h.cell)));
        s.z1 = std::min(h.nz - 1, int(std::ceil((hi.y - h.origin.y) / h.cell)));
        segs[k] = s;
        bx0 = std::min(bx0, s.x0);
        bx1 = std::max(bx1, s.x1);
        bz0 = std::min(bz0, s.z0);
        bz1 = std::max(bz1, s.z1);
    }
    if (bx1 < bx0 || bz1 < bz0)
        return;
    std::vector<std::vector<uint32_t>> rows(size_t(bz1 - bz0 + 1));
    for (size_t k = 0; k + 1 < m; ++k)
        for (int z = segs[k].z0; z <= segs[k].z1; ++z)
            rows[size_t(z - bz0)].push_back(uint32_t(k));

    // Points under a waterfall keep their drop: re-erosion leaves them alone.
    std::vector<uint8_t> inFall(m, 0);
    for (const Profile::Fall& f : pr.falls)
        for (size_t k = f.top > 0 ? f.top - 1 : 0; k <= std::min(m - 1, f.bottom + 1); ++k)
            inFall[k] = 1;

    const int width = bx1 - bx0 + 1;
    tbb::parallel_for(bz0, bz1 + 1, [&](int z) {
        const std::vector<uint32_t>& list = rows[size_t(z - bz0)];
        if (list.empty())
            return;
        std::vector<double> bestD2(size_t(width), kInf), bestT(size_t(width), 0.0), bestSide(size_t(width), 0.0);
        std::vector<int32_t> bestSeg(size_t(width), -1);
        for (uint32_t k : list)
        {
            const glm::dvec2 a = pr.p[k], b = pr.p[k + 1], ab = b - a;
            const double len2 = std::max(1e-12, glm::dot(ab, ab));
            const double r2 = radius[k] * radius[k];
            for (int x = segs[k].x0; x <= segs[k].x1; ++x)
            {
                const glm::dvec2 q = h.samplePos(x, z);
                const double t = std::clamp(glm::dot(q - a, ab) / len2, 0.0, 1.0);
                const glm::dvec2 c = a + ab * t;
                const double d2 = glm::dot(q - c, q - c);
                const size_t xi = size_t(x - bx0);
                if (d2 > r2 || d2 >= bestD2[xi])
                    continue;
                bestD2[xi] = d2;
                bestT[xi] = t;
                bestSeg[xi] = int32_t(k);
                bestSide[xi] = ab.x * (q - a).y - ab.y * (q - a).x;
            }
        }
        for (int x = bx0; x <= bx1; ++x)
        {
            const size_t xi = size_t(x - bx0);
            if (bestSeg[xi] < 0)
                continue;
            const size_t k = size_t(bestSeg[xi]);
            const double t = bestT[xi];
            auto lerp = [&](const std::vector<double>& v) { return v[k] + (v[k + 1] - v[k]) * t; };
            const double w = lerp(pr.width), dp = lerp(pr.depth), bed = lerp(pr.bed), level = bed + dp, reach = lerp(pr.reach);
            // A stream narrower than the grid still cuts one cell wide, or no sample would see it.
            const double halfW = std::max(0.5 * w, 0.75 * h.cell), d = std::sqrt(bestD2[xi]);
            const double fall = std::max(2.0 * w, 4.0 * h.cell);
            const size_t i = h.index(x, z);
            const bool protect = inFall[k] || inFall[k + 1];
            if (d > halfW + reach)
            {
                if (erodeBand && !protect && d <= halfW + reach + fall)
                    st.band[i] = std::max(st.band[i], float(1.0 - smoothstep(halfW + reach, halfW + reach + fall, d)));
                continue;
            }
            double y;
            if (d <= halfW)
            {
                // A flat floor about a cell wide carries the bed exactly; the sides curve up to the water's edge.
                const double floor = std::min(0.75 * h.cell, halfW), across = std::max(0.0, d - floor) / std::max(1e-9, halfW - floor);
                y = bed + dp * across * across;
                if (sp.gravelBars)
                {
                    // Bars build on the inside of bends, where the flow slows; never on the centreline.
                    const double kap = lerp(pr.curvature);
                    const double inner = kap * bestSide[xi];
                    const double strength = std::clamp(std::abs(kap) * w * 3.0, 0.0, 1.0) * lerp(pr.flat);
                    if (inner > 0.0 && strength > 0.0)
                    {
                        const double u = smoothstep(0.25 * halfW, 0.8 * halfW, d);
                        const double barY = bed + (level - 0.1 * dp - bed) * u;
                        if (barY > y)
                        {
                            y += (barY - y) * strength;
                            st.barMask[i] = std::max(st.barMask[i], float(strength * u));
                        }
                    }
                }
                st.bedMask[i] = 1.0f;
                if (float(y) < st.channelY[i])
                {
                    st.channelY[i] = float(y);
                    st.channel[i] = river;
                }
            }
            else
            {
                y = level + (d - halfW) * tanBank;
                const double bankBand = std::max(std::min(reach, std::max(w, 3.0 * h.cell)), 1.5 * h.cell);
                st.bankMask[i] = std::max(st.bankMask[i], float(1.0 - smoothstep(0.0, bankBand, d - halfW)));
            }
            if (protect)
                st.fallMask[i] = std::max(st.fallMask[i], float(1.0 - smoothstep(halfW, halfW + 2.0 * h.cell, d)));
            else if (erodeBand)
                st.band[i] = 1.0f;
            st.target[i] = std::min(st.target[i], float(y));
        }
    });
}

/** 8-connected flood from seeds over samples below level that no other water holds. Sets touchesEdge when it reaches the edge. */
std::vector<uint32_t> floodBelow(const Heightfield& h, const std::vector<uint32_t>& seeds, double level, const std::vector<int32_t>& body, bool* touchesEdge)
{
    std::vector<uint32_t> out;
    std::vector<uint8_t> seen(h.size(), 0);
    std::deque<uint32_t> open;
    for (uint32_t s : seeds)
        if (!seen[s] && body[s] < 0 && double(h.heights[s]) < level)
        {
            seen[s] = 1;
            open.push_back(s);
        }
    *touchesEdge = false;
    while (!open.empty())
    {
        const uint32_t i = open.front();
        open.pop_front();
        out.push_back(i);
        if (onEdge(h, i))
            *touchesEdge = true;
        const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
        for (int k = 0; k < 8; ++k)
        {
            const int px = x + kDx[k], pz = z + kDz[k];
            if (px < 0 || pz < 0 || px >= h.nx || pz >= h.nz)
                continue;
            const uint32_t j = uint32_t(h.index(px, pz));
            if (!seen[j] && body[j] < 0 && double(h.heights[j]) < level)
            {
                seen[j] = 1;
                open.push_back(j);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/**
 * The basin around seed on the filled surface: samples the fill covers, joined at its water level, that no other water
 * holds. Read off the fill rather than ground below a level, since the fill sits a few float steps over the true spill.
 */
std::vector<uint32_t> floodBasin(const Heightfield& h, const Drainage& d, uint32_t seed, const std::vector<int32_t>& body)
{
    std::vector<uint32_t> out;
    const float level = d.filled[seed];
    auto inBasin = [&](uint32_t j) { return body[j] < 0 && d.filled[j] - h.heights[j] > kBasinEps && std::abs(d.filled[j] - level) < 0.5f; };
    if (!inBasin(seed))
        return out;
    std::vector<uint8_t> seen(h.size(), 0);
    std::deque<uint32_t> open = {seed};
    seen[seed] = 1;
    while (!open.empty())
    {
        const uint32_t i = open.front();
        open.pop_front();
        out.push_back(i);
        const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
        for (int k = 0; k < 8; ++k)
        {
            const int px = x + kDx[k], pz = z + kDz[k];
            if (px < 0 || pz < 0 || px >= h.nx || pz >= h.nz)
                continue;
            const uint32_t j = uint32_t(h.index(px, pz));
            if (!seen[j] && inBasin(j))
            {
                seen[j] = 1;
                open.push_back(j);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/** The lowest fill over a basin: its water level, a hair above the spill. */
double basinLevel(const Drainage& d, const std::vector<uint32_t>& cells)
{
    double level = kInf;
    for (uint32_t i : cells)
        level = std::min(level, double(d.filled[i]));
    return level;
}

/** Fills in a lake's stats: lowest rim sample (the spill), area, depth, volume and shoreline. */
void finishLake(const Heightfield& h, LakeResult& l)
{
    std::vector<uint8_t> in(h.size(), 0);
    for (uint32_t i : l.cells)
        in[i] = 1;
    double rim = kInf;
    uint32_t spill = l.cells.empty() ? 0 : l.cells.front();
    for (uint32_t i : l.cells)
    {
        const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
        for (int k = 0; k < 8; ++k)
        {
            const int px = x + kDx[k], pz = z + kDz[k];
            if (px < 0 || pz < 0 || px >= h.nx || pz >= h.nz)
                continue;
            const uint32_t j = uint32_t(h.index(px, pz));
            if (!in[j] && double(h.heights[j]) < rim)
            {
                rim = h.heights[j];
                spill = j;
            }
        }
    }
    l.spill = posOf(h, spill);
    l.spillHeight = std::isfinite(rim) ? rim : l.level;
    const double cellM2 = h.cell * h.cell * 1e-4;
    l.area = double(l.cells.size()) * cellM2;
    l.maxDepth = 0.0;
    l.volume = 0.0;
    for (uint32_t i : l.cells)
    {
        const double dp = l.level - double(h.heights[i]);
        l.maxDepth = std::max(l.maxDepth, dp);
        l.volume += std::max(0.0, dp) * 0.01 * cellM2;
    }
    l.shoreline = outlines(h, in);
}

/** Basins of the filled surface, each one water level: an id per sample (-1 where it drains), and the count. */
int basins(const Heightfield& h, const Drainage& d, std::vector<int32_t>& label)
{
    const size_t n = h.size();
    label.assign(n, -1);
    int count = 0;
    std::deque<uint32_t> open;
    for (uint32_t s = 0; s < uint32_t(n); ++s)
    {
        if (label[s] >= 0 || d.filled[s] - h.heights[s] <= kBasinEps)
            continue;
        // Neighbouring basins at different levels (a tarn above a valley lake) stay apart.
        const float level = d.filled[s];
        label[s] = count;
        open.push_back(s);
        while (!open.empty())
        {
            const uint32_t i = open.front();
            open.pop_front();
            const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
            for (int k = 0; k < 8; ++k)
            {
                const int px = x + kDx[k], pz = z + kDz[k];
                if (px < 0 || pz < 0 || px >= h.nx || pz >= h.nz)
                    continue;
                const uint32_t j = uint32_t(h.index(px, pz));
                if (label[j] < 0 && d.filled[j] - h.heights[j] > kBasinEps && std::abs(d.filled[j] - level) < 0.5f)
                {
                    label[j] = count;
                    open.push_back(j);
                }
            }
        }
        ++count;
    }
    return count;
}

struct Published
{
    std::mutex mutex;
    std::map<std::pair<uint64_t, std::string>, WaterResult> results;
};

Published& published()
{
    static Published p;
    return p;
}
} // namespace water_detail

// ---------------------------------------------------------------------------------------------------- specs

json RiverSpec::toJson() const
{
    json j = {{"id", id},
              {"source", source},
              {"threshold", threshold},
              {"minLength", minLength},
              {"upstreamArea", upstreamArea},
              {"width", width},
              {"depth", depth},
              {"widthScale", widthScale},
              {"depthScale", depthScale},
              {"minWidth", minWidth},
              {"minDepth", minDepth},
              {"meander", meander},
              {"meanderLength", meanderLength},
              {"seed", seed},
              {"bankAngle", bankAngle},
              {"bankWidth", bankWidth},
              {"reErode", reErode},
              {"reErodeStrength", reErodeStrength},
              {"gravelBars", gravelBars},
              {"waterfalls", waterfalls},
              {"waterfallHeight", waterfallHeight},
              {"cliffAngle", cliffAngle},
              {"plungeDepth", plungeDepth},
              {"flowSpeed", flowSpeed},
              {"keepDirection", keepDirection}};
    if (!path.empty())
        j["path"] = water_detail::ringJson(path);
    if (!object.empty())
        j["object"] = object;
    return j;
}

RiverSpec RiverSpec::fromJson(const json& j)
{
    RiverSpec r;
    r.id = j.value("id", std::string());
    r.source = j.value("source", std::string("path"));
    if (r.source != "path" && r.source != "object" && r.source != "auto")
        throw std::invalid_argument("river source '" + r.source + "' is not path, object or auto");
    r.path = water_detail::ringFrom(j.value("path", json::array()));
    r.object = j.value("object", std::string());
    r.threshold = water_detail::num(j, "threshold", r.threshold);
    r.minLength = water_detail::num(j, "minLength", r.minLength);
    r.upstreamArea = water_detail::num(j, "upstreamArea", r.upstreamArea);
    r.width = water_detail::num(j, "width", r.width);
    r.depth = water_detail::num(j, "depth", r.depth);
    r.widthScale = water_detail::num(j, "widthScale", r.widthScale);
    r.depthScale = water_detail::num(j, "depthScale", r.depthScale);
    r.minWidth = water_detail::num(j, "minWidth", r.minWidth);
    r.minDepth = water_detail::num(j, "minDepth", r.minDepth);
    r.meander = water_detail::num(j, "meander", r.meander);
    r.meanderLength = std::max(2.0, water_detail::num(j, "meanderLength", r.meanderLength));
    r.seed = uint32_t(water_detail::num(j, "seed", 0.0));
    r.bankAngle = water_detail::num(j, "bankAngle", r.bankAngle);
    r.bankWidth = water_detail::num(j, "bankWidth", r.bankWidth);
    r.reErode = int(water_detail::num(j, "reErode", r.reErode));
    r.reErodeStrength = water_detail::num(j, "reErodeStrength", r.reErodeStrength);
    r.gravelBars = j.value("gravelBars", r.gravelBars);
    r.waterfalls = j.value("waterfalls", r.waterfalls);
    r.waterfallHeight = water_detail::num(j, "waterfallHeight", r.waterfallHeight);
    r.cliffAngle = water_detail::num(j, "cliffAngle", r.cliffAngle);
    r.plungeDepth = water_detail::num(j, "plungeDepth", r.plungeDepth);
    r.flowSpeed = water_detail::num(j, "flowSpeed", r.flowSpeed);
    r.keepDirection = j.value("keepDirection", r.keepDirection);
    return r;
}

LakeSpec::LakeSpec() : level(water_detail::kNaN) {}

json LakeSpec::toJson() const
{
    json j = {{"id", id},       {"source", source},         {"at", {at.x, at.y}}, {"minDepth", minDepth}, {"minArea", minArea},
              {"depth", depth}, {"shoreSlope", shoreSlope}, {"dam", dam}};
    if (!std::isnan(level))
        j["level"] = level;
    if (!polygon.empty())
        j["polygon"] = water_detail::ringJson(polygon);
    if (!object.empty())
        j["object"] = object;
    return j;
}

LakeSpec LakeSpec::fromJson(const json& j)
{
    LakeSpec l;
    l.id = j.value("id", std::string());
    l.source = j.value("source", std::string("point"));
    if (l.source != "point" && l.source != "region" && l.source != "object" && l.source != "auto")
        throw std::invalid_argument("lake source '" + l.source + "' is not point, region, object or auto");
    if (j.contains("at") && j["at"].is_array() && j["at"].size() == 2)
        l.at = {j["at"][0].get<double>(), j["at"][1].get<double>()};
    l.polygon = water_detail::ringFrom(j.value("polygon", json::array()));
    l.object = j.value("object", std::string());
    l.level = water_detail::num(j, "level", water_detail::kNaN);
    l.minDepth = water_detail::num(j, "minDepth", l.minDepth);
    l.minArea = water_detail::num(j, "minArea", l.minArea);
    l.depth = water_detail::num(j, "depth", l.depth);
    l.shoreSlope = water_detail::num(j, "shoreSlope", l.shoreSlope);
    l.dam = j.value("dam", l.dam);
    return l;
}

WaterSettings::WaterSettings() : seaLevel(water_detail::kNaN) {}

json WaterSettings::toJson() const
{
    json j = {{"shoreWidth", shoreWidth}, {"beachSlope", beachSlope}, {"beachHeight", beachHeight}, {"drainPits", drainPits}};
    if (!std::isnan(seaLevel))
        j["seaLevel"] = seaLevel;
    return j;
}

WaterSettings WaterSettings::fromJson(const json& j)
{
    WaterSettings s;
    if (!j.is_object())
        return s;
    s.seaLevel = water_detail::num(j, "seaLevel", water_detail::kNaN);
    s.shoreWidth = water_detail::num(j, "shoreWidth", s.shoreWidth);
    s.beachSlope = water_detail::num(j, "beachSlope", s.beachSlope);
    s.beachHeight = water_detail::num(j, "beachHeight", s.beachHeight);
    s.drainPits = j.value("drainPits", s.drainPits);
    return s;
}

const std::vector<std::string>& waterMasks()
{
    static const std::vector<std::string> n = {"riverBed", "riverBank", "gravelBar", "waterfall", "lakeBed", "sea", "water", "shore", "beach"};
    return n;
}

// ---------------------------------------------------------------------------------------------------- apply

namespace water_detail
{
WaterResult applyWaterImpl(Heightfield& h, const WaterSpec& spec, const WaterObjects& objects)
{
    LILA_ZONE();

    WaterResult res;
    if (h.empty())
        return res;
    const size_t n = h.size();
    const std::vector<float> start = h.heights;
    const Drainage d0 = drainAuto(h);

    // Rivers as lines from source to mouth.
    std::vector<RiverLine> lines;
    for (size_t si = 0; si < spec.rivers.size(); ++si)
    {
        const RiverSpec& sp = spec.rivers[si];
        if (sp.source == "auto")
        {
            Heightfield bare;
            bare.nx = h.nx;
            bare.nz = h.nz;
            bare.cell = h.cell;
            bare.origin = h.origin;
            bare.heights = h.heights;
            AnalysisParams ap;
            ap.riverArea = std::max(1.0, sp.threshold);
            ap.seaLevel = spec.settings.seaLevel;
            const auto an = analyse(std::make_shared<const Heightfield>(std::move(bare)), ap);
            const Drainage& dr = an->drainage();
            int kept = 0;
            for (const Reach& r : an->reaches())
            {
                std::vector<uint32_t> cells = r.cells;
                if (r.downstream >= 0)
                    cells.push_back(an->reaches()[size_t(r.downstream)].cells.front());
                else if (r.mouth == "lake" || r.mouth == "sea")
                    cells.push_back(dr.receiver[cells.back()]);
                if (r.spring && r.upstream.empty() && double(cells.size() - 1) * h.cell < sp.minLength)
                    continue;
                if (cells.size() < 2)
                    continue;
                RiverLine l;
                l.id = sp.id + "_" + std::to_string(++kept);
                l.spec = si;
                for (uint32_t c : cells)
                    l.points.push_back(posOf(h, c));
                l.points = chaikin(l.points, 2);
                l.spring = r.spring;
                l.reach = r.id;
                l.downstreamReach = r.downstream;
                lines.push_back(std::move(l));
            }
            continue;
        }
        std::vector<std::vector<glm::dvec2>> drawn;
        if (sp.source == "object")
        {
            auto it = objects.lines.find(sp.object);
            if (it == objects.lines.end() || it->second.empty())
            {
                res.warnings.push_back("river " + sp.id + ": its curve " + sp.object + " gives no line");
                continue;
            }
            drawn = it->second;
        }
        else
            drawn.push_back(sp.path);
        for (size_t c = 0; c < drawn.size(); ++c)
        {
            if (drawn[c].size() < 2)
            {
                res.warnings.push_back("river " + sp.id + ": a line needs two points");
                continue;
            }
            RiverLine l;
            l.id = drawn.size() > 1 ? sp.id + "_" + std::to_string(c + 1) : sp.id;
            l.spec = si;
            l.points = drawn[c];
            const double a = h.heightAt(l.points.front().x, l.points.front().y), b = h.heightAt(l.points.back().x, l.points.back().y);
            if (!sp.keepDirection && b > a)
            {
                std::reverse(l.points.begin(), l.points.end());
                l.reversed = true;
            }
            lines.push_back(std::move(l));
        }
    }

    // Carve every river from the original ground, so the order they are listed in doesn't matter.
    CarveState st(n);
    std::vector<Profile> profiles;
    profiles.reserve(lines.size());
    for (size_t r = 0; r < lines.size(); ++r)
    {
        const RiverSpec& sp = spec.rivers[lines[r].spec];
        profiles.push_back(profileOf(h, d0, sp, lines[r]));
        carveRiver(h, sp, profiles.back(), int(r), sp.reErode > 0, st);
    }
    // Plunge pools: bowls under each waterfall, recorded as channel so re-erosion keeps them.
    struct Pool
    {
        size_t river, fall;
        glm::dvec2 at;
    };
    std::vector<Pool> pools;
    for (size_t r = 0; r < lines.size(); ++r)
    {
        const RiverSpec& sp = spec.rivers[lines[r].spec];
        const Profile& pr = profiles[r];
        for (size_t fi = 0; fi < pr.falls.size(); ++fi)
        {
            const size_t b = pr.falls[fi].bottom;
            const Profile::Fall& f = pr.falls[fi];
            const double drop = pr.level[f.top] - pr.level[b];
            const double radius = std::max(0.75 * pr.width[b], 2.0 * h.cell);
            const double deep = std::min(sp.plungeDepth * drop, 3.0 * pr.depth[b]);
            if (deep <= 0.0)
                continue;
            const glm::dvec2 c = pr.p[b];
            const int x0 = std::max(0, int(std::floor((c.x - radius - h.origin.x) / h.cell))), x1 = std::min(h.nx - 1, int(std::ceil((c.x + radius - h.origin.x) / h.cell)));
            const int z0 = std::max(0, int(std::floor((c.y - radius - h.origin.y) / h.cell))), z1 = std::min(h.nz - 1, int(std::ceil((c.y + radius - h.origin.y) / h.cell)));
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                {
                    const double dd = glm::length(h.samplePos(x, z) - c);
                    if (dd >= radius)
                        continue;
                    const size_t i = h.index(x, z);
                    const float y = float(pr.bed[b] - deep * (1.0 - (dd / radius) * (dd / radius)));
                    st.target[i] = std::min(st.target[i], y);
                    st.channelY[i] = std::min(st.channelY[i], y);
                    if (st.channel[i] < 0)
                        st.channel[i] = int32_t(r);
                    st.bedMask[i] = 1.0f;
                    st.fallMask[i] = 1.0f;
                    st.band[i] = 0.0f;
                }
            pools.push_back({r, fi, c});
        }
    }
    tbb::parallel_for(size_t(0), n, [&](size_t i) { h.heights[i] = std::min(h.heights[i], st.target[i]); });
    const std::vector<float> carved = h.heights;

    // Re-erode around the channels so the cut banks settle into the ground, then put the channels back exactly.
    int reErode = 0;
    double reStrength = 0.0;
    for (size_t r = 0; r < lines.size(); ++r)
    {
        reErode = std::max(reErode, spec.rivers[lines[r].spec].reErode);
        reStrength = std::max(reStrength, spec.rivers[lines[r].spec].reErodeStrength);
    }
    if (reErode > 0)
    {
        int x0 = h.nx, x1 = -1, z0 = h.nz, z1 = -1;
        for (int z = 0; z < h.nz; ++z)
            for (int x = 0; x < h.nx; ++x)
                if (st.band[h.index(x, z)] > 0.0f)
                {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    z0 = std::min(z0, z);
                    z1 = std::max(z1, z);
                }
        if (x1 >= x0)
        {
            const int margin = 4;
            x0 = std::max(0, x0 - margin);
            z0 = std::max(0, z0 - margin);
            x1 = std::min(h.nx - 1, x1 + margin);
            z1 = std::min(h.nz - 1, z1 + margin);
            Heightfield crop = lila::geo::makeHeightfield(x1 - x0 + 1, z1 - z0 + 1, h.cell, h.samplePos(x0, z0));
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    crop.heights[crop.index(x - x0, z - z0)] = h.at(x, z);
            ErosionParams ep;
            ep.iterations = reErode;
            ep.strength = reStrength;
            erode(crop, ep);
            tbb::parallel_for(z0, z1 + 1, [&](int z) {
                for (int x = x0; x <= x1; ++x)
                {
                    const size_t i = h.index(x, z);
                    const float b = st.band[i];
                    if (b > 0.0f)
                        h.heights[i] += (crop.heights[crop.index(x - x0, z - z0)] - h.heights[i]) * b;
                    if (st.channel[i] >= 0 || st.fallMask[i] > 0.0f)
                        h.heights[i] = carved[i];
                }
            });
        }
    }
    // Water bodies: sea, drawn lakes, lakes at points, pools and ponds, then any basin deep enough when asked.
    std::vector<int32_t> body(n, -1);
    auto addBody = [&](LakeResult l) {
        for (uint32_t i : l.cells)
            body[i] = int32_t(res.lakes.size());
        finishLake(h, l);
        if (l.kind == "sea")
        {
            l.spill = glm::dvec2(kNaN);
            l.spillHeight = l.level;
        }
        res.lakes.push_back(std::move(l));
    };
    if (!std::isnan(spec.settings.seaLevel))
    {
        std::vector<uint32_t> seeds;
        for (uint32_t i = 0; i < uint32_t(n); ++i)
            if (onEdge(h, i))
                seeds.push_back(i);
        bool edge = false;
        LakeResult sea;
        sea.id = "sea";
        sea.spec = "sea";
        sea.kind = "sea";
        sea.level = spec.settings.seaLevel;
        sea.cells = floodBelow(h, seeds, sea.level, body, &edge);
        if (!sea.cells.empty())
            addBody(std::move(sea));
    }

    auto lakeRings = [&](const LakeSpec& ls) -> std::vector<std::vector<glm::dvec2>> {
        if (ls.source == "region")
            return {ls.polygon};
        auto it = objects.rings.find(ls.object);
        return it != objects.rings.end() ? it->second : std::vector<std::vector<glm::dvec2>>{};
    };
    // Drawn lakes dig their basin and, when asked, dam their rim up to the level.
    for (const LakeSpec& ls : spec.lakes)
    {
        if (ls.source != "region" && ls.source != "object")
            continue;
        Footprint fp;
        for (const auto& ring : lakeRings(ls))
            if (ring.size() >= 3)
                fp.rings.push_back(ring);
        if (fp.rings.empty())
        {
            res.warnings.push_back("lake " + ls.id + ": no region (give polygon= or a closed curve)");
            continue;
        }
        const std::vector<uint8_t> inside = footprintMask(h, fp);
        std::vector<uint8_t> outside(n);
        for (size_t i = 0; i < n; ++i)
            outside[i] = inside[i] ? 0 : 1;
        const std::vector<float> dIn = squaredDistances(outside, h.nx, h.nz);
        const std::vector<float> dOut = squaredDistances(inside, h.nx, h.nz);
        double rimMin = kInf;
        bool atEdge = false;
        for (size_t i = 0; i < n; ++i)
        {
            if (!inside[i] && dOut[i] <= 2.0f)
                rimMin = std::min(rimMin, double(h.heights[i]));
            if (inside[i] && onEdge(h, uint32_t(i)))
                atEdge = true;
        }
        if (!std::isfinite(rimMin))
        {
            res.warnings.push_back("lake " + ls.id + ": its region covers the whole terrain");
            continue;
        }
        double level = std::isnan(ls.level) ? rimMin : ls.level;
        double damHeight = 0.0;
        if (level > rimMin)
        {
            if (ls.dam)
            {
                // A dam: the rim raised to the level, sloping back down to the ground outside.
                const double tanDam = std::tan(30.0 * kRad);
                for (size_t i = 0; i < n; ++i)
                {
                    if (inside[i] || dOut[i] > 16.0f)
                        continue;
                    // The first ring, diagonals included, is the crest; it falls away outside that.
                    const double y = level - std::max(0.0, std::sqrt(double(dOut[i])) - 1.5) * h.cell * tanDam;
                    if (y > h.heights[i])
                    {
                        damHeight = std::max(damHeight, y - double(h.heights[i]));
                        h.heights[i] = float(y);
                    }
                }
            }
            else
            {
                res.warnings.push_back("lake " + ls.id + ": level " + std::to_string(level) + " is above the lowest ground around it, so the water stands at " +
                                       std::to_string(rimMin) + " (dam=True holds it higher)");
                level = rimMin;
            }
        }
        const double tanShore = std::tan(std::clamp(ls.shoreSlope, 1.0, 80.0) * kRad);
        std::vector<uint32_t> seeds;
        for (size_t i = 0; i < n; ++i)
            if (inside[i])
            {
                const double y = level - std::min(ls.depth, std::sqrt(double(dIn[i])) * h.cell * tanShore);
                h.heights[i] = std::min(h.heights[i], float(y));
                seeds.push_back(uint32_t(i));
            }
        bool edge = false;
        LakeResult l;
        l.id = ls.id;
        l.spec = ls.id;
        l.level = level;
        l.cells = floodBelow(h, seeds, level, body, &edge);
        if (edge || atEdge)
            res.warnings.push_back("lake " + ls.id + ": its water reaches the terrain's edge and would run off there");
        l.damHeight = damHeight;
        if (!l.cells.empty())
            addBody(std::move(l));
    }

    Drainage d = drainAuto(h);
    auto pointLake = [&](const std::string& id, const std::string& specId, const std::string& kind, glm::dvec2 at, double level, bool quiet) {
        const uint32_t c = cellOf(h, at);
        if (body[c] >= 0)
            return std::string();
        const std::vector<uint32_t> basin = floodBasin(h, d, c, body);
        std::vector<uint32_t> cells;
        if (std::isnan(level))
        {
            if (basin.empty())
            {
                if (!quiet)
                    res.warnings.push_back("lake " + id + ": the ground there drains away, so there is no basin to fill (give level= or a region)");
                return std::string();
            }
            cells = basin;
            level = basinLevel(d, basin);
        }
        else
        {
            bool edge = false;
            cells = floodBelow(h, {c}, level, body, &edge);
            if (edge)
            {
                if (basin.empty())
                {
                    res.warnings.push_back("lake " + id + ": water at " + std::to_string(level) + " runs off the terrain's edge and there is no basin there");
                    return std::string();
                }
                const double spill = basinLevel(d, basin);
                res.warnings.push_back("lake " + id + ": water at " + std::to_string(level) + " runs off the terrain's edge, so it stands at its spill level " +
                                       std::to_string(spill));
                level = spill;
                cells = basin;
            }
        }
        if (cells.empty())
            return std::string();
        LakeResult l;
        l.id = id;
        l.spec = specId;
        l.kind = kind;
        l.level = level;
        l.cells = std::move(cells);
        addBody(std::move(l));
        return id;
    };
    for (const LakeSpec& ls : spec.lakes)
        if (ls.source == "point")
            pointLake(ls.id, ls.id, "lake", ls.at, ls.level, false);

    std::vector<std::string> poolIds(pools.size());
    for (size_t k = 0; k < pools.size(); ++k)
        poolIds[k] = pointLake(lines[pools[k].river].id + "_pool" + std::to_string(k + 1), spec.rivers[lines[pools[k].river].spec].id, "pool", pools[k].at, kNaN, true);
    for (size_t r = 0; r < lines.size(); ++r)
    {
        const glm::dvec2 end = profiles[r].p.empty() ? glm::dvec2(0.0) : profiles[r].p.back();
        const uint32_t c = cellOf(h, end);
        if (!profiles[r].p.empty() && !onEdge(h, c) && body[c] < 0 && st.channel[c] == int32_t(r))
            pointLake(lines[r].id + "_pond", spec.rivers[lines[r].spec].id, "pond", end, kNaN, true);
    }

    for (const LakeSpec& ls : spec.lakes)
    {
        if (ls.source != "auto")
            continue;
        std::vector<int32_t> label;
        const int count = basins(h, d, label);
        std::vector<std::vector<uint32_t>> members(static_cast<size_t>(count));
        for (uint32_t i = 0; i < uint32_t(n); ++i)
            if (label[i] >= 0)
                members[size_t(label[i])].push_back(i);
        int made = 0;
        for (const std::vector<uint32_t>& cells : members)
        {
            if (std::any_of(cells.begin(), cells.end(), [&](uint32_t i) { return body[i] >= 0; }))
                continue;
            const double level = basinLevel(d, cells);
            double deepest = 0.0;
            for (uint32_t i : cells)
                deepest = std::max(deepest, level - double(h.heights[i]));
            if (deepest < ls.minDepth || double(cells.size()) * h.cell * h.cell * 1e-4 < ls.minArea)
                continue;
            LakeResult l;
            l.id = ls.id + "_" + std::to_string(++made);
            l.spec = ls.id;
            l.kind = "auto";
            l.level = level;
            l.cells = cells;
            addBody(std::move(l));
        }
    }

    // Basins no water holds are filled to their spill, so every sample drains to the edge, the sea or a lake.
    if (spec.settings.drainPits)
    {
        std::vector<int32_t> label;
        const int count = basins(h, d, label);
        std::vector<uint8_t> held(size_t(count), 0);
        for (size_t i = 0; i < n; ++i)
            if (label[i] >= 0 && body[i] >= 0)
                held[size_t(label[i])] = 1;
        std::vector<uint8_t> filled(size_t(count), 0);
        double added = 0.0;
        for (size_t i = 0; i < n; ++i)
            if (label[i] >= 0 && !held[size_t(label[i])])
            {
                added += double(d.filled[i] - h.heights[i]);
                h.heights[i] = d.filled[i];
                filled[size_t(label[i])] = 1;
            }
        res.pitsFilled = int(std::count(filled.begin(), filled.end(), uint8_t(1)));
        res.pitVolume = added * h.cell * h.cell * 1e-6;
    }


    // Results per river.
    for (size_t r = 0; r < lines.size(); ++r)
    {
        const Profile& pr = profiles[r];
        RiverResult rr;
        rr.id = lines[r].id;
        rr.spec = spec.rivers[lines[r].spec].id;
        rr.spring = lines[r].spring;
        rr.reversed = lines[r].reversed;
        const size_t m = pr.p.size();
        for (size_t k = 0; k < m; ++k)
        {
            WaterPoint w;
            w.p = pr.p[k];
            const glm::dvec2 t = pr.p[std::min(m - 1, k + 1)] - pr.p[k > 0 ? k - 1 : 0];
            w.dir = glm::length(t) > 1e-12 ? glm::normalize(t) : glm::dvec2(1.0, 0.0);
            w.distance = pr.s[k];
            w.bed = pr.bed[k];
            w.level = pr.level[k];
            w.width = pr.width[k];
            w.depth = pr.depth[k];
            w.speed = pr.speed[k];
            w.area = pr.area[k];
            w.discharge = dischargeOf(pr.area[k]);
            rr.maxCut = std::max(rr.maxCut, pr.ground[k] - pr.bed[k]);
            rr.points.push_back(w);
        }
        if (m > 0)
        {
            const uint32_t c = cellOf(h, pr.p.back());
            const int32_t b = body[c];
            if (b >= 0)
                rr.mouth = res.lakes[size_t(b)].kind == "sea" ? "sea" : res.lakes[size_t(b)].kind == "pond" ? "pond" : "lake";
            else if (onEdge(h, c))
                rr.mouth = "edge";
            else if (st.channel[c] >= 0 && st.channel[c] != int32_t(r))
            {
                rr.mouth = "river";
                rr.downstream = st.channel[c];
            }
            else
                rr.mouth = "inland";
            if (lines[r].downstreamReach >= 0)
                for (size_t o = 0; o < lines.size(); ++o)
                    if (lines[o].spec == lines[r].spec && lines[o].reach == lines[r].downstreamReach)
                    {
                        rr.mouth = "river";
                        rr.downstream = int(o);
                    }
            if (rr.spring)
            {
                const uint32_t s = cellOf(h, pr.p.front());
                rr.spring = body[s] < 0 && (st.channel[s] < 0 || st.channel[s] == int32_t(r));
            }
            if (rr.spring)
                res.springs.push_back({rr.id, {pr.p.front().x, pr.level.front(), pr.p.front().y}});
        }
        res.rivers.push_back(std::move(rr));
    }
    for (size_t r = 0; r < lines.size(); ++r)
        for (size_t fi = 0; fi < profiles[r].falls.size(); ++fi)
        {
            const Profile& pr = profiles[r];
            const Profile::Fall& f = pr.falls[fi];
            Waterfall w;
            w.river = lines[r].id;
            w.top = {pr.p[f.top].x, pr.level[f.top], pr.p[f.top].y};
            w.bottom = {pr.p[f.bottom].x, pr.level[f.bottom], pr.p[f.bottom].y};
            w.drop = w.top.y - w.bottom.y;
            for (size_t k = 0; k < pools.size(); ++k)
                if (pools[k].river == r && pools[k].fall == fi)
                    w.pool = poolIds[k];
            res.waterfalls.push_back(std::move(w));
        }

    // Masks.
    std::vector<float> lakeBed(n, 0.0f), sea(n, 0.0f), water(n, 0.0f), shore(n, 0.0f), beach(n, 0.0f);
    std::vector<uint8_t> standing(n, 0);
    for (size_t i = 0; i < n; ++i)
    {
        if (body[i] >= 0)
        {
            standing[i] = 1;
            (res.lakes[size_t(body[i])].kind == "sea" ? sea : lakeBed)[i] = 1.0f;
        }
        water[i] = body[i] >= 0 || st.bedMask[i] > 0.0f ? 1.0f : 0.0f;
    }
    if (std::any_of(standing.begin(), standing.end(), [](uint8_t v) { return v != 0; }))
    {
        const std::vector<float> dist = squaredDistances(standing, h.nx, h.nz);
        // The nearest standing water's level, spread outward breadth first.
        std::vector<int32_t> nearest(body);
        std::deque<uint32_t> open;
        for (uint32_t i = 0; i < uint32_t(n); ++i)
            if (body[i] >= 0)
                open.push_back(i);
        const double reach = spec.settings.shoreWidth / h.cell;
        while (!open.empty())
        {
            const uint32_t i = open.front();
            open.pop_front();
            const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
            for (int k = 0; k < 8; ++k)
            {
                const int px = x + kDx[k], pz = z + kDz[k];
                if (px < 0 || pz < 0 || px >= h.nx || pz >= h.nz)
                    continue;
                const uint32_t j = uint32_t(h.index(px, pz));
                if (nearest[j] < 0 && double(dist[j]) <= (reach + 1.0) * (reach + 1.0))
                {
                    nearest[j] = nearest[i];
                    open.push_back(j);
                }
            }
        }
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t i = h.index(x, z);
                if (standing[i] || nearest[i] < 0)
                    continue;
                const double dd = std::sqrt(double(dist[i])) * h.cell;
                if (dd > h.cell + spec.settings.shoreWidth)
                    continue;
                const double s = 1.0 - smoothstep(h.cell, h.cell + spec.settings.shoreWidth, dd);
                shore[i] = float(s);
                const double above = double(h.heights[i]) - res.lakes[size_t(nearest[i])].level;
                if (slopeDegAt(h, x, z) <= spec.settings.beachSlope && above <= spec.settings.beachHeight)
                    beach[i] = float(s * (1.0 - smoothstep(0.5 * spec.settings.beachHeight, spec.settings.beachHeight, above)));
            }
        });
    }
    auto put = [&](const char* name, std::vector<float>& v) {
        if (std::any_of(v.begin(), v.end(), [](float x) { return x > 0.0f; }))
            h.mask(name) = std::move(v);
    };
    put("riverBed", st.bedMask);
    put("riverBank", st.bankMask);
    put("gravelBar", st.barMask);
    put("waterfall", st.fallMask);
    put("lakeBed", lakeBed);
    put("sea", sea);
    put("water", water);
    put("shore", shore);
    put("beach", beach);
    writeFlowMasks(h, drainAuto(h));

    double cut = 0.0, fill = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double dz = double(h.heights[i]) - double(start[i]);
        (dz < 0.0 ? cut : fill) += std::abs(dz);
    }
    res.cut = cut * h.cell * h.cell * 1e-6;
    res.fill = fill * h.cell * h.cell * 1e-6;
    return res;
}
} // namespace water_detail

WaterResult applyWater(Heightfield& h, const WaterSpec& spec, const WaterObjects& objects) { return water_detail::applyWaterImpl(h, spec, objects); }

void publishWater(uint64_t modifier, const std::string& node, WaterResult r)
{
    water_detail::Published& p = water_detail::published();
    std::lock_guard lock(p.mutex);
    p.results[{modifier, node}] = std::move(r);
}

bool publishedWater(uint64_t modifier, const std::string& node, WaterResult* out)
{
    water_detail::Published& p = water_detail::published();
    std::lock_guard lock(p.mutex);
    auto it = p.results.find({modifier, node});
    if (it == p.results.end())
        return false;
    *out = it->second;
    return true;
}

// ---------------------------------------------------------------------------------------------------- export

json waterJson(const WaterResult& r, const WorldMap& w, double seaLevel)
{
    json splines = json::array(), locators = json::array();
    auto dirJson = [&](glm::dvec2 d) {
        const glm::dvec3 v = glm::dvec3(w.m * glm::dvec4(d.x, 0.0, d.y, 0.0));
        const double l = glm::length(v);
        return l > 1e-12 ? json::array({v.x / l, v.y / l, v.z / l}) : json::array({1.0, 0.0, 0.0});
    };
    for (const RiverResult& rr : r.rivers)
    {
        json pts = json::array();
        for (const WaterPoint& p : rr.points)
            pts.push_back({{"position", w.point(p.p.x, p.level, p.p.y)},
                           {"width", p.width * w.across},
                           {"depth", p.depth * w.vertical},
                           {"bedHeight", w.height(p.p.x, p.bed, p.p.y)},
                           {"waterLevel", w.height(p.p.x, p.level, p.p.y)},
                           {"flowSpeed", p.speed},
                           {"discharge", p.discharge},
                           {"flowDirection", dirJson(p.dir)},
                           {"distance", p.distance * w.across}});
        json s = {{"id", rr.id}, {"kind", "river"}, {"feature", rr.spec}, {"closed", false}, {"mouth", rr.mouth}, {"points", pts}};
        if (rr.downstream >= 0)
            s["downstream"] = r.rivers[size_t(rr.downstream)].id;
        splines.push_back(s);
    }
    for (const LakeResult& l : r.lakes)
    {
        const bool isSea = l.kind == "sea";
        for (size_t k = 0; k < l.shoreline.size(); ++k)
        {
            json pts = json::array();
            for (const glm::dvec2& p : l.shoreline[k])
                pts.push_back({{"position", w.point(p.x, l.level, p.y)}, {"waterLevel", w.height(p.x, l.level, p.y)}});
            if (pts.size() >= 3)
                splines.push_back({{"id", l.shoreline.size() > 1 ? l.id + "_shore" + std::to_string(k + 1) : l.id + "_shore"},
                                   {"kind", isSea ? "seaShore" : "lakeShore"},
                                   {"feature", l.id},
                                   {"closed", true},
                                   {"points", pts}});
        }
        if (l.cells.empty() || isSea)
            continue;
        glm::dvec2 c(0.0);
        for (const glm::dvec2& p : l.shoreline.empty() ? std::vector<glm::dvec2>{l.spill} : l.shoreline.front())
            c += p;
        c /= double(std::max<size_t>(1, l.shoreline.empty() ? 1 : l.shoreline.front().size()));
        locators.push_back({{"id", l.id},
                            {"kind", "lakeLevel"},
                            {"lakeKind", l.kind},
                            {"position", w.point(c.x, l.level, c.y)},
                            {"level", w.height(c.x, l.level, c.y)},
                            {"area", l.area * w.across * w.across},
                            {"maxDepth", l.maxDepth * w.vertical},
                            {"volume", l.volume * w.across * w.across * w.vertical},
                            {"spill", w.point(l.spill.x, l.spillHeight, l.spill.y)}});
    }
    if (!std::isnan(seaLevel))
        locators.push_back({{"id", "sea"}, {"kind", "seaLevel"}, {"position", w.point(0.0, seaLevel, 0.0)}, {"level", w.height(0.0, seaLevel, 0.0)}});
    for (const Spring& s : r.springs)
        locators.push_back({{"id", s.river + "_spring"}, {"kind", "spring"}, {"river", s.river}, {"position", w.point(s.at.x, s.at.y, s.at.z)}});
    for (size_t k = 0; k < r.waterfalls.size(); ++k)
    {
        const Waterfall& f = r.waterfalls[k];
        const std::string id = f.river + "_fall" + std::to_string(k + 1);
        locators.push_back({{"id", id + "_top"}, {"kind", "waterfallTop"}, {"river", f.river}, {"position", w.point(f.top.x, f.top.y, f.top.z)}, {"drop", f.drop * w.vertical}});
        locators.push_back({{"id", id + "_bottom"},
                            {"kind", "waterfallBottom"},
                            {"river", f.river},
                            {"position", w.point(f.bottom.x, f.bottom.y, f.bottom.z)},
                            {"drop", f.drop * w.vertical},
                            {"pool", f.pool}});
    }
    return {{"format", "lila.terrainWater"}, {"version", 1}, {"units", "cm"}, {"splines", splines}, {"locators", locators}};
}

json unrealWaterParams(const json& water, const std::string& id, const std::string& label)
{
    using lila::math::Vector;
    auto ue = [](const json& p) {
        const Vector v = lila::math::toUnreal(Vector(glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>())));
        return json::array({v.x, v.y, v.z});
    };
    json splines = json::array(), locators = json::array();
    for (const json& s : water["splines"])
    {
        json pts = json::array();
        for (const json& p : s["points"])
        {
            json q = p;
            q["position"] = ue(p["position"]);
            const json up = ue(json::array({0.0, 0.0, 0.0}));
            if (p.contains("flowDirection"))
            {
                const json d = ue(p["flowDirection"]);
                q["flowDirection"] = {d[0].get<double>() - up[0].get<double>(), d[1].get<double>() - up[1].get<double>(), d[2].get<double>() - up[2].get<double>()};
            }
            // Heights travel as Unreal Z, so a level on a placed point is that point's Z.
            if (p.contains("waterLevel"))
                q["waterLevel"] = q["position"][2];
            if (p.contains("bedHeight"))
                q["bedHeight"] = q["position"][2].get<double>() - p["depth"].get<double>();
            pts.push_back(q);
        }
        json o = s;
        o["points"] = pts;
        splines.push_back(o);
    }
    for (const json& l : water["locators"])
    {
        json o = l;
        o["position"] = ue(l["position"]);
        if (l.contains("spill"))
            o["spill"] = ue(l["spill"]);
        if (l.contains("level"))
            o["level"] = o["position"][2];
        locators.push_back(o);
    }
    return {{"id", id}, {"label", label}, {"splines", splines}, {"locators", locators}};
}

lila::geo::Mesh waterPreviewMesh(const Heightfield& h, const WaterResult& r, const glm::dmat4& world, size_t maxQuads)
{
    LILA_ZONE();

    std::vector<glm::dvec3> pts;
    std::vector<lila::geo::Index> sizes, idx;
    auto quad = [&](glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, glm::dvec3 d) {
        const lila::geo::Index base = lila::geo::Index(pts.size());
        for (const glm::dvec3& p : {a, b, c, d})
            pts.push_back(glm::dvec3(world * glm::dvec4(p, 1.0)));
        sizes.push_back(4);
        for (lila::geo::Index k = 0; k < 4; ++k)
            idx.push_back(base + k);
    };
    size_t lakeCells = 0;
    for (const LakeResult& l : r.lakes)
        lakeCells += l.cells.size();
    const int step = std::max(1, int(std::ceil(std::sqrt(double(lakeCells) / double(std::max<size_t>(1, maxQuads))))));
    for (const LakeResult& l : r.lakes)
    {
        const double half = 0.5 * h.cell * step;
        for (uint32_t i : l.cells)
        {
            const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
            if (x % step || z % step)
                continue;
            const glm::dvec2 c = h.samplePos(x, z);
            const double y = l.level;
            quad({c.x - half, y, c.y - half}, {c.x - half, y, c.y + half}, {c.x + half, y, c.y + half}, {c.x + half, y, c.y - half});
        }
    }
    for (const RiverResult& rr : r.rivers)
        for (size_t k = 0; k + 1 < rr.points.size(); ++k)
        {
            const WaterPoint& a = rr.points[k];
            const WaterPoint& b = rr.points[k + 1];
            const glm::dvec2 la(-a.dir.y, a.dir.x), lb(-b.dir.y, b.dir.x);
            const glm::dvec2 a0 = a.p + la * (0.5 * a.width), a1 = a.p - la * (0.5 * a.width);
            const glm::dvec2 b0 = b.p + lb * (0.5 * b.width), b1 = b.p - lb * (0.5 * b.width);
            quad({a0.x, a.level, a0.y}, {b0.x, b.level, b0.y}, {b1.x, b.level, b1.y}, {a1.x, a.level, a1.y});
        }
    return lila::geo::Mesh::fromPolygons(pts, sizes, idx);
}
} // namespace terrain
