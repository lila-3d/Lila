#include "bake/MapMath.h"
#include "bake/MapsInternal.h"

#include "bake/Backend.h"
#include "bake/BakeInputs.h"
#include "bake/HeightNormal.h"
#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <limits>

/** Maps derived from the baked normals with no new rays: normal to height and detail normal (normal to curvature lives with curvature). */
namespace lila::bake
{
using namespace mapmath;

namespace
{
constexpr float kNoLink = std::numeric_limits<float>::quiet_NaN();

/**
 * Height differences between neighbouring texels: gx[i] is h(x + 1) - h(x), gy[i] is h(y + 1) - h(y), NaN where the
 * two aren't linked (uncovered, off the edge or across a seam).
 */
struct Poisson
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> mask;
    std::vector<float> gx;
    std::vector<float> gy;

    size_t at(int x, int y) const { return size_t(y) * size_t(width) + size_t(x); }
};

Poisson coarsen(const Poisson& f)
{
    Poisson c;
    c.width = (f.width + 1) / 2;
    c.height = (f.height + 1) / 2;
    const size_t n = size_t(c.width) * size_t(c.height);
    c.mask.assign(n, 0);
    c.gx.assign(n, kNoLink);
    c.gy.assign(n, kNoLink);
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x)
            if (f.mask[f.at(x, y)])
                c.mask[c.at(x / 2, y / 2)] = 1;
    // A coarse step spans two fine ones: twice the mean of the fine links around it.
    for (int Y = 0; Y < c.height; ++Y)
        for (int X = 0; X < c.width; ++X)
        {
            float sx = 0.0f, sy = 0.0f;
            int nx = 0, ny = 0;
            for (int a = 0; a < 2; ++a)
                for (int k = 0; k < 3; ++k)
                {
                    const int fx = 2 * X + k, fy = 2 * Y + a;
                    if (fx < f.width && fy < f.height && !std::isnan(f.gx[f.at(fx, fy)]))
                        sx += f.gx[f.at(fx, fy)], ++nx;
                    const int gx = 2 * X + a, gy = 2 * Y + k;
                    if (gx < f.width && gy < f.height && !std::isnan(f.gy[f.at(gx, gy)]))
                        sy += f.gy[f.at(gx, gy)], ++ny;
                }
            if (nx && X + 1 < c.width && c.mask[c.at(X, Y)] && c.mask[c.at(X + 1, Y)])
                c.gx[c.at(X, Y)] = 2.0f * sx / float(nx);
            if (ny && Y + 1 < c.height && c.mask[c.at(X, Y)] && c.mask[c.at(X, Y + 1)])
                c.gy[c.at(X, Y)] = 2.0f * sy / float(ny);
        }
    return c;
}

void relax(const Poisson& p, std::vector<float>& h, int iterations)
{
    const float omega = 1.5f;
    for (int it = 0; it < iterations; ++it)
        for (int y = 0; y < p.height; ++y)
            for (int x = 0; x < p.width; ++x)
            {
                const size_t i = p.at(x, y);
                if (!p.mask[i])
                    continue;
                float sum = 0.0f;
                int n = 0;
                if (!std::isnan(p.gx[i]))
                    sum += h[p.at(x + 1, y)] - p.gx[i], ++n;
                if (x > 0 && !std::isnan(p.gx[p.at(x - 1, y)]))
                    sum += h[p.at(x - 1, y)] + p.gx[p.at(x - 1, y)], ++n;
                if (!std::isnan(p.gy[i]))
                    sum += h[p.at(x, y + 1)] - p.gy[i], ++n;
                if (y > 0 && !std::isnan(p.gy[p.at(x, y - 1)]))
                    sum += h[p.at(x, y - 1)] + p.gy[p.at(x, y - 1)], ++n;
                if (n)
                    h[i] += omega * (sum / float(n) - h[i]);
            }
}

/** Least-squares heights for the links, coarse to fine. */
std::vector<float> solvePoisson(const Poisson& p, int iterations)
{
    std::vector<float> h(p.mask.size(), 0.0f);
    if (p.width > 32 && p.height > 32)
    {
        const Poisson c = coarsen(p);
        const std::vector<float> hc = solvePoisson(c, iterations);
        for (int y = 0; y < p.height; ++y)
            for (int x = 0; x < p.width; ++x)
                h[p.at(x, y)] = hc[c.at(x / 2, y / 2)];
        relax(p, h, iterations);
    }
    else
        relax(p, h, std::max(iterations, 400));
    return h;
}

