// Texture passes of resident bakes: per-texel accumulation of a band's map output, resolve, the device versions of the
// maps' finalize steps, range reductions and jump-flood dilation. Each mirrors src/bake (Backend.cpp resolve and
// dilateCpu, Bake.cpp voteExact, Maps*.cpp finalize). One thread per texel, dispatched over [first, total).

#define BAKE_OWN_PUSH 1
#include "bake_common.slang"

static const uint kResolveAverage = 0;
static const uint kResolveExact = 1;
static const uint kResolveCoverage = 2;

// Where a band's samples on one low material slot accumulate: a texture's device accumulators (id 0: nowhere).
struct AccTarget
{
    float4* value;
    float* weight;
    uint* count;
    uint* exact;
    uint* votes;
    uint mode;
    uint id;
};

struct ImagePush
{
    TexelSample* samples;
    float4* out;
    uint* valid;
    AccTarget* targets;
    LowInfo* low;
    float4* value;
    float* weight;
    uint* count; // samples per texel; covered (0 or 1) once resolved
    uint* exact;
    uint* votes;
    uint* seedsIn;
    uint* seedsOut;
    float* scratch;
    uint* reduce;
    float4 background;
    float4 params;
    float4 params2;
    uint width;
    uint height;
    uint grid;
    uint y0;
    uint rows;
    uint mode;
    uint first;
    uint total;
    int step;
    int padding;
    uint prefix;
    uint shift;
};

[[vk::push_constant]] ImagePush ip;

static const uint kNone = 0xffffffffu;

// A band's samples into their textures, texel by texel in sub-sample order, as runBake's band loop accumulates them.
[shader("compute")]
[numthreads(64, 1, 1)]
void accumulateCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total)
        return;
    uint x = i % ip.width;
    uint texel = (ip.y0 + i / ip.width) * ip.width + x;
    uint S = ip.grid * ip.grid;
    uint base = i * S;
    // Exact votes: texture, colour and count per distinct pair, in order of first appearance; voteTarget is a material
    // slot that writes the texture.
    uint voteTarget[64];
    uint voteId[64];
    uint voteColour[64];
    uint voteCount[64];
    uint votes = 0;
    for (uint sub = 0; sub < S; ++sub)
    {
        uint tri = ip.samples[base + sub].lowTriangle;
        if (tri == kNoHit)
            continue;
        uint mat = ip.low.materials[tri];
        AccTarget t = ip.targets[mat];
        if (t.id == 0)
            continue;
        t.count[texel] += 1;
        if (ip.valid[base + sub] == 0)
            continue;
        float4 v = ip.out[base + sub];
        if (t.mode != kResolveExact)
        {
            t.value[texel] += v;
            t.weight[texel] += 1.0;
            continue;
        }
        uint c = packRgba8(v);
        uint k = 0;
        while (k < votes && !(voteId[k] == t.id && voteColour[k] == c))
            ++k;
        if (k == votes)
        {
            voteTarget[k] = mat;
            voteId[k] = t.id;
            voteColour[k] = c;
            voteCount[k] = 0;
            ++votes;
        }
        ++voteCount[k];
    }
    // Votes are in sub-sample order, so a strictly larger count is needed to beat an earlier colour.
    for (uint k = 0; k < votes; ++k)
    {
        AccTarget t = ip.targets[voteTarget[k]];
        bool best = true;
        for (uint m = 0; m < votes && best; ++m)
            if (m != k && voteId[m] == voteId[k] && (voteCount[m] > voteCount[k] || (voteCount[m] == voteCount[k] && m < k)))
                best = false;
        if (best && voteCount[k] > t.votes[texel])
        {
            t.votes[texel] = voteCount[k];
            t.exact[texel] = voteColour[k];
        }
    }
}

