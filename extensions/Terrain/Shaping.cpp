// Shaping operators: terrace, strata, fold, warp, ridge sharpen and cliff. Each works per sample (warp and the blurs
// read neighbours from a copy), in parallel rows, so the result is the same at any thread count.

#include "Shaping.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace terrain
{
namespace shaping_
{
constexpr double kPi = 3.14159265358979323846;

double smooth01(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** Slope (rise over run) at a sample from central differences. */
double slopeAt(const Heightfield& h, int x, int z)
{
    const int x0 = std::max(0, x - 1), x1 = std::min(h.nx - 1, x + 1);
    const int z0 = std::max(0, z - 1), z1 = std::min(h.nz - 1, z + 1);
    const double gx = (h.at(x1, z) - h.at(x0, z)) / (std::max(1, x1 - x0) * h.cell);
    const double gz = (h.at(x, z1) - h.at(x, z0)) / (std::max(1, z1 - z0) * h.cell);
    return std::sqrt(gx * gx + gz * gz);
}

double fbm(double x, double z, uint32_t seed, int octaves)
{
    double sum = 0.0, amp = 1.0, norm = 0.0, f = 1.0;
    for (int o = 0; o < octaves; ++o)
    {
        sum += amp * gradientNoise(x * f, z * f, seed + uint32_t(o) * 1013u);
        norm += amp;
        amp *= 0.5;
        f *= 2.0;
    }
    return sum / norm;
}
} // namespace shaping_

std::vector<float> boxBlurred(const std::vector<float>& v, int nx, int nz, int radius, int passes)
{
    LILA_ZONE();

    std::vector<float> a = v, b(v.size());
    if (radius <= 0)
        return a;
    const double inv = 1.0 / double(2 * radius + 1);
    for (int pass = 0; pass < passes; ++pass)
    {
        tbb::parallel_for(0, nz, [&](int z) {
            const float* row = a.data() + size_t(z) * size_t(nx);
            float* out = b.data() + size_t(z) * size_t(nx);
            double sum = 0.0;
            for (int o = -radius; o <= radius; ++o)
                sum += row[std::clamp(o, 0, nx - 1)];
            for (int x = 0; x < nx; ++x)
            {
                out[x] = float(sum * inv);
                sum += row[std::min(nx - 1, x + radius + 1)] - row[std::max(0, x - radius)];
            }
        });
        tbb::parallel_for(0, nx, [&](int x) {
            double sum = 0.0;
            for (int o = -radius; o <= radius; ++o)
                sum += b[size_t(std::clamp(o, 0, nz - 1)) * size_t(nx) + size_t(x)];
            for (int z = 0; z < nz; ++z)
            {
                a[size_t(z) * size_t(nx) + size_t(x)] = float(sum * inv);
                sum += b[size_t(std::min(nz - 1, z + radius + 1)) * size_t(nx) + size_t(x)] - b[size_t(std::max(0, z - radius)) * size_t(nx) + size_t(x)];
            }
        });
    }
    return a;
}

double terraceHeight(double height, double step, double sharpness, double offset)
{
    if (step <= 0.0)
        return height;
    const double t = (height - offset) / step;
    const double level = std::floor(t), frac = t - level;
    // The riser takes the last (1 - sharpness) of each step; the rest is a flat tread.
    const double riser = std::max(0.02, 1.0 - std::clamp(sharpness, 0.0, 1.0) * 0.98);
    const double s = shaping_::smooth01((frac - (1.0 - riser)) / riser);
    return offset + (level + s) * step;
}

void terrace(Heightfield& h, const TerraceParams& p)
{
    LILA_ZONE();

    const std::vector<float>* m = p.mask.empty() ? nullptr : h.findMask(p.mask);
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) {
        const double w = std::clamp(p.strength, 0.0, 1.0) * (m ? std::clamp(double((*m)[k]), 0.0, 1.0) : 1.0);
        const double v = h.heights[k];
        h.heights[k] = float(v + (terraceHeight(v, p.step, p.sharpness, p.offset) - v) * w);
    });
}

