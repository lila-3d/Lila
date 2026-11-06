// Painted inputs beyond plain brightness: removing 8-bit steps, colour-coded sketches, and drawn ridge, valley, cliff, river
// and flat constraints that erosion holds to.

#include "Painted.h"

#include "Erosion.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace lila;

namespace terrain
{
namespace painteddetail
{
/** One running box filter of radius r along rows (stride 1) or columns, clamped at the edges. */
void boxPass(std::vector<float>& v, int w, int h, int r, bool rows)
{
    const int lines = rows ? h : w, len = rows ? w : h;
    tbb::parallel_for(0, lines, [&](int line) {
        std::vector<float> lineIn(static_cast<size_t>(len)), lineOut(static_cast<size_t>(len));
        auto at = [&](int i) -> float& { return rows ? v[size_t(line) * size_t(w) + size_t(i)] : v[size_t(i) * size_t(w) + size_t(line)]; };
        for (int i = 0; i < len; ++i)
            lineIn[size_t(i)] = at(i);
        auto clampAt = [&](int i) { return double(lineIn[size_t(std::clamp(i, 0, len - 1))]); };
        double sum = 0.0;
        for (int k = -r; k <= r; ++k)
            sum += clampAt(k);
        const double inv = 1.0 / double(2 * r + 1);
        for (int i = 0; i < len; ++i)
        {
            lineOut[size_t(i)] = float(sum * inv);
            sum += clampAt(i + r + 1) - clampAt(i - r);
        }
        for (int i = 0; i < len; ++i)
            at(i) = lineOut[size_t(i)];
    });
}

double smoothFalloff(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    return 1.0 - u * u * (3.0 - 2.0 * u);
}

double bilinear(const std::vector<float>& v, const Heightfield& h, glm::dvec2 p)
{
    const double fx = std::clamp((p.x - h.origin.x) / h.cell, 0.0, double(h.nx - 1));
    const double fz = std::clamp((p.y - h.origin.y) / h.cell, 0.0, double(h.nz - 1));
    const int x0 = std::min(int(fx), h.nx - 2), z0 = std::min(int(fz), h.nz - 2);
    const double tx = fx - x0, tz = fz - z0;
    auto at = [&](int x, int z) { return double(v[h.index(x, z)]); };
    return (at(x0, z0) * (1 - tx) + at(x0 + 1, z0) * tx) * (1 - tz) + (at(x0, z0 + 1) * (1 - tx) + at(x0 + 1, z0 + 1) * tx) * tz;
}

/** Voids take the mean of the valid samples around them at the coarsest scale that has any. */
void fillFromAround(std::vector<float>& v, std::vector<uint8_t>& valid, int w, int h)
{
    if (std::all_of(valid.begin(), valid.end(), [](uint8_t b) { return b != 0; }))
        return;
    const int cw = (w + 1) / 2, chh = (h + 1) / 2;
    std::vector<float> coarse(size_t(cw) * size_t(chh), 0.0f);
    std::vector<uint8_t> cvalid(coarse.size(), 0);
    for (int y = 0; y < chh; ++y)
        for (int x = 0; x < cw; ++x)
        {
            double sum = 0.0;
            int n = 0;
            for (int j = 2 * y; j < std::min(h, 2 * y + 2); ++j)
                for (int i = 2 * x; i < std::min(w, 2 * x + 2); ++i)
                    if (valid[size_t(j) * size_t(w) + size_t(i)])
                    {
                        sum += v[size_t(j) * size_t(w) + size_t(i)];
                        ++n;
                    }
            if (n)
            {
                coarse[size_t(y) * size_t(cw) + size_t(x)] = float(sum / n);
                cvalid[size_t(y) * size_t(cw) + size_t(x)] = 1;
            }
        }
    if (cw == w && chh == h)
        return;
    fillFromAround(coarse, cvalid, cw, chh);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (!valid[size_t(y) * size_t(w) + size_t(x)])
                v[size_t(y) * size_t(w) + size_t(x)] = coarse[size_t(std::min(y / 2, chh - 1)) * size_t(cw) + size_t(std::min(x / 2, cw - 1))];
    std::fill(valid.begin(), valid.end(), uint8_t(1));
}

/** A blurred stroke mask scaled so the middle of a stroke is 1: how far its shape spreads into the land around. */
std::vector<float> spread(const std::vector<float>& mask, int w, int h, double sigma)
{
    std::vector<float> s = mask;
    gaussianApprox(s, w, h, sigma);
    double sum = 0.0;
    size_t n = 0;
    for (size_t k = 0; k < mask.size(); ++k)
        if (mask[k] > 0.5f)
        {
            sum += s[k];
            ++n;
        }
    const float inv = n && sum > 0.0 ? float(double(n) / sum) : 0.0f;
    for (float& v : s)
        v = std::min(1.0f, v * inv);
    return s;
}

const char* kindMask(ConstraintKind k)
{
    static const char* names[] = {"constraintRidge", "constraintValley", "constraintCliff", "constraintRiver", "constraintFlat"};
    return names[size_t(k)];
}
} // namespace painteddetail

using namespace painteddetail;

void gaussianApprox(std::vector<float>& v, int w, int h, double sigma)
{
    if (sigma < 0.5 || w < 1 || h < 1)
        return;
    const int r = std::max(1, int(std::lround((std::sqrt(4.0 * sigma * sigma + 1.0) - 1.0) * 0.5)));
    for (int pass = 0; pass < 3; ++pass)
    {
        boxPass(v, w, h, r, true);
        boxPass(v, w, h, r, false);
    }
}

namespace painteddetail
{
/** dequantize, reading the level size from the samples marked in only (all when null); the others are free to move. */
double dequantizeWhere(Heightfield& h, const std::vector<uint8_t>* only)
{
    if (h.empty())
        return 0.0;
    double q = 0.0;
    for (double levels : {255.0, 65535.0})
    {
        bool all = true;
        for (size_t k = 0; k < h.size() && all; ++k)
        {
            if (only && !(*only)[k])
                continue;
            const float v = h.heights[k];
            const double s = double(v) * levels;
            all = !(v < -1e-6f || v > 1.0f + 1e-6f || std::abs(s - std::round(s)) > 1e-3);
        }
        if (all)
        {
            q = 1.0 / levels;
            break;
        }
    }
    if (q == 0.0)
        return 0.0;
    std::vector<float> lo(h.size()), hi(h.size()), pinned(h.size(), std::numeric_limits<float>::quiet_NaN());
    const std::vector<float> levels = h.heights;
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t k = h.index(x, z);
            const bool painted = !only || (*only)[k];
            lo[k] = painted ? float(double(levels[k]) - q * 0.5) : -std::numeric_limits<float>::infinity();
            hi[k] = painted ? float(double(levels[k]) + q * 0.5) : std::numeric_limits<float>::infinity();
            if (!painted)
                continue;
            // Next to a step the true surface is about to cross into the neighbouring level: held a quarter level toward it.
            bool up = false, down = false;
            const int nb[4][2] = {{x - 1, z}, {x + 1, z}, {x, z - 1}, {x, z + 1}};
            for (const auto& n : nb)
            {
                if (n[0] < 0 || n[1] < 0 || n[0] >= h.nx || n[1] >= h.nz)
                    continue;
                const size_t j = h.index(n[0], n[1]);
                if (only && !(*only)[j])
                    continue;
                up = up || levels[j] > levels[k];
                down = down || levels[j] < levels[k];
            }
            if (up != down)
                pinned[k] = float(double(levels[k]) + (up ? 0.25 : -0.25) * q);
        }
    });
    // Coarse to fine: each blur turns the plateaus at its scale into ramps between the held steps; the clamp keeps every
    // sample in its level.
    const double top = std::min(128.0, double(std::max(h.nx, h.nz)) / 4.0);
    for (double sigma = top; sigma >= 0.99; sigma *= 0.5)
        for (int rep = 0; rep < 3; ++rep)
        {
            gaussianApprox(h.heights, h.nx, h.nz, sigma);
            tbb::parallel_for(size_t(0), h.size(), [&](size_t k) {
                h.heights[k] = std::isnan(pinned[k]) ? std::clamp(h.heights[k], lo[k], hi[k]) : pinned[k];
            });
        }
    // A last light pass so the held samples blend with their neighbours.
    gaussianApprox(h.heights, h.nx, h.nz, 1.0);
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) { h.heights[k] = std::clamp(h.heights[k], lo[k], hi[k]); });
    return q;
}
} // namespace painteddetail