// TextureBuffer::resolve: value from the accumulators (background where nothing counted), count becomes covered.
[shader("compute")]
[numthreads(64, 1, 1)]
void resolveCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total)
        return;
    float4 v = ip.background;
    uint covered = 0;
    if (ip.mode == kResolveExact)
    {
        if (ip.votes[i] != 0)
        {
            v = unpackRgba8(ip.exact[i]);
            covered = 1;
        }
    }
    else if (ip.mode == kResolveCoverage)
    {
        uint n = ip.count[i];
        if (n != 0)
        {
            float f = min(1.0, float(n) / float(max(1u, ip.grid * ip.grid)));
            v = float4(f, f, f, 1.0);
            covered = 1;
        }
    }
    else
    {
        float w = ip.weight[i];
        if (w > 0.0)
        {
            v = ip.value[i] / w;
            covered = 1;
        }
    }
    ip.value[i] = v;
    ip.count[i] = covered;
}

// renormalize / normalFinalize: averaged normals back on the unit sphere.
[shader("compute")]
[numthreads(64, 1, 1)]
void renormalizeCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total || ip.count[i] == 0)
        return;
    ip.value[i] = float4(unitOr(ip.value[i].xyz * 2.0 - 1.0, float3(0, 0, 1)) * 0.5 + 0.5, 1.0);
}

// The largest red over covered texels into reduce[0] (as float bits): mode 0 |r|, mode 1 max(r, 0).
[shader("compute")]
[numthreads(64, 1, 1)]
void maxCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total || ip.count[i] == 0)
        return;
    float r = ip.value[i].x;
    float v = ip.mode == 0 ? abs(r) : max(r, 0.0);
    if (v > 0.0)
        InterlockedMax(ip.reduce[0], asuint(v));
}

// heightFinalize: raw heights to 0-1 about 0.5 over params.x.
[shader("compute")]
[numthreads(64, 1, 1)]
void heightCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total)
        return;
    float v = ip.count[i] != 0 ? clamp(0.5 + ip.value[i].x / (2.0 * ip.params.x), 0.0, 1.0) : 0.5;
    ip.value[i] = float4(v, v, v, 1.0);
}

// thicknessFinalize: raw distances to 0-1 over params.x, mode 1 inverts.
[shader("compute")]
[numthreads(64, 1, 1)]
void thicknessApplyCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total || ip.count[i] == 0)
        return;
    float v = clamp(ip.value[i].x / ip.params.x, 0.0, 1.0);
    if (ip.mode == 1)
        v = 1.0 - v;
    ip.value[i] = float4(v, v, v, 1.0);
}

// The mesh curvature source: the resolved red as the raw curvature.
[shader("compute")]
[numthreads(64, 1, 1)]
void copyRedCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total)
        return;
    ip.scratch[i] = ip.value[i].x;
}

bool linked(int px, int py, uint comp, float c, int r)
{
    if (px < 0 || px >= int(ip.width) || py < 0 || py >= int(ip.height))
        return false;
    uint j = uint(py) * ip.width + uint(px);
    // A slope jump this large between neighbours is a UV seam (the tangent frame changes), not geometry.
    return ip.count[j] != 0 && abs(ip.value[j][comp] - c) < 1.0 * float(r);
}

float slopeDerivative(int x, int y, int dx, int dy, uint comp, float texelWorld, float4 c, int r)
{
    int px = x + dx * r, py = y + dy * r, nx = x - dx * r, ny = y - dy * r;
    bool hasP = linked(px, py, comp, c[comp], r), hasN = linked(nx, ny, comp, c[comp], r);
    float sum = 0.0, span = 0.0;
    if (hasP && hasN)
    {
        sum = ip.value[uint(py) * ip.width + uint(px)][comp] - ip.value[uint(ny) * ip.width + uint(nx)][comp];
        span = 2.0 * float(r);
    }
    else if (hasP)
    {
        sum = ip.value[uint(py) * ip.width + uint(px)][comp] - c[comp];
        span = float(r);
    }
    else if (hasN)
    {
        sum = c[comp] - ip.value[uint(ny) * ip.width + uint(nx)][comp];
        span = float(r);
    }
    return span > 0.0 ? sum / (span * texelWorld) : 0.0;
}

