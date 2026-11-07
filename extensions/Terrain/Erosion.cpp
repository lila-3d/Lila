// Drainage and erosion on heightfields: priority-flood routing, implicit stream-power erosion with uplift, and thermal
// relaxation at the talus angle. CPU, deterministic at any thread count.

#include "Erosion.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>

namespace terrain
{
namespace
{
// Opposite directions are k and k ^ 1.
constexpr int kDx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr int kDz[8] = {0, 0, 1, -1, 1, -1, -1, 1};
constexpr double kDist[8] = {1, 1, 1, 1, 1.4142135623730951, 1.4142135623730951, 1.4142135623730951, 1.4142135623730951};

bool onEdge(const Heightfield& h, int x, int z) { return x == 0 || z == 0 || x == h.nx - 1 || z == h.nz - 1; }

/** A mask's values, or fallback everywhere when it is missing. */
std::vector<float> maskOr(const Heightfield& h, const std::string& name, float fallback)
{
    if (!name.empty())
        if (const std::vector<float>* m = h.findMask(name))
            return *m;
    return std::vector<float>(h.size(), fallback);
}

/** Sorts like the flood's priority queue compares floats: ascending, with -0 equal to +0. */
uint32_t floodKey(float f)
{
    if (f == 0.0f)
        f = 0.0f;
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

void normalisedInto(std::vector<float>& dst, const std::vector<float>& src)
{
    float hi = 0.0f;
    for (float v : src)
        hi = std::max(hi, v);
    const float inv = hi > 0.0f ? 1.0f / hi : 0.0f;
    for (size_t k = 0; k < src.size(); ++k)
        dst[k] = src[k] * inv;
}

/** Receivers and accumulated area for d.filled and d.order. */
void routeDrainage(const Heightfield& h, Drainage& d)
{
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const uint32_t i = uint32_t(h.index(x, z));
            d.receiver[i] = i;
            if (onEdge(h, x, z))
                continue;
            double best = 0.0;
            for (int k = 0; k < 8; ++k)
            {
                const uint32_t j = uint32_t(h.index(x + kDx[k], z + kDz[k]));
                const double slope = (double(d.filled[i]) - double(d.filled[j])) / kDist[k];
                if (slope > best)
                {
                    best = slope;
                    d.receiver[i] = j;
                }
            }
        }
    });
    walkDrainage(
        d.order, d.receiver, true,
        [&](uint32_t i) {
            prefetchLine(&d.area[i]);
            prefetchLine(&d.area[d.receiver[i]]);
        },
        [&](uint32_t i) {
            if (const uint32_t r = d.receiver[i]; r != i)
                d.area[r] += d.area[i];
        });
}
} // namespace

Drainage drain(const Heightfield& h)
{
    LILA_ZONE();

    Drainage d;
    const size_t n = h.size();
    d.filled = h.heights;
    d.receiver.resize(n);
    d.order.reserve(n);
    d.area.assign(n, 1.0f);
    std::vector<uint8_t> seen(n, 0);
    using Entry = std::pair<float, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
            if (onEdge(h, x, z))
            {
                const uint32_t i = uint32_t(h.index(x, z));
                seen[i] = 1;
                open.push({d.filled[i], i});
            }
    while (!open.empty())
    {
        const auto [height, i] = open.top();
        open.pop();
        d.order.push_back(i);
        const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
        for (int k = 0; k < 8; ++k)
        {
            const int nx = x + kDx[k], nz = z + kDz[k];
            if (nx < 0 || nz < 0 || nx >= h.nx || nz >= h.nz)
                continue;
            const uint32_t j = uint32_t(h.index(nx, nz));
            if (seen[j])
                continue;
            seen[j] = 1;
            // A pit is raised to just above its spill point, so it drains instead of holding water.
            if (d.filled[j] <= height)
                d.filled[j] = std::nextafter(height, std::numeric_limits<float>::infinity());
            open.push({d.filled[j], j});
        }
    }

    routeDrainage(h, d);
    return d;
}

Drainage drainFilled(const Heightfield& h, std::vector<float> filled)
{
    LILA_ZONE();

    Drainage d;
    const size_t n = h.size();
    d.filled = std::move(filled);
    d.receiver.resize(n);
    d.area.assign(n, 1.0f);
    std::vector<uint64_t> keyed(n);
    tbb::parallel_for(size_t(0), n, [&](size_t i) { keyed[i] = uint64_t(floodKey(d.filled[i])) << 32 | uint64_t(i); });
    tbb::parallel_sort(keyed.begin(), keyed.end());
    d.order.resize(n);
    tbb::parallel_for(size_t(0), n, [&](size_t k) { d.order[k] = uint32_t(keyed[k]); });
    routeDrainage(h, d);
    return d;
}

void writeFlowMasks(Heightfield& h, const Drainage& d)
{
    LILA_ZONE();

    std::vector<float>& flow = h.mask("flow");
    std::vector<float>& lakes = h.mask("lakes");
    float top = 0.0f;
    for (float a : d.area)
        top = std::max(top, a);
    const float inv = top > 1.0f ? 1.0f / std::log(top) : 0.0f;
    float deepest = 0.0f;
    for (size_t k = 0; k < h.size(); ++k)
        deepest = std::max(deepest, d.filled[k] - h.heights[k]);
    const float invDepth = deepest > 1e-3f ? 1.0f / deepest : 0.0f;
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) {
        flow[k] = std::log(d.area[k]) * inv;
        lakes[k] = std::clamp((d.filled[k] - h.heights[k]) * invDepth, 0.0f, 1.0f);
    });
}

