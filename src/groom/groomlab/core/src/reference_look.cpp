// SPDX-License-Identifier: Apache-2.0
// Guides from reference photos, the measured look (6-10): clumps and color variation from profiles
// across the flow, flyaways and frizz from the Hessian's ridges. See reference_guides.h; the
// aggregation over the views and the graph values are in reference_guides.cpp (fitLook).

#include <algorithm>
#include <cmath>
#include <vector>

#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/reference_guides.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float median(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

// A 1D Gaussian blur (clamped ends).
std::vector<float> blur1(const std::vector<float>& in, float sigma) {
    const int r = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<float> k(static_cast<std::size_t>(2 * r + 1));
    float sum = 0.0f;
    for (int i = -r; i <= r; ++i) sum += k[static_cast<std::size_t>(i + r)] = pm::exp(-0.5f * static_cast<float>(i * i) / (sigma * sigma));
    for (float& x : k) x /= sum;
    const int n = static_cast<int>(in.size());
    std::vector<float> out(in.size(), 0.0f);
    for (int i = 0; i < n; ++i) {
        float acc = 0.0f;
        for (int j = -r; j <= r; ++j) acc += k[static_cast<std::size_t>(j + r)] * in[static_cast<std::size_t>(std::clamp(i + j, 0, n - 1))];
        out[static_cast<std::size_t>(i)] = acc;
    }
    return out;
}

// The line through the samples taken out (a gradient of the light across the profile).
void detrend(std::vector<float>& v) {
    const double n = static_cast<double>(v.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        const double x = static_cast<double>(i);
        sx += x;
        sy += v[i];
        sxx += x * x;
        sxy += x * v[i];
    }
    const double d = n * sxx - sx * sx;
    const double b = std::abs(d) > 1e-12 ? (n * sxy - sx * sy) / d : 0.0, a = (sy - b * sx) / n;
    for (std::size_t i = 0; i < v.size(); ++i) v[i] -= static_cast<float>(a + b * static_cast<double>(i));
}

double stddev(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double m = 0, s = 0;
    for (float x : v) m += x;
    m /= static_cast<double>(v.size());
    for (float x : v) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(v.size()));
}

// The autocorrelation's first peak past its first zero (as measureCurl reads a period): the lag and
// its value; lag -1 when none.
std::pair<int, double> firstPeak(const std::vector<float>& line, int minLag) {
    double mean = 0.0;
    for (float a : line) mean += a;
    mean /= static_cast<double>(line.size());
    double var = 0.0;
    for (float a : line) var += (a - mean) * (a - mean);
    var /= static_cast<double>(line.size());
    if (var < 1e-12) return {-1, 0.0};
    const int maxLag = static_cast<int>(line.size()) / 2;
    std::vector<double> ac(static_cast<std::size_t>(maxLag) + 2, 0.0);
    for (int lag = 0; lag <= maxLag + 1 && lag < static_cast<int>(line.size()); ++lag) {
        double sum = 0.0;
        for (std::size_t i = 0; i + static_cast<std::size_t>(lag) < line.size(); ++i)
            sum += (line[i] - mean) * (line[i + static_cast<std::size_t>(lag)] - mean);
        ac[static_cast<std::size_t>(lag)] = sum / (static_cast<double>(line.size() - static_cast<std::size_t>(lag)) * var);
    }
    int lag = 1;
    while (lag < maxLag && ac[static_cast<std::size_t>(lag)] > 0.0) ++lag;
    for (int k = std::max(lag + 1, minLag); k < maxLag; ++k)
        if (ac[static_cast<std::size_t>(k)] > ac[static_cast<std::size_t>(k - 1)] && ac[static_cast<std::size_t>(k)] >= ac[static_cast<std::size_t>(k + 1)])
            return {k, ac[static_cast<std::size_t>(k)]};
    return {-1, 0.0};
}