// curvatureFromSlopes: mean curvature from the resolved slope texture (sx, sy, 0, worldPerUv); mode is the radius.
[shader("compute")]
[numthreads(64, 1, 1)]
void slopeCurvatureCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total)
        return;
    float k = 0.0;
    float4 c = ip.value[i];
    if (ip.count[i] != 0 && c.w > 0.0)
    {
        int x = int(i % ip.width), y = int(i / ip.width), r = int(max(1u, ip.mode));
        // d/du along +x, d/dv along -y (row 0 is the top of the texture, V = 1).
        float du = slopeDerivative(x, y, 1, 0, 0, c.w / float(ip.width), c, r);
        float dv = slopeDerivative(x, y, 0, -1, 1, c.w / float(ip.height), c, r);
        k = 0.5 * (du + dv);
    }
    ip.scratch[i] = k;
}

groupshared uint gHistogram[256];

// One radix-select pass over |scratch| of covered texels: the histogram of the byte at shift among keys whose higher
// bytes equal prefix. Four passes find percentileAbs's exact element.
[shader("compute")]
[numthreads(256, 1, 1)]
void histogramCS(uint3 gid: SV_GroupID, uint3 lid: SV_GroupThreadID)
{
    gHistogram[lid.x] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint start = ip.first + gid.x * 256 * 16;
    for (uint k = 0; k < 16; ++k)
    {
        uint i = start + k * 256 + lid.x;
        if (i >= ip.total || ip.count[i] == 0)
            continue;
        uint key = asuint(abs(ip.scratch[i]));
        if (ip.shift < 24 && (key >> (ip.shift + 8)) != ip.prefix)
            continue;
        InterlockedAdd(gHistogram[(key >> ip.shift) & 255u], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gHistogram[lid.x] != 0)
        InterlockedAdd(ip.reduce[lid.x], gHistogram[lid.x]);
}

// curvatureFinalize: params x range, y threshold, z softness, w noise; params2.x noise scale. mode 0 curvature,
// 1 convexity, 2 cavity, 3 edge wear.
[shader("compute")]
[numthreads(64, 1, 1)]
void curvatureCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total)
        return;
    float k = ip.count[i] != 0 ? clamp(ip.scratch[i] / ip.params.x, -1.0, 1.0) : 0.0;
    float v = 0.0;
    if (ip.mode == 0)
        v = 0.5 + 0.5 * k;
    else if (ip.mode == 1)
        v = max(k, 0.0);
    else if (ip.mode == 2)
        v = 1.0 - max(-k, 0.0);
    else
    {
        v = smoothstepCpu(ip.params.y, ip.params.y + ip.params.z, k);
        float noise = ip.params.w;
        if (noise > 0.0 && v > 0.0)
        {
            // Bilinear value noise on the texel grid, deterministic.
            int x = int(i % ip.width), y = int(i / ip.width);
            float fx = float(x) / ip.params2.x, fy = float(y) / ip.params2.x;
            int x0 = int(floor(fx)), y0 = int(floor(fy));
            float ax = smoothstepCpu(0.0, 1.0, fx - float(x0)), ay = smoothstepCpu(0.0, 1.0, fy - float(y0));
            float n00 = float(hashBits(uint(x0) * 73856093u ^ uint(y0) * 19349663u) & 0xffff) / 65535.0;
            float n10 = float(hashBits(uint(x0 + 1) * 73856093u ^ uint(y0) * 19349663u) & 0xffff) / 65535.0;
            float n01 = float(hashBits(uint(x0) * 73856093u ^ uint(y0 + 1) * 19349663u) & 0xffff) / 65535.0;
            float n11 = float(hashBits(uint(x0 + 1) * 73856093u ^ uint(y0 + 1) * 19349663u) & 0xffff) / 65535.0;
            float nv = lerp(lerp(n00, n10, ax), lerp(n01, n11, ax), ay);
            v *= 1.0 - noise * nv;
        }
    }
    ip.value[i] = float4(v, v, v, 1.0);
}

