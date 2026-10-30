// Energy-conserving bloom (Jimenez, Call of Duty: Advanced Warfare): the scene filtered down a mip chain with a 13-tap
// kernel, then back up with a tent filter, each level adding its own. The compose pass blends the top of it over the scene.

#include "scene_color.slang"

struct BloomPush
{
    float4 size; // destination extent, then the source's texel size
    float4 params; // x: 1 when the source is the compressed scene (the first level), y: tent radius in source texels
};

[[vk::push_constant]] BloomPush bp;
[[vk::binding(0, 0)]] Sampler2D srcTex;    // the level above (down) or below (up)
[[vk::binding(1, 0)]] Sampler2D addTex;    // up: the down chain at the destination's level
[[vk::binding(2, 0)]] RWTexture2D<float4> dstTex;

float3 tap(float2 uv)
{
    float3 c = srcTex.SampleLevel(uv, 0).rgb;
    return bp.params.x > 0.5 ? sceneExpand(c) : c;
}

float karis(float3 c)
{
    return 1.0 / (1.0 + dot(c, float3(0.2126, 0.7152, 0.0722)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void downCS(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(bp.size.xy)))
        return;
    float2 uv = (float2(id.xy) + 0.5) / bp.size.xy;
    float2 t = bp.size.zw;
    float3 a = tap(uv + t * float2(-2, -2)), b = tap(uv + t * float2(0, -2)), c = tap(uv + t * float2(2, -2));
    float3 d = tap(uv + t * float2(-2, 0)), e = tap(uv), f = tap(uv + t * float2(2, 0));
    float3 g = tap(uv + t * float2(-2, 2)), h = tap(uv + t * float2(0, 2)), k = tap(uv + t * float2(2, 2));
    float3 l = tap(uv + t * float2(-1, -1)), m = tap(uv + t * float2(1, -1)), n = tap(uv + t * float2(-1, 1)), o = tap(uv + t * float2(1, 1));
    float3 r;
    if (bp.params.x > 0.5)
    {
        // Karis average of the five 2x2 blocks on the first level, so a lone bright pixel can't flicker into a big blob.
        float3 g0 = (a + b + d + e) * 0.25, g1 = (b + c + e + f) * 0.25, g2 = (d + e + g + h) * 0.25, g3 = (e + f + h + k) * 0.25;
        float3 g4 = (l + m + n + o) * 0.25;
        float w0 = karis(g0), w1 = karis(g1), w2 = karis(g2), w3 = karis(g3), w4 = karis(g4);
        r = (g0 * w0 * 0.125 + g1 * w1 * 0.125 + g2 * w2 * 0.125 + g3 * w3 * 0.125 + g4 * w4 * 0.5) /
            max(w0 * 0.125 + w1 * 0.125 + w2 * 0.125 + w3 * 0.125 + w4 * 0.5, 1e-6);
    }
    else
        r = e * 0.125 + (a + c + g + k) * 0.03125 + (b + d + f + h) * 0.0625 + (l + m + n + o) * 0.125;
    dstTex[id.xy] = float4(r, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void upCS(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(bp.size.xy)))
        return;
    float2 uv = (float2(id.xy) + 0.5) / bp.size.xy;
    float2 t = bp.size.zw * bp.params.y;
    float3 s = srcTex.SampleLevel(uv, 0).rgb * 4.0;
    s += (srcTex.SampleLevel(uv + float2(-t.x, 0), 0).rgb + srcTex.SampleLevel(uv + float2(t.x, 0), 0).rgb +
          srcTex.SampleLevel(uv + float2(0, -t.y), 0).rgb + srcTex.SampleLevel(uv + float2(0, t.y), 0).rgb) * 2.0;
    s += srcTex.SampleLevel(uv + float2(-t.x, -t.y), 0).rgb + srcTex.SampleLevel(uv + float2(t.x, -t.y), 0).rgb +
         srcTex.SampleLevel(uv + float2(-t.x, t.y), 0).rgb + srcTex.SampleLevel(uv + float2(t.x, t.y), 0).rgb;
    dstTex[id.xy] = float4(addTex.SampleLevel(uv, 0).rgb + s / 16.0, 1.0);
}
