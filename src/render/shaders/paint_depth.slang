// The painted mesh's depth from a stroke's view, for occlusion while painting in texture space: one thread per triangle
// rasterising its screen box. Depth is Vulkan NDC z (reversed: larger is nearer), kept with an atomic max of its bits,
// which order positive floats like the floats themselves, so the result does not depend on thread order.

struct Tri
{
    float4 p[3];
    float4 n[3];
    float4 t[3];
};

struct DepthPush
{
    float4x4 objectToClip;
    Tri* tris;
    uint* depth;
    uint triCount;
    uint width;
    uint height;
    uint groupsX;
};

[[vk::push_constant]] DepthPush dp;

float edge(float2 a, float2 b, float2 p)
{
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void paintDepthCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    uint t = (gid.y * dp.groupsX + gid.x) * 64 + local.x;
    if (t >= dp.triCount)
        return;
    Tri tri = dp.tris[t];
    float2 s[3];
    float z[3];
    for (int k = 0; k < 3; ++k)
    {
        float4 c = mul(dp.objectToClip, float4(tri.p[k].xyz, 1.0));
        // Triangles crossing the eye plane are left out rather than clipped.
        if (c.w <= 1e-6)
            return;
        float3 n = c.xyz / c.w;
        s[k] = (n.xy * 0.5 + 0.5) * float2(float(dp.width), float(dp.height));
        z[k] = n.z;
    }
    float area = edge(s[0], s[1], s[2]);
    if (abs(area) < 1e-12)
        return;
    float2 lo = min(s[0], min(s[1], s[2]));
    float2 hi = max(s[0], max(s[1], s[2]));
    int x0 = max(int(floor(lo.x)), 0), y0 = max(int(floor(lo.y)), 0);
    int x1 = min(int(ceil(hi.x)), int(dp.width) - 1), y1 = min(int(ceil(hi.y)), int(dp.height) - 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            float2 p = float2(float(x), float(y)) + 0.5;
            float w0 = edge(s[1], s[2], p) / area;
            float w1 = edge(s[2], s[0], p) / area;
            float w2 = edge(s[0], s[1], p) / area;
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0)
                continue;
            float d = w0 * z[0] + w1 * z[1] + w2 * z[2];
            if (d > 0.0)
                InterlockedMax(dp.depth[y * int(dp.width) + x], asuint(d));
        }
}