bool inside(const RefImage& mask, int w, int h, float u, float v) {
    if (u < 0 || v < 0 || u >= static_cast<float>(w) || v >= static_cast<float>(h)) return false;
    return mask.empty() || mask.sample(u * static_cast<float>(mask.width) / static_cast<float>(w),
                                       v * static_cast<float>(mask.height) / static_cast<float>(h)) >= 0.5f;
}

// A one-channel mask at w x h as bytes (resampled when its size differs).
std::vector<std::uint8_t> maskBytes(const RefImage& mask, int w, int h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 1);
    if (mask.empty()) return out;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out[static_cast<std::size_t>(y * w + x)] = inside(mask, w, h, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) ? 1 : 0;
    return out;
}

// Max (grow) or min (shrink) over a (2r+1) square, separably.
std::vector<std::uint8_t> morph(const std::vector<std::uint8_t>& in, int w, int h, int r, bool grow) {
    std::vector<std::uint8_t> tmp(in.size()), out(in.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t v = grow ? 0 : 1;
            for (int i = std::max(0, x - r); i <= std::min(w - 1, x + r); ++i) {
                const std::uint8_t s = in[static_cast<std::size_t>(y * w + i)];
                v = grow ? std::max(v, s) : std::min(v, s);
            }
            tmp[static_cast<std::size_t>(y * w + x)] = v;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t v = grow ? 0 : 1;
            for (int j = std::max(0, y - r); j <= std::min(h - 1, y + r); ++j) {
                const std::uint8_t s = tmp[static_cast<std::size_t>(j * w + x)];
                v = grow ? std::max(v, s) : std::min(v, s);
            }
            out[static_cast<std::size_t>(y * w + x)] = v;
        }
    return out;
}

// A 2D Gaussian blur of one channel.
std::vector<float> blur2(const std::vector<float>& in, int w, int h, float sigma) {
    const int r = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<float> k(static_cast<std::size_t>(2 * r + 1));
    float sum = 0.0f;
    for (int i = -r; i <= r; ++i) sum += k[static_cast<std::size_t>(i + r)] = pm::exp(-0.5f * static_cast<float>(i * i) / (sigma * sigma));
    for (float& x : k) x /= sum;
    std::vector<float> tmp(in.size()), out(in.size());
    parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t yy) {
        const int y = static_cast<int>(yy);
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int j = -r; j <= r; ++j) acc += k[static_cast<std::size_t>(j + r)] * in[static_cast<std::size_t>(y * w + std::clamp(x + j, 0, w - 1))];
            tmp[static_cast<std::size_t>(y * w + x)] = acc;
        }
    }, 16);
    parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t yy) {
        const int y = static_cast<int>(yy);
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int j = -r; j <= r; ++j) acc += k[static_cast<std::size_t>(j + r)] * tmp[static_cast<std::size_t>(std::clamp(y + j, 0, h - 1) * w + x)];
            out[static_cast<std::size_t>(y * w + x)] = acc;
        }
    }, 16);
    return out;
}

}  // namespace

