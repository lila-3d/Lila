#pragma once

namespace lila::tex
{
/** Declarations every generated pass starts with: the bindings (see Plan.h), port interfaces and conversions. */
inline constexpr const char* kPrelude = R"slang(
struct TexPassHeader
{
    float4 res;
    float4 uv;
};
ConstantBuffer<TexPassHeader> gPass;
StructuredBuffer<float4> gU;
StructuredBuffer<float4> gArena;
RWStructuredBuffer<float4> gOut;

struct TexMaterial
{
    float4 baseColor;
    float roughness;
    float metallic;
    float4 normal;
    float height;
    float ao;
    float4 emissive;
    float opacity;
};

TexMaterial texDefaultMaterial()
{
    TexMaterial m;
    m.baseColor = float4(0.5, 0.5, 0.5, 1.0);
    m.roughness = 0.5;
    m.metallic = 0.0;
    m.normal = float4(0.5, 0.5, 1.0, 1.0);
    m.height = 0.5;
    m.ao = 1.0;
    m.emissive = float4(0.0, 0.0, 0.0, 1.0);
    m.opacity = 1.0;
    return m;
}

interface ITexFloat { float sample(float2 uv); }
interface ITexColor { float4 sample(float2 uv); }
interface ITexVector { float3 sample(float2 uv); }
interface ITexMaterial { TexMaterial sample(float2 uv); }

float2 texel() { return gPass.res.zw; }
float2 texResolution() { return gPass.res.xy; }

float texToFloat(float v) { return v; }
float texToFloat(float3 v) { return v.x; }
float texToFloat(float4 c) { return dot(c.rgb, float3(0.2126, 0.7152, 0.0722)); }
float4 texToColor(float v) { return float4(v, v, v, 1.0); }
float4 texToColor(float3 v) { return float4(v, 1.0); }
float4 texToColor(float4 c) { return c; }
float3 texToVector(float v) { return float3(v, v, v); }
float3 texToVector(float3 v) { return v; }
float3 texToVector(float4 c) { return c.xyz; }
TexMaterial texToMaterial(TexMaterial m) { return m; }
float4 texStore(float v) { return float4(v, v, v, 1.0); }
float4 texStore(float3 v) { return float4(v, 1.0); }
float4 texStore(float4 c) { return c; }

float4 texFetchBuf(uint slot, int2 p)
{
    float4 d = gU[slot];
    int w = int(asuint(d.y));
    int h = int(asuint(d.z));
    int x = ((p.x % w) + w) % w;
    int y = ((p.y % h) + h) % h;
    return gArena[asuint(d.x) + uint(y * w + x)];
}

float4 texReadBuf(uint slot, float2 uv)
{
    float4 d = gU[slot];
    float2 size = float2(float(asuint(d.y)), float(asuint(d.z)));
    float2 p = float2(uv.x, 1.0 - uv.y) * size - 0.5;
    float2 f = floor(p);
    float2 t = p - f;
    int2 i = int2(f);
    float4 a = texFetchBuf(slot, i);
    float4 b = texFetchBuf(slot, i + int2(1, 0));
    float4 c = texFetchBuf(slot, i + int2(0, 1));
    float4 e = texFetchBuf(slot, i + int2(1, 1));
    return lerp(lerp(a, b, t.x), lerp(c, e, t.x), t.y);
}

float4 texGradientEval(uint base, float t)
{
    float4 h = gU[base];
    int n = int(h.x);
    int mode = int(h.y);
    if (n <= 0)
        return float4(t, t, t, 1.0);
    float p0 = gU[base + 1].x;
    if (t <= p0)
        return gU[base + 2];
    for (int k = 1; k < n; ++k)
    {
        float p1 = gU[base + 1 + uint(2 * k)].x;
        if (t <= p1)
        {
            float4 c0 = gU[base + uint(2 * k)];
            float4 c1 = gU[base + 2 + uint(2 * k)];
            if (mode == 1)
                return c0;
            float f = saturate((t - p0) / max(p1 - p0, 1e-6));
            if (mode == 2)
                f = f * f * (3.0 - 2.0 * f);
            return lerp(c0, c1, f);
        }
        p0 = p1;
    }
    return gU[base + uint(2 * n)];
}

float2 texCurvePoint(uint base, int k)
{
    float4 s = gU[base + 1 + uint(k / 2)];
    return (k & 1) != 0 ? s.zw : s.xy;
}

float texCurveEval(uint base, float x)
{
    int n = int(gU[base].x);
    bool smooth = gU[base].y != 0.0;
    if (n <= 0)
        return x;
    float2 p0 = texCurvePoint(base, 0);
    if (x <= p0.x)
        return p0.y;
    for (int k = 1; k < n; ++k)
    {
        float2 p1 = texCurvePoint(base, k);
        if (x <= p1.x)
        {
            float span = max(p1.x - p0.x, 1e-6);
            float f = (x - p0.x) / span;
            if (!smooth)
                return lerp(p0.y, p1.y, f);
            float2 pm = k >= 2 ? texCurvePoint(base, k - 2) : p0;
            float2 pn = k + 1 < n ? texCurvePoint(base, k + 1) : p1;
            float m0 = (p1.y - pm.y) / max(p1.x - pm.x, 1e-6) * span;
            float m1 = (pn.y - p0.y) / max(pn.x - p0.x, 1e-6) * span;
            float f2 = f * f;
            float f3 = f2 * f;
            return (2.0 * f3 - 3.0 * f2 + 1.0) * p0.y + (f3 - 2.0 * f2 + f) * m0 + (3.0 * f2 - 2.0 * f3) * p1.y + (f3 - f2) * m1;
        }
        p0 = p1;
    }
    return p0.y;
}

struct TexGradient
{
    uint base;
    float4 eval(float t) { return texGradientEval(base, t); }
};

struct TexCurve
{
    uint base;
    float eval(float x) { return texCurveEval(base, x); }
};

uint texHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint texHash(uint2 v, uint seed) { return texHash(v.x ^ texHash(v.y ^ texHash(seed + 0x9e3779b9u))); }
float texRand(uint2 v, uint seed) { return float(texHash(v, seed) >> 8) * (1.0 / 16777216.0); }
float2 texRand2(uint2 v, uint seed) { return float2(texRand(v, seed), texRand(v, seed ^ 0x68bc21ebu)); }
)slang";
} // namespace lila::tex
