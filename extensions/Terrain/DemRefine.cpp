// Refining real-world elevation: bare-earth smoothing of surface models and multi-scale erosion amplification of coarse data.

#include "DemRefine.h"

#include "Erosion.h"
#include "Painted.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <deque>

using namespace lila;

namespace terrain
{
namespace refinedetail
{
/** Square min (or max) filter of radius r, separable, with a monotonic queue: O(n) at any radius. */
void rankFilter(std::vector<float>& v, int w, int h, int r, bool takeMax)
{
    auto pass = [&](bool rows) {
        const int lines = rows ? h : w, len = rows ? w : h;
        tbb::parallel_for(0, lines, [&](int line) {
            auto idx = [&](int i) { return rows ? size_t(line) * size_t(w) + size_t(i) : size_t(i) * size_t(w) + size_t(line); };
            std::vector<float> lineIn(static_cast<size_t>(len)), lineOut(static_cast<size_t>(len));
            for (int i = 0; i < len; ++i)
                lineIn[size_t(i)] = v[idx(i)];
            std::deque<int> q;
            auto better = [&](float a, float b) { return takeMax ? a >= b : a <= b; };
            int next = 0;
            for (int i = 0; i < len; ++i)
            {
                for (; next <= std::min(len - 1, i + r); ++next)
                {
                    while (!q.empty() && better(lineIn[size_t(next)], lineIn[size_t(q.back())]))
                        q.pop_back();
                    q.push_back(next);
                }
                while (q.front() < i - r)
                    q.pop_front();
                lineOut[size_t(i)] = lineIn[size_t(q.front())];
            }
            for (int i = 0; i < len; ++i)
                v[idx(i)] = lineOut[size_t(i)];
        });
    };
    pass(true);
    pass(false);
}
} // namespace refinedetail

using namespace refinedetail;

BareEarthStats bareEarth(Heightfield& h, const BareEarthParams& p)
{
    LILA_ZONE();

    BareEarthStats st;
    if (h.empty() || p.radius <= 0.0 || p.maxHeight <= 0.0)
        return st;
    const int r = std::max(1, int(std::lround(p.radius / h.cell)));
    std::vector<float> opened = h.heights;
    rankFilter(opened, h.nx, h.nz, r, false);
    rankFilter(opened, h.nx, h.nz, r, true);
    std::vector<float> ground = opened;
    gaussianApprox(ground, h.nx, h.nz, double(r) * 0.5);
    const std::vector<float>* where = p.mask.empty() ? nullptr : h.findMask(p.mask);
    std::vector<float>& removed = h.mask("bareEarth");
    std::vector<float> drop(h.size(), 0.0f);
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t k = h.index(x, z);
            const double target = std::min(double(h.heights[k]), double(ground[k]));
            const double stand = double(h.heights[k]) - target;
            if (stand <= 0.0)
                continue;
            double wgt = std::clamp((1.5 * p.maxHeight - stand) / (0.5 * p.maxHeight), 0.0, 1.0);
            if (where)
                wgt *= std::clamp(double((*where)[k]), 0.0, 1.0);
            drop[k] = float(stand * wgt);
        }
    });
    double sum = 0.0;
    for (size_t k = 0; k < h.size(); ++k)
    {
        if (drop[k] <= 0.0f)
            continue;
        h.heights[k] -= drop[k];
        removed[k] = float(std::min(1.0, double(drop[k]) / p.maxHeight));
        ++st.lowered;
        st.most = std::max(st.most, double(drop[k]));
        sum += drop[k];
    }
    st.mean = st.lowered ? sum / double(st.lowered) : 0.0;
    return st;
}

Heightfield amplify(const Heightfield& src, const AmplifyParams& p)
{
    LILA_ZONE();

    Heightfield h = src;
    int factor = 1;
    while (factor < std::clamp(p.factor, 1, 16))
        factor *= 2;
    for (int level = 0; (1 << level) < factor; ++level)
    {
        const double coarse = h.cell;
        if (uint64_t(2 * h.nx - 1) * uint64_t(2 * h.nz - 1) > (uint64_t(1) << 28))
            throw std::runtime_error("amplifying to " + std::to_string(2 * h.nx - 1) + " x " + std::to_string(2 * h.nz - 1) + " samples is too many");
        Heightfield up = geo::resampled(h, 2 * h.nx - 1, 2 * h.nz - 1, coarse * 0.5, h.origin);
        const std::vector<float> base = up.heights;
        NoiseParams np;
        np.featureSize = coarse * 3.0;
        np.amplitude = 1.0;
        np.octaves = 3;
        np.ridged = 0.7;
        np.seed = p.seed + uint32_t(level) * 7919u;
        tbb::parallel_for(0, up.nz, [&](int z) {
            for (int x = 0; x < up.nx; ++x)
            {
                const glm::dvec2 at = up.samplePos(x, z);
                const glm::dvec3 n = h.normalAt(at.x, at.y);
                const double slope = std::sqrt(std::max(0.0, 1.0 - n.y * n.y)) / std::max(0.05, n.y);
                up.heights[up.index(x, z)] += float((noiseHeight(np, at.x, at.y) - 0.5) * p.detail * slope * coarse);
            }
        });
        ErosionParams ep;
        ep.iterations = std::max(0, p.iterations);
        ep.thermalIterations = 1;
        if (ep.iterations > 0)
            erode(up, ep);
        // Keep the coarse data's shape: the new heights contribute only what is finer than a coarse cell.
        std::vector<float> low = up.heights, baseLow = base;
        gaussianApprox(low, up.nx, up.nz, 2.0);
        gaussianApprox(baseLow, up.nx, up.nz, 2.0);
        tbb::parallel_for(size_t(0), up.size(), [&](size_t k) { up.heights[k] = up.heights[k] - low[k] + baseLow[k]; });
        h = std::move(up);
    }
    return h;
}
} // namespace terrain
