// Ray-traced ambient occlusion: from each pixel of the depth prepass, a few cosine-weighted rays into the scene's
// acceleration structure. A new set each frame; the denoise pass and temporal antialiasing average them.

struct RtaoPush
{
    float4x4 invViewProj; // as the prepass was drawn
    float4 params;        // x radius (cm), y intensity, z frame seed, w rays per pixel
    float4 size;          // extent, then its inverse
};

[[vk::push_constant]] RtaoPush rp;
[[vk::binding(0, 0)]] Sampler2D depthTex;
[[vk::binding(1, 0)]] Sampler2D normalTex;
[[vk::binding(2, 0)]] RaytracingAccelerationStructure sceneAs;
[[vk::binding(3, 0)]] RWTexture2D<float> aoOut;

uint hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

float rand(inout uint state)
{
    state = hash(state);
    return float(state & 0xffffff) / 16777216.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rtaoCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (any(px >= int2(rp.size.xy)))
        return;
    float depth = depthTex.Load(int3(px, 0)).r;
    float3 n = normalTex.Load(int3(px, 0)).xyz;
    if (depth <= 0.0 || dot(n, n) < 0.01)
    {
        aoOut[px] = 1.0;
        return;
    }
    n = normalize(n);
    float2 ndc = (float2(px) + 0.5) * rp.size.zw * 2.0 - 1.0;
    float4 h = mul(rp.invViewProj, float4(ndc, depth, 1.0));
    float3 P = h.xyz / h.w;
    // Depth stores the surface only so precisely: start the rays clear of it, toward the camera and along the normal.
    float4 eyeH = mul(rp.invViewProj, float4(ndc, 1.0, 1.0));
    float3 toEye = eyeH.xyz / eyeH.w - P;
    float dist = length(toEye);
    P += toEye / max(dist, 1e-4) * dist * 1e-3 + n * max(0.05, dist * 1e-3);

    float3 t = abs(n.y) < 0.99 ? normalize(cross(float3(0, 1, 0), n)) : normalize(cross(float3(1, 0, 0), n));
    float3 b = cross(n, t);
    uint state = hash(uint(px.x) * 1973u + uint(px.y) * 9277u + uint(rp.params.z) * 26699u);
    const uint rays = uint(max(rp.params.w, 1.0));
    const float radius = max(rp.params.x, 0.01);
    float occlusion = 0.0;
    for (uint k = 0; k < rays; ++k)
    {
        float u1 = rand(state), u2 = rand(state);
        float r = sqrt(u1), phi = 6.2831853 * u2;
        float3 dir = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(1.0 - u1, 0.0)));
        RayDesc ray;
        ray.Origin = P;
        ray.Direction = dir;
        ray.TMin = 0.0;
        ray.TMax = radius;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(sceneAs, RAY_FLAG_NONE, 0xff, ray);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            // Near hits darken fully, far ones fade out toward the radius.
            float d = q.CommittedRayT() / radius;
            occlusion += 1.0 - d * d;
        }
    }
    aoOut[px] = saturate(1.0 - occlusion / float(rays) * rp.params.y);
}
