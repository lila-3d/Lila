// The UV raster of a band on the device, matching TexelRaster (src/bake/TexelRaster.cpp) sample for sample: a dense slot
// per (texel, sub-sample) of the band, claimed by the lowest-numbered triangle covering it, then filled in as a
// TexelSample (lowTriangle kNoHit where nothing claimed it). Coverage tests run in double precision, as on the CPU, so
// both claim the same samples; bake_raster32 falls back to float on devices without shaderFloat64. Included by
// bake_raster64.slang and bake_raster32.slang, which set RASTER_F64.

#define BAKE_OWN_PUSH 1
#include "bake_common.slang"

#if RASTER_F64
typedef double real;
typedef double2 real2;
#else
typedef float real;
typedef float2 real2;
#endif

struct RasterPush
{
    LowInfo* low;
    uint* items; // per item: triangle, then block x and y (8 x 8 texels) in the low's texture
    uint* claim;
    TexelSample* out;
    uint width;
    uint height;
    uint grid;
    uint y0;
    uint rows;
    uint tileU;
    uint tileV;
    uint udim;
    uint first;
    uint count;
    uint lowIndex;
    uint explicitCage;
    float frontal;
    float rear;
    float offset;
    float skew;
};

[[vk::push_constant]] RasterPush rp;

float hash01(uint a, uint b, uint c, uint d)
{
    uint h = hashBits(a ^ hashBits(b ^ hashBits(c ^ hashBits(d))));
    return float(h >> 8) * (1.0 / 16777216.0);
}

real edgeFn(real2 a, real2 b, real2 p)
{
    precise real e = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    return e;
}

// The triangle's corners in texel space of the tile (row 0 the top).
void cornersOf(uint t, out real2 p[3])
{
    float2* uv = rp.low.uvs + t * 3;
    for (int k = 0; k < 3; ++k)
        p[k] = real2((real(uv[k].x) - real(rp.tileU)) * real(rp.width), (real(1.0) - (real(uv[k].y) - real(rp.tileV))) * real(rp.height));
}

real2 samplePoint(uint x, uint y, uint sub)
{
    uint n = rp.grid;
    uint i = sub % n, j = sub / n;
    real jx = n == 1 ? real(0.5) : real(hash01(x, y, sub * 2, rp.udim));
    real jy = n == 1 ? real(0.5) : real(hash01(x, y, sub * 2 + 1, rp.udim));
    return real2(real(x) + (real(i) + jx) / real(n), real(y) + (real(j) + jy) / real(n));
}

// One workgroup per item, a thread per texel of its 8 x 8 block: every sample inside the triangle takes the lowest
// triangle index.
[shader("compute")]
[numthreads(8, 8, 1)]
void claimCS(uint3 gid: SV_GroupID, uint3 lid: SV_GroupThreadID)
{
    uint item = rp.first + gid.x;
    if (item >= rp.count)
        return;
    uint t = rp.items[item * 3];
    uint x = rp.items[item * 3 + 1] * 8 + lid.x;
    uint y = rp.items[item * 3 + 2] * 8 + lid.y;
    real2 p[3];
    cornersOf(t, p);
    real area = edgeFn(p[0], p[1], p[2]);
    if (abs(area) < real(1e-12))
        return;
    real minX = min(p[0].x, min(p[1].x, p[2].x)), maxX = max(p[0].x, max(p[1].x, p[2].x));
    real minY = min(p[0].y, min(p[1].y, p[2].y)), maxY = max(p[0].y, max(p[1].y, p[2].y));
    int xa = clamp(int(floor(minX)), 0, int(rp.width)), xb = clamp(int(ceil(maxX)), 0, int(rp.width));
    int ya = clamp(int(floor(minY)), int(rp.y0), int(rp.y0 + rp.rows)), yb = clamp(int(ceil(maxY)), int(rp.y0), int(rp.y0 + rp.rows));
    if (int(x) < xa || int(x) >= xb || int(y) < ya || int(y) >= yb)
        return;
    uint S = rp.grid * rp.grid;
    for (uint sub = 0; sub < S; ++sub)
    {
        real2 sp = samplePoint(x, y, sub);
        real w0 = edgeFn(p[1], p[2], sp) / area, w1 = edgeFn(p[2], p[0], sp) / area, w2 = edgeFn(p[0], p[1], sp) / area;
        if (w0 < real(0) || w1 < real(0) || w2 < real(0))
            continue;
        uint slot = ((y - rp.y0) * rp.width + x) * S + sub;
        InterlockedMin(rp.claim[slot], t);
    }
}