double dequantize(Heightfield& h)
{
    LILA_ZONE();
    return dequantizeWhere(h, nullptr);
}

const std::vector<std::string>& constraintKindNames()
{
    static const std::vector<std::string> n = {"ridge", "valley", "cliff", "river", "flat"};
    return n;
}

bool parseConstraintKind(std::string_view name, ConstraintKind* out)
{
    const auto& n = constraintKindNames();
    auto it = std::find(n.begin(), n.end(), name);
    if (it == n.end())
        return false;
    *out = ConstraintKind(it - n.begin());
    return true;
}

nlohmann::json Constraint::toJson() const
{
    nlohmann::json j = {{"id", id}, {"kind", constraintKindNames()[size_t(kind)]}, {"height", height}, {"width", width}, {"strength", strength}};
    if (!object.empty())
        j["object"] = object;
    else
    {
        j["path"] = nlohmann::json::array();
        for (const glm::dvec2& p : path)
            j["path"].push_back({p.x, p.y});
    }
    return j;
}

Constraint Constraint::fromJson(const nlohmann::json& j)
{
    Constraint c;
    c.id = j.value("id", std::string());
    if (!parseConstraintKind(j.value("kind", std::string("ridge")), &c.kind))
        throw std::runtime_error("unknown constraint kind '" + j.value("kind", std::string()) + "' (ridge, valley, cliff, river, flat)");
    c.object = j.value("object", std::string());
    if (j.contains("path") && j["path"].is_array())
        for (const nlohmann::json& p : j["path"])
            if (p.is_array() && p.size() >= 2)
                c.path.push_back({p[0].get<double>(), p[p.size() == 3 ? 2 : 1].get<double>()});
    c.height = j.value("height", c.height);
    c.width = j.value("width", c.width);
    c.strength = j.value("strength", c.strength);
    return c;
}

