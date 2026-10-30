// Ray-traced shadows for up to four lights: from each pixel of the depth prepass, a few rays toward each light spread
// over its size (the sun's disc, a lamp's sphere). The mesh shader reads a light's channel in place of a shadow map;
// temporal antialiasing averages the softness.

struct ShadowPush
{
    float4x4 invViewProj; // as the prepass was drawn
    float4 position[4];   // xyz, w: 1 for a light at a point, 0 for a direction (xyz toward it)
    float4 size[4];       // x: radius (cm) for points, tangent of the angular radius for directions; y: range (0 none)
    float4 params;        // x count, z frame seed, w rays per light
    float4 extent;        // extent, then its inverse
};

[[vk::push_constant]] ShadowPush sp;
[[vk::binding(0, 0)]] Sampler2D depthTex;
[[vk::binding(1, 0)]] Sampler2D normalTex;
[[vk::binding(2, 0)]] RaytracingAccelerationStructure sceneAs;
[[vk::binding(3, 0)]] RWTexture2D<float4> shadowOut; // a channel per light: 1 lit, 0 shadowed

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
void shadowCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (any(px >= int2(sp.extent.xy)))
        return;
    float depth = depthTex.Load(int3(px, 0)).r;
    float3 n = normalTex.Load(int3(px, 0)).xyz;
    if (depth <= 0.0 || dot(n, n) < 0.01)
    {
        shadowOut[px] = float4(1, 1, 1, 1);
        return;
    }
    n = normalize(n);
    float2 ndc = (float2(px) + 0.5) * sp.extent.zw * 2.0 - 1.0;
    float4 h = mul(sp.invViewProj, float4(ndc, depth, 1.0));
    float3 P = h.xyz / h.w;
    float4 eyeH = mul(sp.invViewProj, float4(ndc, 1.0, 1.0));
    float3 toEye = eyeH.xyz / eyeH.w - P;
    float dist = length(toEye);
    // Clear of the surface by more than the depth's own error at this distance.
    P += toEye / max(dist, 1e-4) * dist * 1e-3 + n * max(0.05, dist * 1e-3);

    uint state = hash(uint(px.x) * 2654435u + uint(px.y) * 40503u + uint(sp.params.z) * 9973u);
    const uint rays = uint(max(sp.params.w, 1.0));
    const uint count = min(uint(sp.params.x), 4u);
    float4 result = float4(1, 1, 1, 1);
    for (uint li = 0; li < count; ++li)
    {
        const bool point = sp.position[li].w > 0.5;
        float3 toLight = point ? sp.position[li].xyz - P : sp.position[li].xyz;
        float reach = point ? length(toLight) : 1e7;
        float3 L = normalize(toLight);
        if (dot(n, L) <= 0.0)
        {
            result[li] = 0.0;
            continue;
        }
        float3 t = abs(L.y) < 0.99 ? normalize(cross(float3(0, 1, 0), L)) : normalize(cross(float3(1, 0, 0), L));
        float3 b = cross(L, t);
        float spread = point ? sp.size[li].x / max(reach, 1e-3) : sp.size[li].x;
        float lit = 0.0;
        for (uint k = 0; k < rays; ++k)
        {
            float r = sqrt(rand(state)) * spread, phi = 6.2831853 * rand(state);
            RayDesc ray;
            ray.Origin = P;
            ray.Direction = normalize(L + t * (r * cos(phi)) + b * (r * sin(phi)));
            ray.TMin = 0.0;
            // A light at a point stops just short of it, so its own glyph or bulb never shadows it.
            ray.TMax = point ? reach * 0.995 : reach;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
            q.TraceRayInline(sceneAs, RAY_FLAG_NONE, 0xff, ray);
            q.Proceed();
            lit += q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
        }
        result[li] = lit / float(rays);
    }
    shadowOut[px] = result;
}
