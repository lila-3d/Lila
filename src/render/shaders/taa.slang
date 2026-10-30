// Temporal antialiasing on the scene target: blends the jittered scene into its history, clipped to the neighbourhood
// while things move, as plain accumulation while the view holds still. compose.slang then tonemaps the result.

#include "scene_color.slang"

struct TaaPush
{
    float4x4 invViewProj;  // this frame's, unjittered: the history follows the camera, not the jitter
    float4x4 prevViewProj; // the previous frame's, unjittered: where the history holds each point
    float4 size;           // extent, then its inverse
    float4 params;         // x: weight of this frame, y: 1 to clip the history to the neighbourhood, z: 1 to ignore the history,
                           // w: 1 to blend in place, without reprojection
};

[[vk::push_constant]] TaaPush tp;

[[vk::binding(0, 0)]] Sampler2D sceneTex;   // resolved, compressed
[[vk::binding(1, 0)]] Sampler2D depthTex;   // resolved, reversed-Z
[[vk::binding(2, 0)]] Sampler2D historyTex; // the previous result, filtered
[[vk::binding(3, 0)]] RWTexture2D<float4> taaOut;

float3 toYCoCg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 fromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

/** The history at uv through a Catmull-Rom filter from 9 bilinear taps, which keeps it from going soft frame over frame. */
float3 sampleHistory(float2 uv)
{
    float2 pos = uv * tp.size.xy;
    float2 center = floor(pos - 0.5) + 0.5;
    float2 f = pos - center;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (center - 1.0) * tp.size.zw;
    float2 t3 = (center + 2.0) * tp.size.zw;
    float2 t12 = (center + w2 / w12) * tp.size.zw;
    float3 c = 0.0;
    c += historyTex.SampleLevel(float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y);
    c += historyTex.SampleLevel(float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y);
    c += historyTex.SampleLevel(float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    c += historyTex.SampleLevel(float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y);
    c += historyTex.SampleLevel(float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(c / wsum, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void taaCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    int2 size = int2(tp.size.xy);
    if (any(px >= size))
        return;
    float4 currentA = sceneTex.Load(int3(px, 0));
    float3 current = currentA.rgb;
    if (tp.params.z > 0.5)
    {
        taaOut[px] = currentA;
        return;
    }
    if (tp.params.w > 0.5)
    {
        taaOut[px] = lerp(historyTex.Load(int3(px, 0)), currentA, tp.params.x);
        return;
    }

    // Neighbourhood moments for clipping, and the nearest depth so edges reproject with their foreground.
    float3 m1 = 0.0, m2 = 0.0;
    float nearest = 0.0;
    int2 nearestPx = px;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            int2 q = clamp(px + int2(x, y), int2(0), size - 1);
            float3 c = toYCoCg(sceneTex.Load(int3(q, 0)).rgb);
            m1 += c;
            m2 += c * c;
            float d = depthTex.Load(int3(q, 0)).r;
            if (d > nearest)
            {
                nearest = d;
                nearestPx = q;
            }
        }

    float2 uv = (float2(px) + 0.5) * tp.size.zw;
    float2 ndc = (float2(nearestPx) + 0.5) * tp.size.zw * 2.0 - 1.0;
    // Homogeneous all the way: reversed-Z with an infinite far plane puts the background at w = 0.
    float4 world = mul(tp.invViewProj, float4(ndc, nearest, 1.0));
    float4 prev = mul(tp.prevViewProj, world);
    float2 prevUv = uv;
    if (abs(prev.w) > 1e-12)
        prevUv = uv + (prev.xy / prev.w * 0.5 + 0.5) - (float2(nearestPx) + 0.5) * tp.size.zw;
    if (any(prevUv < 0.0) || any(prevUv > 1.0))
    {
        taaOut[px] = currentA;
        return;
    }

    float3 history = sampleHistory(prevUv);
    float historyAlpha = historyTex.SampleLevel(prevUv, 0).a;
    if (tp.params.y > 0.5)
    {
        // Variance clipping (Salvi): pull the history toward the neighbourhood's mean until it is inside its box.
        float3 mean = m1 / 9.0;
        float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
        float3 lo = mean - 1.25 * sigma, hi = mean + 1.25 * sigma;
        float3 h = toYCoCg(history);
        float3 center = 0.5 * (hi + lo), extent = 0.5 * (hi - lo) + 1e-5;
        float3 offset = h - center;
        float3 units = abs(offset / extent);
        float far = max(units.x, max(units.y, units.z));
        if (far > 1.0)
            h = center + offset / far;
        history = fromYCoCg(h);
    }
    taaOut[px] = float4(lerp(history, current, tp.params.x), lerp(historyAlpha, currentA.a, tp.params.x));
}