ConstraintStats applyConstraint(Heightfield& h, const Constraint& c, const std::vector<std::vector<glm::dvec2>>& lines)
{
    LILA_ZONE();

    ConstraintStats st;
    st.lowest = std::numeric_limits<double>::infinity();
    st.highest = -std::numeric_limits<double>::infinity();
    if (h.empty())
        return st;
    const std::vector<float> h0 = h.heights;
    std::vector<float>& weight = h.mask("constraint");
    std::vector<float>& target = h.mask("constraintHeight");
    std::vector<float>& cover = h.mask(kindMask(c.kind));
    const double half = std::max(c.width * 0.5, h.cell);
    const double strength = std::clamp(c.strength, 0.0, 1.0);
    const double height = std::max(0.0, c.height);

    auto commit = [&](size_t k, double value, double f, double pinWeight) {
        h.heights[k] = float(value);
        cover[k] = std::max(cover[k], float(f));
        ++st.touched;
        if (pinWeight > double(weight[k]))
        {
            weight[k] = float(pinWeight);
            target[k] = float(value);
        }
        if (pinWeight > 0.0)
            ++st.held;
        st.lowest = std::min(st.lowest, value);
        st.highest = std::max(st.highest, value);
    };
    auto sampleRange = [&](glm::dvec2 lo, glm::dvec2 hi, int& x0, int& x1, int& z0, int& z1) {
        x0 = std::max(0, int(std::floor((lo.x - h.origin.x) / h.cell)));
        x1 = std::min(h.nx - 1, int(std::ceil((hi.x - h.origin.x) / h.cell)));
        z0 = std::max(0, int(std::floor((lo.y - h.origin.y) / h.cell)));
        z1 = std::min(h.nz - 1, int(std::ceil((hi.y - h.origin.y) / h.cell)));
    };

    for (const std::vector<glm::dvec2>& line : lines)
    {
        if (line.size() < 2)
            continue;
        if (c.kind == ConstraintKind::Flat)
        {
            double sum = 0.0;
            glm::dvec2 lo(std::numeric_limits<double>::infinity()), hi(-std::numeric_limits<double>::infinity());
            for (const glm::dvec2& p : line)
            {
                sum += bilinear(h0, h, p);
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
            const double level = sum / double(line.size());
            int x0, x1, z0, z1;
            sampleRange(lo - half, hi + half, x0, x1, z0, z1);
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                {
                    const glm::dvec2 p = h.samplePos(x, z);
                    bool inside = false;
                    double best = std::numeric_limits<double>::infinity();
                    for (size_t i = 0, j = line.size() - 1; i < line.size(); j = i++)
                    {
                        const glm::dvec2 a = line[j], b = line[i];
                        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                            inside = !inside;
                        const glm::dvec2 ab = b - a;
                        const double t = glm::dot(ab, ab) > 0.0 ? std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0, 1.0) : 0.0;
                        best = std::min(best, glm::length(p - (a + ab * t)));
                    }
                    if (!inside && best >= half)
                        continue;
                    const double f = inside ? 1.0 : smoothFalloff(best / half);
                    const size_t k = h.index(x, z);
                    commit(k, double(h.heights[k]) + (level - double(h.heights[k])) * f, f, inside ? strength : 0.0);
                }
            continue;
        }

        // Dense along the line, so the river bed and the ground under the line are known every cell.
        std::vector<glm::dvec2> pts{line.front()};
        for (size_t i = 1; i < line.size(); ++i)
        {
            const double len = glm::length(line[i] - line[i - 1]);
            const int steps = std::max(1, int(std::ceil(len / h.cell)));
            for (int s = 1; s <= steps; ++s)
                pts.push_back(line[i - 1] + (line[i] - line[i - 1]) * (double(s) / steps));
        }
        std::vector<double> along(pts.size(), 0.0), ground(pts.size()), bed(pts.size());
        for (size_t i = 0; i < pts.size(); ++i)
        {
            if (i)
                along[i] = along[i - 1] + glm::length(pts[i] - pts[i - 1]);
            ground[i] = bilinear(h0, h, pts[i]);
            bed[i] = ground[i] - height;
            if (i)
                bed[i] = std::min(bed[i], bed[i - 1]);
        }
        glm::dvec2 lo(std::numeric_limits<double>::infinity()), hi(-std::numeric_limits<double>::infinity());
        for (const glm::dvec2& p : pts)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        int x0, x1, z0, z1;
        sampleRange(lo - half, hi + half, x0, x1, z0, z1);
        const int bw = x1 - x0 + 1, bh = z1 - z0 + 1;
        if (bw <= 0 || bh <= 0)
            continue;
        std::vector<double> dist(size_t(bw) * size_t(bh), std::numeric_limits<double>::infinity()), param(dist.size(), 0.0), side(dist.size(), 0.0);
        for (size_t i = 1; i < pts.size(); ++i)
        {
            const glm::dvec2 a = pts[i - 1], b = pts[i], ab = b - a;
            const double ll = glm::dot(ab, ab);
            int sx0, sx1, sz0, sz1;
            sampleRange(glm::min(a, b) - half, glm::max(a, b) + half, sx0, sx1, sz0, sz1);
            for (int z = sz0; z <= sz1; ++z)
                for (int x = sx0; x <= sx1; ++x)
                {
                    const glm::dvec2 p = h.samplePos(x, z);
                    const double t = ll > 0.0 ? std::clamp(glm::dot(p - a, ab) / ll, 0.0, 1.0) : 0.0;
                    const double d = glm::length(p - (a + ab * t));
                    const size_t k = size_t(z - z0) * size_t(bw) + size_t(x - x0);
                    if (d < dist[k])
                    {
                        dist[k] = d;
                        param[k] = double(i - 1) + t;
                        // Left of the direction of travel, seen from above with +Z south.
                        side[k] = (ab.x * (p.y - a.y) - ab.y * (p.x - a.x)) < 0.0 ? 1.0 : -1.0;
                    }
                }
        }
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
            {
                const size_t b = size_t(z - z0) * size_t(bw) + size_t(x - x0);
                if (!(dist[b] < half))
                    continue;
                const double u = dist[b] / half, f = smoothFalloff(u);
                const size_t i0 = std::min(size_t(param[b]), pts.size() - 2);
                const double t = param[b] - double(i0);
                const double groundAt = ground[i0] + (ground[i0 + 1] - ground[i0]) * t;
                const double bedAt = bed[i0] + (bed[i0 + 1] - bed[i0]) * t;
                const size_t k = h.index(x, z);
                const double cur = h.heights[k];
                const double core = std::clamp(1.0 - u / 0.35, 0.0, 1.0);
                switch (c.kind)
                {
                case ConstraintKind::Ridge: commit(k, std::max(cur, groundAt + height * f), f, strength * core); break;
                case ConstraintKind::Valley: commit(k, std::min(cur, groundAt - height * f), f, 0.6 * strength * core); break;
                case ConstraintKind::River: commit(k, std::min(cur, bedAt + height * u * u), f, strength * std::clamp((0.6 - u) / 0.35, 0.0, 1.0)); break;
                default:
                    commit(k, cur + side[b] * height * 0.5 * (1.0 - u) * (1.0 - u), f, strength * (1.0 - u));
                    break;
                }
            }
    }
    if (!st.touched)
        st.lowest = st.highest = 0.0;
    return st;
}