float3 mix3(float* v, uint c0, float3 w)
{
    return load3(v, c0) * w.x + load3(v, c0 + 1) * w.y + load3(v, c0 + 2) * w.z;
}

// A slot's TexelSample from the triangle that claimed it, as TexelRaster::band builds it.
[shader("compute")]
[numthreads(64, 1, 1)]
void samplesCS(uint3 id: SV_DispatchThreadID)
{
    uint slot = rp.first + id.x;
    if (slot >= rp.count)
        return;
    uint S = rp.grid * rp.grid;
    uint sub = slot % S, texel = slot / S;
    uint x = texel % rp.width, y = rp.y0 + texel / rp.width;
    TexelSample smp;
    smp.position = float3(0.0);
    smp.normal = float3(0.0);
    smp.tangent = float4(0.0);
    smp.origin = float3(0.0);
    smp.direction = float3(0.0);
    smp.rayLength = 0.0;
    smp.uv = float2(0.0);
    smp.texel = y * rp.width + x;
    smp.sub = sub;
    smp.tile = rp.udim;
    smp.low = rp.lowIndex;
    uint t = rp.claim[slot];
    smp.lowTriangle = t;
    if (t == kNoHit)
    {
        rp.out[slot] = smp;
        return;
    }
    real2 p[3];
    cornersOf(t, p);
    real area = edgeFn(p[0], p[1], p[2]);
    real2 sp = samplePoint(x, y, sub);
    float3 w = float3(float(edgeFn(p[1], p[2], sp) / area), float(edgeFn(p[2], p[0], sp) / area), float(edgeFn(p[0], p[1], sp) / area));
    LowInfo low = rp.low[0];
    uint c0 = t * 3;
    float3 P = mix3(low.positions, c0, w);
    float3 N = mix3(low.normals, c0, w);
    float4 T = low.tangents[c0] * w.x + low.tangents[c0 + 1] * w.y + low.tangents[c0 + 2] * w.z;
    float2 U = low.uvs[c0] * w.x + low.uvs[c0 + 1] * w.y + low.uvs[c0 + 2] * w.z;
    float3 origin, dir;
    float len;
    bool explicitCage = rp.explicitCage != 0;
    float3 C = explicitCage ? mix3(low.cagePositions, c0, w) : float3(0.0);
    float toCage = explicitCage ? length(P - C) : 0.0;
    if (explicitCage && toCage > 1e-6)
    {
        origin = C;
        dir = (P - C) / toCage;
        len = toCage + rp.rear;
    }
    else
    {
        float3 cn = mix3(low.cageNormals, c0, w);
        float l = length(cn);
        cn = l > 0.0 ? cn / l : float3(0.0, 0.0, 1.0);
        float skewExtra = 0.0;
        if (low.hasSkew != 0)
            skewExtra = low.cageSkew[c0] * w.x + low.cageSkew[c0 + 1] * w.y + low.cageSkew[c0 + 2] * w.z;
        float skew = clamp(rp.skew + skewExtra, 0.0, 1.0);
        if (skew > 0.0)
        {
            float3 a = load3(low.positions, c0), b = load3(low.positions, c0 + 1), c = load3(low.positions, c0 + 2);
            float3 g = cross(b - a, c - a);
            float gl = length(g);
            if (gl > 0.0)
            {
                float3 d = lerp(cn, g / gl, skew);
                float dl = length(d);
                cn = dl > 1e-6 ? d / dl : cn;
            }
        }
        float push = rp.frontal * rp.offset * (low.cageScale[c0] * w.x + low.cageScale[c0 + 1] * w.y + low.cageScale[c0 + 2] * w.z);
        origin = P + cn * push;
        dir = -cn;
        len = push + rp.rear;
    }
    smp.position = P;
    smp.normal = N;
    smp.tangent = float4(T.xyz, low.tangents[c0].w < 0.0 ? -1.0 : 1.0);
    smp.origin = origin;
    smp.direction = dir;
    smp.rayLength = len;
    smp.uv = U;
    rp.out[slot] = smp;
}
