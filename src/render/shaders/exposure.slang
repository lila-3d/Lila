// Auto exposure: a histogram of the scene's log luminance, then its average between two percentiles, eased toward over
// time. The compose pass multiplies by the result.

#include "scene_color.slang"

static const uint kBins = 64;

struct ExposurePush
{
    float4 size;  // scene extent, then its inverse
    float4 range; // x: log2 luminance of the first bin, y: of the last, z: lowest exposure (stops), w: highest
    float4 adapt; // x: blend toward the target this frame (1 jumps), y: compensation (stops), z: low percentile, w: high
};

[[vk::push_constant]] ExposurePush ep;
[[vk::binding(0, 0)]] Sampler2D sceneTex;
[[vk::binding(2, 0)]] RWTexture2D<uint> histogram; // kBins x 1
[[vk::binding(3, 0)]] RWTexture2D<float> state;    // x: multiplier, y: stops

groupshared uint gBins[kBins];

[shader("compute")]
[numthreads(16, 16, 1)]
void histogramCS(uint3 id: SV_DispatchThreadID, uint gi: SV_GroupIndex)
{
    if (gi < kBins)
        gBins[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    // Every fourth pixel each way is plenty for an average.
    uint2 px = id.xy * 4;
    if (all(px < uint2(ep.size.xy)))
    {
        float3 c = sceneExpand(sceneTex.Load(int3(px, 0)).rgb);
        float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
        if (lum > 1e-5)
        {
            float t = saturate((log2(lum) - ep.range.x) / (ep.range.y - ep.range.x));
            InterlockedAdd(gBins[min(uint(t * float(kBins)), kBins - 1)], 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < kBins && gBins[gi] > 0)
        InterlockedAdd(histogram[uint2(gi, 0)], gBins[gi]);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void averageCS(uint3 id: SV_DispatchThreadID)
{
    uint total = 0;
    for (uint i = 0; i < kBins; ++i)
        total += histogram[uint2(i, 0)];
    float low = float(total) * ep.adapt.z, high = float(total) * ep.adapt.w;
    float sum = 0.0, weight = 0.0, seen = 0.0;
    for (uint b = 0; b < kBins; ++b)
    {
        float count = float(histogram[uint2(b, 0)]);
        float lo = max(seen, low), hi = min(seen + count, high);
        seen += count;
        if (hi <= lo)
            continue;
        float logLum = ep.range.x + (float(b) + 0.5) / float(kBins) * (ep.range.y - ep.range.x);
        sum += logLum * (hi - lo);
        weight += hi - lo;
    }
    // Middle grey (18%) for the average, as a light meter does.
    float target = weight > 0.0 ? log2(0.18) - sum / weight : 0.0;
    target = clamp(target + ep.adapt.y, ep.range.z, ep.range.w);
    float stops = lerp(state[uint2(1, 0)], target, saturate(ep.adapt.x));
    state[uint2(1, 0)] = stops;
    state[uint2(0, 0)] = exp2(stops);
}
