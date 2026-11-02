#include "bake/MapMath.h"
#include "bake/MapsInternal.h"

#include "bake/Backend.h"
#include "bake/BakeInputs.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

/** Maps of the low's UV layout (wireframe, island borders), the bake's own diagnostics (error) and gradients. */
namespace lila::bake
{
using namespace mapmath;

namespace
{
/** Per group, per low, per triangle: bit k set when edge k (corner k to k + 1) is drawn. */
struct EdgeFlags
{
    std::vector<std::vector<std::vector<uint8_t>>> flags;
};

enum class EdgeKind
{
    Wire,
    Border,
};

/**
 * Wire: polygon edges, leaving out the diagonals a triangulated polygon adds. Border: edges with no neighbour in the
 * same UV island across them (seams and open edges).
 */
std::vector<uint8_t> edgeFlags(const LowInput& low, EdgeKind kind)
{
    const size_t tris = low.soup.triangleCount();
    size_t points = 0;
    const std::vector<uint32_t> weld = weldPositions(low.soup.positions, points);
    std::unordered_map<uint64_t, std::vector<uint32_t>> edges;
    edges.reserve(tris * 3);
    auto key = [](uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; };
    for (uint32_t t = 0; t < tris; ++t)
        for (uint32_t k = 0; k < 3; ++k)
            edges[key(weld[t * 3 + k], weld[t * 3 + (k + 1) % 3])].push_back(t * 3 + k);
    std::vector<uint8_t> out(tris, 0);
    auto sameUv = [&](size_t a, size_t b) {
        return low.soup.uvs.empty() || glm::all(glm::lessThan(glm::abs(low.soup.uvs[a] - low.soup.uvs[b]), glm::vec2(1e-6f)));
    };
    for (uint32_t t = 0; t < tris; ++t)
        for (uint32_t k = 0; k < 3; ++k)
        {
            const size_t c0 = t * 3 + k, c1 = t * 3 + (k + 1) % 3;
            bool draw = true;
            for (uint32_t other : edges[key(weld[c0], weld[c1])])
            {
                const uint32_t ot = other / 3;
                if (ot == t)
                    continue;
                if (kind == EdgeKind::Wire)
                {
                    if (!low.faces.empty() && low.faces[ot] == low.faces[t])
                        draw = false;
                    continue;
                }
                const size_t o0 = other, o1 = ot * 3 + (other % 3 + 1) % 3;
                const bool aligned = weld[o0] == weld[c0];
                const bool joined = aligned ? sameUv(o0, c0) && sameUv(o1, c1) : sameUv(o0, c1) && sameUv(o1, c0);
                const bool sameIsland = low.islands.empty() || low.islands[ot] == low.islands[t];
                if (joined && sameIsland)
                    draw = false;
            }
            if (draw)
                out[t] |= uint8_t(1u << k);
        }
    return out;
}

Prepare edgePrepare(EdgeKind kind)
{
    return [kind](const MapSettings&, const BakeInputs& inputs) -> std::shared_ptr<const void> {
        auto prep = std::make_shared<EdgeFlags>();
        prep->flags.resize(inputs.groups.size());
        for (size_t g = 0; g < inputs.groups.size(); ++g)
            for (const LowInput& low : inputs.groups[g].lows)
                prep->flags[g].push_back(edgeFlags(low, kind));
        return prep;
    };
}

CpuKernel edgeKernel(float defaultWidth)
{
    return [defaultWidth](const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord>, std::span<glm::vec4> out,
                          std::span<uint8_t> valid) {
        const auto* prep = static_cast<const EdgeFlags*>(ctx.prepared);
        const float half = 0.5f * std::max(0.0f, ctx.settings.options.value("width", defaultWidth));
        const glm::vec2 res(float(std::max(1, ctx.width)), float(std::max(1, ctx.height)));
        for (size_t i = 0; i < samples.size(); ++i)
        {
            const TexelSample& s = samples[i];
            valid[i] = 1;
            out[i] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            if (!prep || ctx.groupIndex >= prep->flags.size() || s.low >= prep->flags[ctx.groupIndex].size())
                continue;
            const uint8_t f = prep->flags[ctx.groupIndex][s.low][s.lowTriangle];
            const LowInput& low = ctx.group.lows[s.low];
            const glm::vec2 p = glm::vec2(s.uv[0], s.uv[1]) * res;
            float d = 1e30f;
            for (int k = 0; k < 3; ++k)
                if (f & (1u << k))
                {
                    const size_t c = size_t(s.lowTriangle) * 3;
                    d = std::min(d, segmentDistance(p, low.soup.uvs[c + size_t(k)] * res, low.soup.uvs[c + size_t((k + 1) % 3)] * res));
                }
            if (d <= half)
                out[i] = glm::vec4(1.0f);
        }
    };
}

const glm::vec4 kErrOk(0.0f, 0.0f, 0.0f, 1.0f), kErrMiss(1.0f, 0.0f, 0.0f, 1.0f), kErrBackface(1.0f, 0.0f, 1.0f, 1.0f),
    kErrBeyond(1.0f, 1.0f, 0.0f, 1.0f), kErrDegenerate(0.0f, 1.0f, 1.0f, 1.0f);

bool degenerateFrame(const TexelSample& s, const LowInput& low)
{
    const glm::vec3 n = v3(s.normal), t = v3(s.tangent);
    const float ln = glm::length(n), lt = glm::length(t);
    if (ln < 1e-6f || lt < 1e-6f || !std::isfinite(ln) || !std::isfinite(lt))
        return true;
    if (std::abs(glm::dot(n / ln, t / lt)) > 0.99f)
        return true;
    const float w = worldPerUv(low, s.lowTriangle);
    return !(w > 0.0f) || !std::isfinite(w);
}

void errorKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                 std::span<uint8_t> valid)
{
    const float diag = glm::length(ctx.group.highMax - ctx.group.highMin);
    const double beyondOpt = ctx.settings.options.value("beyond", 0.0);
    const float beyond = beyondOpt > 0.0 ? float(beyondOpt) : std::max(2.0f * diag, 1e-3f);
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const TexelSample& s = samples[i];
        const HitRecord& h = hits[i];
        valid[i] = 1;
        if (s.low < ctx.group.lows.size() && degenerateFrame(s, ctx.group.lows[s.low]))
            out[i] = kErrDegenerate;
        else if (h.flags & kHitBackface)
            out[i] = kErrBackface;
        else if (h.flags & kHitValid)
            out[i] = kErrOk;
        else
        {
            out[i] = kErrMiss;
            if (ctx.tracer)
            {
                const glm::vec3 o = v3(s.origin), d = v3(s.direction);
                TraceHit th;
                if (ctx.tracer->closestHit(ctx.groupIndex, OccluderScope::Group, kNoHit, s.low, RayKind::Projection, o, d, beyond, th) ||
                    ctx.tracer->closestHit(ctx.groupIndex, OccluderScope::Group, kNoHit, s.low, RayKind::Projection, o, -d, beyond, th))
                    out[i] = kErrBeyond;
            }
        }
    }
}