ClumpEstimate measureClumps(const RefImage& luminance, const OrientationField& coarse, const RefImage& mask) {
    ClumpEstimate est;
    const int w = coarse.width, h = coarse.height;
    if (luminance.empty() || w <= 0 || h <= 0) return est;
    auto lum = [&](float u, float v) {
        return luminance.sample(u * static_cast<float>(luminance.width) / static_cast<float>(w),
                                v * static_cast<float>(luminance.height) / static_cast<float>(h));
    };
    const int spacing = std::max(6, std::max(w, h) / 48);
    const int half = std::max(32, std::max(w, h) / 10);  // samples each way across the flow (1 pixel apart)
    std::vector<float> periods, contrasts, spreads, peaks, coherent;
    const float shift = 6.0f;  // pixels along the flow to the second profile
    for (int sy = spacing / 2; sy < h; sy += spacing)
        for (int sx = spacing / 2; sx < w; sx += spacing) {
            const float u0 = static_cast<float>(sx) + 0.5f, v0 = static_cast<float>(sy) + 0.5f;
            float cu = 0, cv = 0, cc = 0;
            if (!inside(mask, w, h, u0, v0) || !coarse.sample(u0, v0, cu, cv, cc) || cc < 0.1f) continue;
            const float pu = -cv, pv = cu;  // across the flow (a straight line: the flow bends slowly)
            std::vector<float> back, fwd;
            for (int dir = -1; dir <= 1; dir += 2)
                for (int k = 1; k <= half; ++k) {
                    const float u = u0 + pu * static_cast<float>(k * dir), v = v0 + pv * static_cast<float>(k * dir);
                    if (!inside(mask, w, h, u, v)) break;
                    (dir < 0 ? back : fwd).push_back(pm::log(std::max(lum(u, v), 1e-3f)));
                }
            std::vector<float> line(back.rbegin(), back.rend());
            line.push_back(pm::log(std::max(lum(u0, v0), 1e-3f)));
            line.insert(line.end(), fwd.begin(), fwd.end());
            if (line.size() < 40) continue;
            ++est.profiles;
            // Finer than ~2 pixels: the strands; smoother: the clumps (the light's gradient taken out).
            const std::vector<float> smooth = blur1(line, 1.5f);
            std::vector<float> fine(line.size());
            for (std::size_t i = 0; i < line.size(); ++i) fine[i] = line[i] - smooth[i];
            spreads.push_back(static_cast<float>(stddev(fine)));
            {
                // The same profile a few pixels along the flow: what of the fine swing it shares runs along
                // the strands (their own colors), the rest is light and shading.
                const float ou = u0 + cu * shift, ov = v0 + cv * shift;
                const int start = -static_cast<int>(back.size());
                std::vector<float> other;
                bool ok = true;
                for (int k = start; k < start + static_cast<int>(line.size()); ++k) {
                    const float u = ou + pu * static_cast<float>(k), v = ov + pv * static_cast<float>(k);
                    if (!inside(mask, w, h, u, v)) {
                        ok = false;
                        break;
                    }
                    other.push_back(pm::log(std::max(lum(u, v), 1e-3f)));
                }
                if (ok) {
                    const std::vector<float> os = blur1(other, 1.5f);
                    double cov = 0.0;
                    for (std::size_t i = 0; i < other.size(); ++i) cov += fine[i] * (other[i] - os[i]);
                    cov /= static_cast<double>(other.size());
                    coherent.push_back(static_cast<float>(std::sqrt(std::max(0.0, cov))));
                }
            }
            std::vector<float> band = smooth;
            detrend(band);
            // Shading over the head (wider than a few clumps) out as well.
            const std::vector<float> wide = blur1(band, std::max(8.0f, static_cast<float>(line.size()) / 6.0f));
            for (std::size_t i = 0; i < band.size(); ++i) band[i] -= wide[i];
            contrasts.push_back(static_cast<float>(stddev(band)));
            const auto [lag, value] = firstPeak(band, 4);
            if (lag > 0 && value > 0.1) {
                periods.push_back(static_cast<float>(lag));
                peaks.push_back(static_cast<float>(value));
            }
        }
    if (est.profiles) est.periodicShare = static_cast<float>(periods.size()) / static_cast<float>(est.profiles);
    est.spacingPx = median(periods);
    est.contrast = median(contrasts);
    est.strandSpread = median(spreads);
    est.clumpPeak = median(peaks);
    est.coherentSpread = median(coherent);
    return est;
}