void strata(Heightfield& h, const StrataParams& p)
{
    LILA_ZONE();

    const double a = p.dipDirection * shaping_::kPi / 180.0;
    const double tilt = std::tan(std::clamp(p.dip, -80.0, 80.0) * shaping_::kPi / 180.0);
    const glm::dvec2 dir(std::cos(a) * tilt, std::sin(a) * tilt);
    const double thick = std::max(1.0, p.thickness);
    std::vector<float>& hard = h.mask("strata");
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t k = h.index(x, z);
            const glm::dvec2 q = h.samplePos(x, z);
            const double wobble = p.noise * thick * shaping_::fbm(q.x / (thick * 8.0), q.y / (thick * 8.0), p.seed, 3);
            const double layer = (h.heights[k] + glm::dot(q, dir) + wobble) / thick;
            // Hard bands are the upper part of each layer; a smooth edge keeps them from aliasing.
            const double frac = layer - std::floor(layer);
            const double hv = shaping_::smooth01((frac - 0.45) / 0.1) * (1.0 - shaping_::smooth01((frac - 0.9) / 0.1));
            hard[k] = float(hv);
            // Hard bands stand proud as benches: their top edge is pulled level, the soft rock above recedes.
            h.heights[k] = float(h.heights[k] + p.strength * thick * (hv - 0.5) * 0.5);
        }
    });
}

void fold(Heightfield& h, const FoldParams& p)
{
    LILA_ZONE();

    const double a = p.direction * shaping_::kPi / 180.0;
    const glm::dvec2 dir(std::cos(a), std::sin(a));
    const double wl = std::max(1.0, p.wavelength);
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 q = h.samplePos(x, z);
            const double bend = p.bend * wl * shaping_::fbm(q.x / (wl * 2.0), q.y / (wl * 2.0), p.seed, 3);
            const double phase = (glm::dot(q, dir) + bend) / wl * 2.0 * shaping_::kPi;
            h.heights[h.index(x, z)] = float(h.heights[h.index(x, z)] + p.amplitude * std::sin(phase));
        }
    });
}

void warp(Heightfield& h, const WarpParams& p)
{
    LILA_ZONE();

    const Heightfield src = h;
    const double fs = std::max(1.0, p.featureSize);
    std::vector<glm::dvec2> at(h.size());
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 q = h.samplePos(x, z);
            const double dx = shaping_::fbm(q.x / fs, q.y / fs, p.seed, 3);
            const double dz = shaping_::fbm(q.x / fs + 31.7, q.y / fs - 17.3, p.seed + 7u, 3);
            at[h.index(x, z)] = q + glm::dvec2(dx, dz) * p.amount;
        }
    });
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) { h.heights[k] = float(src.heightAt(at[k].x, at[k].y)); });
    for (auto& [name, values] : h.masks)
    {
        if (values.size() != h.size())
            continue;
        const std::string n = name;
        tbb::parallel_for(size_t(0), h.size(), [&](size_t k) { values[k] = float(src.maskAt(n, at[k].x, at[k].y)); });
    }
}

void ridgeSharpen(Heightfield& h, const RidgeSharpenParams& p)
{
    LILA_ZONE();

    const int r = std::max(1, int(std::lround(p.radius / h.cell)));
    const std::vector<float> blur = boxBlurred(h.heights, h.nx, h.nz, r);
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) {
        const double above = double(h.heights[k]) - double(blur[k]);
        if (above > 0.0)
            h.heights[k] = float(h.heights[k] + p.amount * above);
    });
}

void cliff(Heightfield& h, const CliffParams& p)
{
    LILA_ZONE();

    const double lo = std::tan(std::max(1.0, p.threshold - 6.0) * shaping_::kPi / 180.0);
    const double hi = std::tan(std::min(89.0, p.threshold + 6.0) * shaping_::kPi / 180.0);
    const Heightfield src = h;
    std::vector<float> c(h.size());
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
            c[h.index(x, z)] = float(shaping_::smooth01((shaping_::slopeAt(src, x, z) - lo) / std::max(1e-6, hi - lo)));
    });
    // Feathered so a band's edge never shows as a seam where the slope crosses the threshold.
    c = boxBlurred(c, h.nx, h.nz, 1, 1);
    std::vector<float>& mask = h.mask("cliff");
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t k = h.index(x, z);
            const glm::dvec2 q = h.samplePos(x, z);
            const double jitter = p.step * 0.25 * shaping_::fbm(q.x / (p.step * 6.0), q.y / (p.step * 6.0), p.seed, 2);
            const double v = src.heights[k];
            const double stepped = terraceHeight(v + jitter, p.step, 0.92, 0.0) - jitter;
            const double w = std::clamp(p.strength, 0.0, 1.0) * c[k];
            h.heights[k] = float(v + (stepped - v) * w);
            mask[k] = c[k];
        }
    });
}
} // namespace terrain
