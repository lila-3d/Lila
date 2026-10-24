//! Cellular noise: distances to the nearest and second nearest feature points, tileable with the period equal to the scale.

struct VoronoiResult
{
    float f1;
    float f2;
    float cellRandom;
};

VoronoiResult voronoi(float2 uv, int2 scale, float jitter, uint seed)
{
    float2 q = uv * float2(scale);
    float2 i = floor(q);
    float2 f = q - i;
    int2 c = int2(i);
    VoronoiResult r;
    r.f1 = 8.0;
    r.f2 = 8.0;
    r.cellRandom = 0.0;
    // 5 x 5 cells: the second nearest point can lie two cells away.
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            int2 n = c + int2(x, y);
            int2 w = ((n % scale) + scale) % scale;
            float2 o = float2(float(x), float(y)) + 0.5 + (texRand2(uint2(w), seed) - 0.5) * jitter;
            float d = length(o - f);
            if (d < r.f1)
            {
                r.f2 = r.f1;
                r.f1 = d;
                r.cellRandom = texRand(uint2(w), seed ^ 0x2c1b3c6du);
            }
            else if (d < r.f2)
                r.f2 = d;
        }
    }
    return r;
}