void errorFinalize(const MapSettings&, const BakeInputs&, TextureBuffer& tex, json& meta)
{
    uint64_t ok = 0, miss = 0, back = 0, beyond = 0, degenerate = 0;
    for (size_t i = 0; i < tex.value.size(); ++i)
    {
        if (!tex.covered[i])
            continue;
        const glm::vec4& v = tex.value[i];
        if (v == kErrMiss)
            ++miss;
        else if (v == kErrBackface)
            ++back;
        else if (v == kErrBeyond)
            ++beyond;
        else if (v == kErrDegenerate)
            ++degenerate;
        else
            ++ok;
    }
    meta["texels"] = {{"ok", ok}, {"miss", miss}, {"backface", back}, {"beyondDistance", beyond}, {"degenerateFrame", degenerate}};
    meta["legend"] = {{"ok", {0, 0, 0}}, {"miss", {255, 0, 0}}, {"backface", {255, 0, 255}}, {"beyondDistance", {255, 255, 0}},
                      {"degenerateFrame", {0, 255, 255}}};
}

glm::vec3 jsonVec3(const json& j, const glm::vec3& fallback)
{
    if (!j.is_array() || j.size() < 3)
        return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

void gradientKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                    std::span<uint8_t> valid)
{
    const glm::vec3 d = unit(jsonVec3(ctx.settings.options.value("direction", json()), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(0.0f, 1.0f, 0.0f));
    const bool all = ctx.settings.options.value("bounds", std::string("group")) == "all";
    const glm::vec3 lo = all ? ctx.inputs.highMin : ctx.group.highMin, hi = all ? ctx.inputs.highMax : ctx.group.highMax;
    float mn = 1e30f, mx = -1e30f;
    for (int c = 0; c < 8; ++c)
    {
        const glm::vec3 p((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
        mn = std::min(mn, glm::dot(p, d));
        mx = std::max(mx, glm::dot(p, d));
    }
    const float span = mx - mn;
    for (size_t i = 0; i < samples.size(); ++i)
    {
        valid[i] = (hits[i].flags & kHitValid) ? 1 : 0;
        if (!valid[i])
            continue;
        const float v = span > 0.0f ? std::clamp((glm::dot(v3(hits[i].position), d) - mn) / span, 0.0f, 1.0f) : 0.5f;
        out[i] = glm::vec4(v, v, v, 1.0f);
    }
}
} // namespace

std::vector<uint8_t> lowEdgeFlags(const LowInput& low, bool borders) { return edgeFlags(low, borders ? EdgeKind::Border : EdgeKind::Wire); }

void registerUvMaps()
{
    registerMap({.id = "wireframe",
                 .label = "UV Wireframe",
                 .suffix = "wireframe",
                 .doc = "The low's polygon edges in UV space, white on black, triangulation diagonals left out. option width "
                        "(texels, 1).",
                 .background = {0.0f, 0.0f, 0.0f, 1.0f},
                 .dilate = false,
                 .cpu = edgeKernel(1.0f),
                 .prepare = edgePrepare(EdgeKind::Wire)});
    registerMap({.id = "islandBorder",
                 .label = "Island Border Mask",
                 .suffix = "islandborder",
                 .doc = "White along each UV island's border (seams and open edges) and into the padding, black inside: a mask "
                        "for seam fixes. option width (texels, 2).",
                 .background = {0.0f, 0.0f, 0.0f, 1.0f},
                 .cpu = edgeKernel(2.0f),
                 .prepare = edgePrepare(EdgeKind::Border)});
    registerMap({.id = "error",
                 .label = "Error Map",
                 .suffix = "error",
                 .doc = "Where the projection failed, a flat colour per texel (majority of its samples): black ok, red missed, "
                        "magenta hit a back face, yellow a surface lies beyond the cage or rear distance (raise frontal/rear), cyan "
                        "the low's tangent frame is degenerate. meta texels counts each. option beyond (cm, how far to look; 0 = "
                        "twice the group's high bounds).",
                 .resolve = Resolve::Exact,
                 .background = {0.0f, 0.0f, 0.0f, 1.0f},
                 .linear = false,
                 .cpu = errorKernel,
                 .finalize = errorFinalize});
    registerMap({.id = "gradient",
                 .label = "Directional Gradient",
                 .suffix = "gradient",
                 .doc = "0 to 1 along a direction across the high's bounds, from the hit positions (no new rays). options "
                        "direction ([0, 1, 0]: bottom to top), bounds (group or all).",
                 .background = {0.0f, 0.0f, 0.0f, 1.0f},
                 .cpu = gradientKernel});
}
} // namespace lila::bake