Heightfield sketchTerrain(const image::Image& img, glm::dvec2 center, double width, const ImageOptions& o, SketchStats* stats)
{
    LILA_ZONE();

    if (img.width < 2 || img.height < 2)
        throw std::runtime_error("the sketch is empty");
    const int w = img.width, hgt = img.height;
    Heightfield h = geo::makeHeightfield(w, hgt, std::max(1e-6, width) / double(w - 1), center);
    h.origin = center - h.extent() * 0.5;
    const size_t n = h.size();
    std::vector<float> base(n, 0.0f);
    std::vector<uint8_t> valid(n, 0);
    std::vector<float> cls[4];
    for (auto& v : cls)
        v.assign(n, 0.0f);
    SketchStats s;
    for (size_t k = 0; k < n; ++k)
    {
        const float r = img.rgba[k * 4], g = img.rgba[k * 4 + 1], b = img.rgba[k * 4 + 2], a = img.rgba[k * 4 + 3];
        if (a < 0.5f)
            continue;
        const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), sat = hi - lo;
        if (sat < 0.25f)
        {
            base[k] = (r + g + b) / 3.0f;
            valid[k] = 1;
            ++s.grey;
            continue;
        }
        // Ridge red, valley green, river blue, flat yellow.
        int c;
        if (r >= g && r >= b)
            c = g > 0.6f * r ? 3 : 0;
        else
            c = g >= b ? 1 : 2;
        cls[c][k] = std::clamp(sat * 1.25f, 0.0f, 1.0f);
        (c == 0 ? s.ridge : c == 1 ? s.valley : c == 2 ? s.river : s.flat)++;
    }
    if (!s.grey)
        std::fill(valid.begin(), valid.end(), uint8_t(1));
    const std::vector<uint8_t> valid0 = valid;
    fillFromAround(base, valid, w, hgt);
    h.heights = base;
    // Grey under the strokes was filled in, not painted: only painted samples set and keep their levels.
    s.levels = dequantizeWhere(h, s.grey ? &valid0 : nullptr);
    const double smooth = std::max(o.smooth, 0.0);
    gaussianApprox(h.heights, w, hgt, smooth);
    const double shape = std::max(0.05, o.shape), range = o.high - o.low;
    for (float& v : h.heights)
        v = float(o.low + range * std::pow(std::clamp(double(v), 0.0, 1.0), shape));

    const double scale = double(std::max(w, hgt));
    std::vector<float>& weight = h.mask("constraint");
    std::vector<float>& target = h.mask("constraintHeight");
    const char* names[4] = {"constraintRidge", "constraintValley", "constraintRiver", "constraintFlat"};
    const double amount[4] = {0.35, -0.25, -0.12, 0.0};
    const double reach[4] = {scale / 40.0, scale / 40.0, scale / 120.0, scale / 30.0};
    const double hold[4] = {1.0, 0.6, 1.0, 1.0};
    for (int c = 0; c < 4; ++c)
    {
        std::vector<float> m = cls[c];
        gaussianApprox(m, w, hgt, 1.0);
        h.mask(names[c]) = m;
        if (!std::any_of(cls[c].begin(), cls[c].end(), [](float v) { return v > 0.0f; }))
            continue;
        if (c == 3)
        {
            std::vector<float> level = h.heights;
            gaussianApprox(level, w, hgt, reach[c]);
            for (size_t k = 0; k < n; ++k)
                h.heights[k] += (level[k] - h.heights[k]) * m[k];
        }
        else
        {
            const std::vector<float> sp = spread(cls[c], w, hgt, reach[c]);
            for (size_t k = 0; k < n; ++k)
                h.heights[k] += float(amount[c] * std::abs(range) * double(sp[k]));
        }
        for (size_t k = 0; k < n; ++k)
            if (float wt = float(hold[c]) * m[k]; wt > weight[k])
                weight[k] = wt;
    }
    target = h.heights;

    const int iterations = int(std::lround(std::clamp(o.erosion, 0.0, 1.0) * 150.0));
    if (iterations > 0)
    {
        NoiseParams np;
        np.featureSize = std::max(h.extent().x, h.extent().y) / 12.0;
        np.amplitude = std::abs(range) * o.detail;
        np.octaves = 5;
        np.seed = o.seed;
        if (np.amplitude > 0.0)
            tbb::parallel_for(0, h.nz, [&](int z) {
                for (int x = 0; x < h.nx; ++x)
                {
                    const glm::dvec2 p = h.samplePos(x, z);
                    h.heights[h.index(x, z)] += float(noiseHeight(np, p.x, p.y) - np.amplitude * 0.5);
                }
            });
        ErosionParams p;
        p.iterations = iterations;
        erode(h, p);
    }
    if (stats)
        *stats = s;
    return h;
}
} // namespace terrain
