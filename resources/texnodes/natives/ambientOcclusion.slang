// Ambient occlusion from a height: per direction the steepest rise within the radius, occlusion the mean sine of those
// angles. Contract in src/tex/Natives.h. gU: 0 height, 1 output, 2 radius, 3 depth, 4 directions, 5 steps, 6 strength.

void occlusion(uint3 id)
{
    int2 p = int2(id.xy);
    if (p.x >= int(gPass.res.x) || p.y >= int(gPass.res.y))
        return;
    float2 uv = float2((float(p.x) + 0.5) * gPass.res.z, 1.0 - (float(p.y) + 0.5) * gPass.res.w);
    float radius = max(gU[2].x, 1e-5);
    float depth = gU[3].x;
    int dirs = clamp(int(gU[4].x), 1, 64);
    int steps = clamp(int(gU[5].x), 1, 64);
    float h0 = texReadBuf(0, uv).x;
    float occluded = 0.0;
    for (int d = 0; d < dirs; ++d)
    {
        float a = 6.28318530718 * (float(d) + 0.5) / float(dirs);
        float2 dir = float2(cos(a), sin(a));
        float steepest = 0.0;
        for (int s = 1; s <= steps; ++s)
        {
            float t = radius * float(s) / float(steps);
            steepest = max(steepest, (texReadBuf(0, uv + dir * t).x - h0) * depth / t);
        }
        occluded += steepest / sqrt(1.0 + steepest * steepest);
    }
    float v = saturate(1.0 - gU[6].x * occluded / float(dirs));
    nativeStore(p, float4(v, v, v, 1.0));
}