void normalToHeightFinalize(const MapSettings& settings, const BakeInputs&, TextureBuffer& tex, json& meta)
{
    LILA_ZONE();

    Poisson p;
    p.width = tex.width;
    p.height = tex.height;
    p.mask = tex.covered;
    p.gx.assign(tex.texelCount(), kNoLink);
    p.gy.assign(tex.texelCount(), kNoLink);
    // A slope jump this large between neighbours is a UV seam, not geometry.
    const float seam = 1.0f;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
        {
            const size_t i = p.at(x, y);
            if (!p.mask[i])
                continue;
            const glm::vec4 a = tex.value[i];
            if (x + 1 < p.width && p.mask[p.at(x + 1, y)])
            {
                const glm::vec4 b = tex.value[p.at(x + 1, y)];
                if (std::abs(a.x - b.x) < seam)
                    p.gx[i] = -0.5f * (a.x * a.w + b.x * b.w) / float(p.width);
            }
            if (y + 1 < p.height && p.mask[p.at(x, y + 1)])
            {
                const glm::vec4 b = tex.value[p.at(x, y + 1)];
                if (std::abs(a.y - b.y) < seam)
                    p.gy[i] = 0.5f * (a.y * a.w + b.y * b.w) / float(p.height);
            }
        }
    std::vector<float> h = solvePoisson(p, std::clamp(settings.options.value("iterations", 60), 1, 10000));

    // Each linked region (a UV island) floats free: centre it on the low.
    std::vector<int32_t> comp(h.size(), -1);
    std::vector<size_t> stack;
    for (size_t s = 0; s < h.size(); ++s)
    {
        if (!p.mask[s] || comp[s] >= 0)
            continue;
        std::vector<size_t> members;
        stack.assign(1, s);
        comp[s] = 1;
        while (!stack.empty())
        {
            const size_t i = stack.back();
            stack.pop_back();
            members.push_back(i);
            const int x = int(i % size_t(p.width)), y = int(i / size_t(p.width));
            auto visit = [&](int nx, int ny, float link) {
                if (std::isnan(link))
                    return;
                const size_t j = p.at(nx, ny);
                if (comp[j] < 0)
                {
                    comp[j] = 1;
                    stack.push_back(j);
                }
            };
            if (x + 1 < p.width)
                visit(x + 1, y, p.gx[i]);
            if (x > 0)
                visit(x - 1, y, p.gx[p.at(x - 1, y)]);
            if (y + 1 < p.height)
                visit(x, y + 1, p.gy[i]);
            if (y > 0)
                visit(x, y - 1, p.gy[p.at(x, y - 1)]);
        }
        double mean = 0.0;
        for (size_t i : members)
            mean += h[i];
        mean /= double(members.size());
        for (size_t i : members)
            h[i] -= float(mean);
    }

    double range = settings.options.value("range", 0.0);
    const bool automatic = !(range > 0.0);
    if (automatic)
    {
        range = percentileAbs(h, tex.covered, 1.0f);
        if (!(range > 0.0))
            range = 1.0;
    }
    for (size_t i = 0; i < tex.value.size(); ++i)
    {
        const float v = tex.covered[i] ? float(std::clamp(0.5 + double(h[i]) / (2.0 * range), 0.0, 1.0)) : 0.5f;
        tex.value[i] = glm::vec4(v, v, v, 1.0f);
    }
    meta["heightRange"] = range;
    meta["heightAuto"] = automatic;
    meta["decode"] = "height = (value - 0.5) * 2 * heightRange (cm, each UV island centred on 0)";
}

/** The tangent normal (OpenGL, unit) in xyz and the signed height above the low (cm) in w, for detailNormalFinalize. */
void detailNormalKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                        std::span<uint8_t> valid)
{
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const TexelSample& s = samples[i];
        const glm::vec3 n = unit(v3(s.normal));
        const bool hit = (hits[i].flags & kHitValid) != 0;
        const glm::vec3 h = hit ? hitNormal(ctx.prepared, ctx, hits[i]) : n;
        const glm::vec3 e = encodeTangentNormal(h, n, glm::vec4(s.tangent[0], s.tangent[1], s.tangent[2], s.tangent[3]), true);
        out[i] = glm::vec4(e, hit ? hits[i].height : 0.0f);
        valid[i] = 1;
    }
}