void thermal(Heightfield& h, double talusAngle, int iterations)
{
    LILA_ZONE();

    const double talus = std::tan(std::clamp(talusAngle, 1.0, 89.0) * 0.017453292519943295) * h.cell;
    std::vector<float> sum(h.size()), move(h.size()), delta(h.size());
    auto excess = [&](size_t i, int x, int z, int k) -> float {
        const int nx = x + kDx[k], nz = z + kDz[k];
        if (nx < 0 || nz < 0 || nx >= h.nx || nz >= h.nz)
            return 0.0f;
        return std::max(0.0f, float(h.heights[i] - h.heights[h.index(nx, nz)] - talus * kDist[k]));
    };
    for (int it = 0; it < iterations; ++it)
    {
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t i = h.index(x, z);
                float s = 0.0f, most = 0.0f;
                for (int k = 0; k < 8; ++k)
                {
                    const float e = excess(i, x, z, k);
                    s += e;
                    most = std::max(most, e);
                }
                sum[i] = s;
                // Half the steepest excess: enough to settle, little enough that two neighbours never overshoot.
                move[i] = 0.5f * most;
            }
        });
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t i = h.index(x, z);
                float in = 0.0f;
                for (int k = 0; k < 8; ++k)
                {
                    const int nx = x + kDx[k], nz = z + kDz[k];
                    if (nx < 0 || nz < 0 || nx >= h.nx || nz >= h.nz)
                        continue;
                    const size_t j = h.index(nx, nz);
                    if (sum[j] <= 0.0f)
                        continue;
                    // The neighbour's share for this sample is its excess toward it.
                    const float e = excess(j, nx, nz, k ^ 1);
                    in += move[j] * e / sum[j];
                }
                delta[i] = in - (sum[i] > 0.0f ? move[i] : 0.0f);
            }
        });
        tbb::parallel_for(size_t(0), h.size(), [&](size_t i) { h.heights[i] += delta[i]; });
    }
}

void erode(Heightfield& h, const ErosionParams& p)
{
    LILA_ZONE();

    if (h.empty())
        return;
    const std::vector<float> start = h.heights;
    const std::vector<float> uplift = maskOr(h, p.upliftMask, 1.0f);
    const std::vector<float> hardness = maskOr(h, p.hardnessMask, 0.0f);
    const std::vector<float> pinWeight = maskOr(h, h.findMask(p.constraintHeightMask) ? p.constraintMask : std::string(), 0.0f);
    const std::vector<float> pinHeight = maskOr(h, p.constraintHeightMask, 0.0f);
    auto pin = [&](size_t i, float v) { return v + (pinHeight[i] - v) * std::clamp(pinWeight[i], 0.0f, 1.0f); };
    const bool pinned = std::any_of(pinWeight.begin(), pinWeight.end(), [](float w) { return w > 0.0f; });
    auto pinAll = [&] {
        if (pinned)
            tbb::parallel_for(size_t(0), h.size(), [&](size_t i) { h.heights[i] = pin(i, h.heights[i]); });
    };
    pinAll();
    // Erosion works in metres so a grid erodes the same at any resolution: area in m2, distance in m.
    const double cellM = h.cell * 0.01, cellArea = cellM * cellM;
    Drainage d;
    for (int it = 0; it < std::max(0, p.iterations); ++it)
    {
        d = drain(h);
        std::vector<float>& z = h.heights;
        z = d.filled;
        for (uint32_t i : d.order)
        {
            const uint32_t r = d.receiver[i];
            if (r == i)
                continue;
            const int x = int(i % uint32_t(h.nx)), zi = int(i / uint32_t(h.nx));
            const int rx = int(r % uint32_t(h.nx)), rz = int(r / uint32_t(h.nx));
            const double dist = std::sqrt(double((x - rx) * (x - rx) + (zi - rz) * (zi - rz))) * cellM;
            const double k = p.strength * (1.0 - 0.9 * std::clamp(double(hardness[i]), 0.0, 1.0));
            const double f = k * std::pow(double(d.area[i]) * cellArea, p.areaExponent) / dist;
            const double raised = double(z[i]) + p.uplift * double(uplift[i]);
            z[i] = pin(i, float((raised + f * double(z[r])) / (1.0 + f)));
        }
        if (p.thermalIterations > 0)
        {
            thermal(h, p.talusAngle, p.thermalIterations);
            for (int x = 0; x < h.nx; ++x)
            {
                z[h.index(x, 0)] = d.filled[h.index(x, 0)];
                z[h.index(x, h.nz - 1)] = d.filled[h.index(x, h.nz - 1)];
            }
            for (int r = 0; r < h.nz; ++r)
            {
                z[h.index(0, r)] = d.filled[h.index(0, r)];
                z[h.index(h.nx - 1, r)] = d.filled[h.index(h.nx - 1, r)];
            }
            pinAll();
        }
    }
    d = drain(h);
    writeFlowMasks(h, d);
    std::vector<float> lost(h.size()), gained(h.size());
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) {
        const float change = h.heights[k] - start[k] - float(p.uplift * p.iterations * uplift[k]);
        lost[k] = std::max(0.0f, -change);
        gained[k] = std::max(0.0f, change);
    });
    normalisedInto(h.mask("wear"), lost);
    normalisedInto(h.mask("deposit"), gained);
}
} // namespace terrain