StrayEstimate measureStrays(const RefImage& gray, const OrientationField& coarse, const RefImage& mask, const RefImage& near,
                            std::vector<std::uint8_t>* picture) {
    StrayEstimate est;
    const int w = coarse.width, h = coarse.height;
    if (gray.empty() || w <= 0 || h <= 0) return est;
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> g(pixels);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            g[static_cast<std::size_t>(y * w + x)] = gray.sample((static_cast<float>(x) + 0.5f) * static_cast<float>(gray.width) / static_cast<float>(w),
                                                                 (static_cast<float>(y) + 0.5f) * static_cast<float>(gray.height) / static_cast<float>(h));
    // The Hessian at a strand's scale: its eigenvalues |l1| >= |l2|; a line where |l1| is large and
    // |l2| small (Frangi), running along l2's eigenvector.
    const std::vector<float> b = blur2(g, w, h, 1.0f);
    std::vector<float> ridge(pixels, 0.0f), along(pixels, 0.0f);
    parallelFor(1, static_cast<std::size_t>(std::max(1, h - 1)), [&](std::size_t yy) {
        const int y = static_cast<int>(yy);
        for (int x = 1; x + 1 < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y * w + x);
            const float c = b[i];
            const float xx = b[i - 1] - 2.0f * c + b[i + 1];
            const float yv = b[i - static_cast<std::size_t>(w)] - 2.0f * c + b[i + static_cast<std::size_t>(w)];
            const float xy = 0.25f * (b[i + static_cast<std::size_t>(w) + 1] - b[i + static_cast<std::size_t>(w) - 1] -
                                      b[i - static_cast<std::size_t>(w) + 1] + b[i - static_cast<std::size_t>(w) - 1]);
            const float tr = 0.5f * (xx + yv), d = std::sqrt(0.25f * (xx - yv) * (xx - yv) + xy * xy);
            float l1 = tr + d, l2 = tr - d;
            if (std::abs(l2) > std::abs(l1)) std::swap(l1, l2);
            ridge[i] = std::max(0.0f, std::abs(l1) - 2.0f * std::abs(l2));
            // l1's eigenvector (across the line): (xy, l1 - xx) or (l1 - yv, xy); the line is at 90 degrees.
            float ex = xy, ey = l1 - xx;
            if (std::abs(ex) + std::abs(ey) < 1e-9f) {
                ex = l1 - yv;
                ey = xy;
            }
            along[i] = pm::atan2(ey, ex) + 0.5f * kPi;
            // A line, not an edge: both sides across it differ from it the same way (an edge's
            // second derivative is as strong, but one side is darker and the other lighter).
            const float len = std::sqrt(ex * ex + ey * ey);
            if (ridge[i] > 0.0f && len > 1e-9f) {
                auto at = [&](float px, float py) {
                    const int qx = std::clamp(static_cast<int>(std::lround(px)), 0, w - 1), qy = std::clamp(static_cast<int>(std::lround(py)), 0, h - 1);
                    return b[static_cast<std::size_t>(qy * w + qx)];
                };
                const float nx = 2.0f * ex / len, ny = 2.0f * ey / len;
                const float s1 = at(static_cast<float>(x) + nx, static_cast<float>(y) + ny) - c;
                const float s2 = at(static_cast<float>(x) - nx, static_cast<float>(y) - ny) - c;
                if (s1 * s2 <= 0.0f || std::min(std::abs(s1), std::abs(s2)) < 0.35f * std::max(std::abs(s1), std::abs(s2))) ridge[i] = 0.0f;
            }
        }
    }, 16);
    const std::vector<std::uint8_t> m = maskBytes(mask, w, h);
    // The body: the mask opened (thin strays off) and closed; its outline.
    const int r = std::max(2, std::max(w, h) / 256);
    const std::vector<std::uint8_t> body = morph(morph(morph(morph(m, w, h, r, false), w, h, r, true), w, h, r, true), w, h, r, false);
    const std::vector<std::uint8_t> inner = morph(body, w, h, 2 * r, false), outer = morph(body, w, h, r, true);
    const std::vector<std::uint8_t> reach = morph(body, w, h, std::max(8, std::max(w, h) / 16), true);
    std::vector<std::uint8_t> nearBytes = near.empty() ? std::vector<std::uint8_t>(pixels, 1) : maskBytes(near, w, h);
    // The backdrop: what the image's border reaches around the body (not a hole in it: a face).
    {
        std::vector<std::uint8_t> seen(pixels, 0);
        std::vector<std::size_t> stack;
        for (int x = 0; x < w; ++x)
            for (int y : {0, h - 1}) stack.push_back(static_cast<std::size_t>(y * w + x));
        for (int y = 0; y < h; ++y)
            for (int x : {0, w - 1}) stack.push_back(static_cast<std::size_t>(y * w + x));
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            if (seen[i] || outer[i]) continue;
            seen[i] = 1;
            const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
            if (x > 0) stack.push_back(i - 1);
            if (x + 1 < w) stack.push_back(i + 1);
            if (y > 0) stack.push_back(i - static_cast<std::size_t>(w));
            if (y + 1 < h) stack.push_back(i + static_cast<std::size_t>(w));
        }
        for (std::size_t i = 0; i < pixels; ++i) nearBytes[i] = nearBytes[i] && seen[i];
    }
    // A ridge: a line showing at least a share of the hair's contrast against the backdrop (a lone
    // strand covers part of its pixel), and above the photo's own texture inside the hair.
    std::vector<float> hairGray, backGray, insideRidge;
    for (std::size_t i = 0; i < pixels; i += 2) {
        if (inner[i]) {
            hairGray.push_back(g[i]);
            insideRidge.push_back(ridge[i]);
        } else if (!outer[i] && reach[i] && nearBytes[i]) {
            backGray.push_back(g[i]);
        }
    }
    est.contrast = std::abs(median(hairGray) - median(backGray));
    const float threshold = std::max({0.002f, 0.04f * est.contrast, 1.5f * median(insideRidge)});
    double outline = 0, stray = 0, ridged = 0, crossing = 0, innerCount = 0;
    if (picture) {
        picture->assign(pixels * 3, 0);
        for (std::size_t i = 0; i < pixels; ++i)
            for (int c = 0; c < 3; ++c) (*picture)[3 * i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(std::clamp(g[i], 0.0f, 1.0f) * 90.0f);
    }
    auto paint = [&](std::size_t i, std::uint8_t r, std::uint8_t gg, std::uint8_t bb) {
        if (!picture) return;
        (*picture)[3 * i] = r;
        (*picture)[3 * i + 1] = gg;
        (*picture)[3 * i + 2] = bb;
    };
    for (int y = 1; y + 1 < h; ++y)
        for (int x = 1; x + 1 < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y * w + x);
            if (body[i] && (!body[i - 1] || !body[i + 1] || !body[i - static_cast<std::size_t>(w)] || !body[i + static_cast<std::size_t>(w)])) {
                ++outline;
                paint(i, 255, 255, 255);
            }
            const bool isRidge = ridge[i] > threshold;
            if (!outer[i] && reach[i] && nearBytes[i]) {
                if (isRidge) {
                    ++stray;
                    paint(i, 255, 40, 40);
                }
            } else if (inner[i]) {
                ++innerCount;
                if (!isRidge) continue;
                ++ridged;
                paint(i, 40, 200, 40);
                float cu = 0, cv = 0, cc = 0;
                if (!coarse.sample(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, cu, cv, cc) || cc < 0.1f) continue;
                float d = std::abs(pm::atan2(cv, cu) - along[i]);
                d = std::fmod(d, kPi);
                d = std::min(d, kPi - d);
                if (d > kPi / 6.0f) {
                    ++crossing;
                    paint(i, 230, 220, 40);
                }
            }
        }
    est.outlinePx = static_cast<float>(outline);
    est.strayPx = static_cast<float>(stray);
    est.strayDensity = outline > 0 ? static_cast<float>(stray / outline) : 0.0f;
    est.ridgeShare = innerCount > 0 ? static_cast<float>(ridged / innerCount) : 0.0f;
    est.crossShare = ridged > 0 ? static_cast<float>(crossing / ridged) : 0.0f;
    return est;
}

float densityOfCoverage(float coverage) {
    // Calibrated on the look sweep's thinned creature (look_sweep.py): see kDensityGamma.
    constexpr float kFull = 0.97f, kDensityGamma = 2.0f;
    return std::clamp(pm::pow(std::clamp(coverage / kFull, 0.0f, 1.0f), kDensityGamma), 0.0f, 1.0f);
}

}  // namespace groomlab
