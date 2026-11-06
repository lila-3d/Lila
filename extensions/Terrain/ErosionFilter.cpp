// The erosion filter, written from Rune Skovbo Johansen's description of the technique (stripes aligned with the slope,
// blended over jittered cells, octaves following the slope the previous ones made). shaders/terrain_landform.slang's
// filterCS is the same arithmetic per sample; keep them in step.

#include "ErosionFilter.h"
#include "Landform.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace terrain
{
namespace efilter_
{
constexpr double kTau = 6.283185307179586;
/** Kernel reach in cells. With points jittered inside the middle half of their cell, the 3 x 3 cells around a point
 *  hold every point within reach, and the nearest is always inside it. */
constexpr double kReach = 1.25;

uint32_t hash3(int32_t x, int32_t z, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(z) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

double unit(uint32_t h) { return double(h & 0xffffffu) / double(0x1000000u); }
} // namespace efilter_

double erosionFilterOffset(const ErosionFilterParams& p, double wx, double wz, double gx, double gz)
{
    using namespace efilter_;
    double total = 0.0, dx = 0.0, dz = 0.0;
    double spacing = std::max(1.0, p.scale), depth = p.strength * std::max(1.0, p.scale);
    for (int o = 0; o < std::max(0, p.octaves); ++o)
    {
        const double ux = gx + dx, uz = gz + dz;
        const double slope = std::sqrt(ux * ux + uz * uz);
        const double f = 1.0 / spacing;
        if (slope > 1e-9)
        {
            // Across the slope: stripes vary along it and stay constant down the fall line, so the gullies run downhill.
            const double px = -uz / slope, pz = ux / slope;
            const double qx = wx * f, qz = wz * f;
            const int cx = int(std::floor(qx)), cz = int(std::floor(qz));
            double sw = 0.0, sv = 0.0, sdx = 0.0, sdz = 0.0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                {
                    const uint32_t hsh = hash3(cx + i, cz + j, p.seed * 31u + uint32_t(o));
                    const double ox = double(cx + i) + 0.25 + 0.5 * unit(hsh);
                    const double oz = double(cz + j) + 0.25 + 0.5 * unit((hsh * 0x9e3779b9u) >> 8);
                    const double ddx = qx - ox, ddz = qz - oz;
                    const double t = 1.0 - (ddx * ddx + ddz * ddz) / (kReach * kReach);
                    if (t <= 0.0)
                        continue;
                    const double w = t * t * t;
                    const double phase = (ddx * px + ddz * pz) * kTau;
                    sw += w;
                    sv += w * std::cos(phase);
                    const double s = -std::sin(phase) * kTau * w;
                    sdx += s * px;
                    sdz += s * pz;
                }
            if (sw > 1e-12)
            {
                const double fade = std::clamp(slope / std::max(1e-6, p.slopeFull), 0.0, 1.0);
                const double a = depth * fade;
                total += a * sv / sw;
                dx += a * sdx / sw * f;
                dz += a * sdz / sw * f;
            }
        }
        spacing /= std::max(1.01, p.lacunarity);
        depth *= std::clamp(p.gain, 0.0, 1.0);
    }
    return total;
}

void erosionFilter(Heightfield& h, const ErosionFilterParams& p)
{
    LILA_ZONE();

    if (h.empty())
        return;
    std::vector<float> offset(h.size());
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const int x0 = std::max(0, x - 1), x1 = std::min(h.nx - 1, x + 1);
            const int z0 = std::max(0, z - 1), z1 = std::min(h.nz - 1, z + 1);
            const double gx = (double(h.at(x1, z)) - double(h.at(x0, z))) / (double(x1 - x0) * h.cell);
            const double gz = (double(h.at(x, z1)) - double(h.at(x, z0))) / (double(z1 - z0) * h.cell);
            const glm::dvec2 q = h.samplePos(x, z);
            offset[h.index(x, z)] = float(erosionFilterOffset(p, q.x, q.y, gx, gz));
        }
    });
    std::vector<float> floors(h.size());
    for (size_t k = 0; k < h.size(); ++k)
    {
        h.heights[k] += offset[k];
        floors[k] = std::max(0.0f, -offset[k]);
    }
    accumulateMask(h, "wear", floors);
}
} // namespace terrain