// Jump flood dilation, as bake_dilate.slang but reading covered from count and copying values on the device.
[shader("compute")]
[numthreads(64, 1, 1)]
void jfaInitCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total)
        return;
    ip.seedsOut[i] = ip.count[i] != 0 ? i : kNone;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void jfaStepCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total)
        return;
    int2 p = int2(int(i % ip.width), int(i / ip.width));
    uint best = kNone;
    int bestCheb = 0x7fffffff;
    int bestEuclid = 0x7fffffff;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 q = p + int2(dx, dy) * ip.step;
            if (any(q < 0) || q.x >= int(ip.width) || q.y >= int(ip.height))
                continue;
            uint s = ip.seedsIn[uint(q.y) * ip.width + uint(q.x)];
            if (s == kNone)
                continue;
            int2 sp = int2(int(s % ip.width), int(s / ip.width));
            int2 d = abs(sp - p);
            int cheb = max(d.x, d.y);
            int euclid = d.x * d.x + d.y * d.y;
            if (cheb < bestCheb || (cheb == bestCheb && (euclid < bestEuclid || (euclid == bestEuclid && s < best))))
            {
                best = s;
                bestCheb = cheb;
                bestEuclid = euclid;
            }
        }
    ip.seedsOut[i] = best;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void dilateApplyCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total || ip.count[i] != 0)
        return;
    uint s = ip.seedsIn[i];
    if (s == kNone)
        return;
    int dx = abs(int(s % ip.width) - int(i % ip.width)), dy = abs(int(s / ip.width) - int(i / ip.width));
    if (ip.padding < 0 || max(dx, dy) <= ip.padding)
        ip.value[i] = ip.value[s];
}

// Incremental rebakes: zeroes the accumulators of the texel tiles listed in seedsIn (index ty * columns + tx, columns in
// shift), step x step texels each. Thread i is texel i % (step * step) of listed tile i / (step * step).
[shader("compute")]
[numthreads(64, 1, 1)]
void clearTilesCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total)
        return;
    uint s = uint(ip.step);
    uint tile = ip.seedsIn[i / (s * s)];
    uint x = (tile % ip.shift) * s + i % s;
    uint y = (tile / ip.shift) * s + (i / s) % s;
    if (x >= ip.width || y >= ip.height)
        return;
    uint t = y * ip.width + x;
    ip.value[t] = float4(0, 0, 0, 0);
    ip.weight[t] = 0.0;
    ip.count[t] = 0;
    if (ip.mode == kResolveExact)
    {
        ip.exact[t] = 0;
        ip.votes[t] = 0;
    }
}

// Finished values into an image's texel layout in seedsOut, for a buffer to image copy: mode 0 RGBA16F, 1 R16F (thread i
// packs texels 2i and 2i + 1), 2 RGBA8 unorm.
[shader("compute")]
[numthreads(64, 1, 1)]
void packTexelsCS(uint3 id: SV_DispatchThreadID)
{
    uint i = ip.first + id.x;
    if (i >= ip.total)
        return;
    if (ip.mode == 0)
    {
        float4 v = ip.value[i];
        ip.seedsOut[i * 2] = f32tof16(v.x) | (f32tof16(v.y) << 16);
        ip.seedsOut[i * 2 + 1] = f32tof16(v.z) | (f32tof16(v.w) << 16);
    }
    else if (ip.mode == 1)
    {
        uint n = ip.width * ip.height;
        uint a = f32tof16(ip.value[i * 2].x);
        uint b = i * 2 + 1 < n ? f32tof16(ip.value[i * 2 + 1].x) : 0u;
        ip.seedsOut[i] = a | (b << 16);
    }
    else
        ip.seedsOut[i] = packRgba8(ip.value[i]);
}