/** World cm per UV unit of the lows: the root of their summed surface area over their summed UV area. */
double measuredUvSize(const BakeInputs& inputs)
{
    double area = 0.0, uvArea = 0.0;
    for (const GroupInputs& g : inputs.groups)
        for (const LowInput& low : g.lows)
        {
            const MeshSoup& m = low.soup;
            if (m.uvs.size() != m.positions.size())
                continue;
            for (size_t c = 0; c + 2 < m.positions.size(); c += 3)
            {
                area += 0.5 * double(glm::length(glm::cross(m.positions[c + 1] - m.positions[c], m.positions[c + 2] - m.positions[c])));
                const glm::vec2 a = m.uvs[c + 1] - m.uvs[c], b = m.uvs[c + 2] - m.uvs[c];
                uvArea += 0.5 * std::abs(double(a.x * b.y - a.y * b.x));
            }
        }
    return uvArea > 0.0 && area > 0.0 ? std::sqrt(area / uvArea) : 100.0;
}

void detailNormalFinalize(const MapSettings& settings, const BakeInputs& inputs, TextureBuffer& tex, json& meta)
{
    LILA_ZONE();
    const json o = settings.options.is_object() ? settings.options : json::object();
    glm::dvec2 size{0.0};
    if (const auto it = o.find("uvSize"); it != o.end() && it->is_array() && it->size() == 2)
        size = {(*it)[0].get<double>(), (*it)[1].get<double>()};
    else if (it != o.end() && it->is_number())
        size = glm::dvec2(it->get<double>());
    const bool automatic = !(size.x > 0.0 && size.y > 0.0);
    if (automatic)
        size = glm::dvec2(measuredUvSize(inputs));
    const double fade = o.value("fade", 45.0);
    const bool greenUp = o.value("greenUp", false);

    const size_t n = tex.texelCount();
    image::Image normal{tex.width, tex.height, std::vector<float>(n * 4)}, height{tex.width, tex.height, std::vector<float>(n * 4)};
    for (size_t i = 0; i < n; ++i)
    {
        const glm::vec3 v = tex.covered[i] ? unit(glm::vec3(tex.value[i])) : glm::vec3(0.0f, 0.0f, 1.0f);
        normal.rgba[i * 4 + 0] = v.x * 0.5f + 0.5f;
        normal.rgba[i * 4 + 1] = v.y * 0.5f + 0.5f;
        normal.rgba[i * 4 + 2] = v.z * 0.5f + 0.5f;
        height.rgba[i * 4] = tex.covered[i] ? tex.value[i].w : 0.0f;
    }
    // Heights are already cm, so the slope per UV unit is one over the world size.
    const image::Image detail = detailNormals(normal, height, 1.0 / size, fade, true, &tex.covered);
    for (size_t i = 0; i < n; ++i)
    {
        const float* d = &detail.rgba[i * 4];
        tex.value[i] = tex.covered[i] ? glm::vec4(d[0], greenUp ? d[1] : 1.0f - d[1], d[2], 1.0f) : tex.def->background;
    }
    meta["uvSize"] = {size.x, size.y};
    meta["uvSizeAuto"] = automatic;
    meta["fade"] = fade;
    meta["decode"] = "normal = reoriented(heightNormal, detail): heightNormal from the height map (depth = 2 * its heightRange) over uvSize cm";
}
} // namespace

void registerDerivedMaps()
{
    registerMap({.id = "normalToHeight",
                 .label = "Normal to Height",
                 .suffix = "nheight",
                 .doc = "Height integrated from the baked tangent normals (a least-squares Poisson solve per UV island, no new "
                        "rays), 0.5 at each island's mean. options range (cm, 0 = auto), iterations (60 per level), highNormalMap.",
                 .background = {0.5f, 0.5f, 0.5f, 1.0f},
                 .preferredBits = 16,
                 .cpu = [](const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                           std::span<uint8_t> valid) { slopeKernel(ctx, ctx.prepared, samples, hits, out, valid); },
                 .finalize = normalToHeightFinalize,
                 .prepare = prepareHighNormalMaps});
    registerMap({.id = "detailNormal",
                 .label = "Detail Normal",
                 .suffix = "detailnormal",
                 .doc = "The tangent normal with the shape the height map already describes taken out, for materials that rebuild "
                        "normals from the height (Lila's Normals From Height, LilaLink's MF_LilaHeightNormal): walls a bake "
                        "looking straight down misses come back from the height. Bake height (16 bit) with it. options uvSize "
                        "(cm of world per UV unit, a number or [u, v]; 0 = measured from the lows), fade (degrees, 45: where "
                        "the height is steeper the detail fades out; 0 keeps it all), greenUp, highNormalMap.",
                 .background = {0.5f, 0.5f, 1.0f, 1.0f},
                 .cpu = detailNormalKernel,
                 .finalize = detailNormalFinalize,
                 .prepare = prepareHighNormalMaps});
}
} // namespace lila::bake
