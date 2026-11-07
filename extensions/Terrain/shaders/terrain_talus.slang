// Thermal erosion with a talus per sample (Landform.cpp's thermalMaterial), bit for bit: every operation is IEEE
// float in the CPU's order, nothing contracts to an fma, and the one division runs in double, which rounds to the
// correctly rounded float quotient (Vulkan's float division may be off by ulps). Needs shaderFloat64; LandformGpu.cpp
// drives it.

#include "terrain_landform.slang"

float talusExcess(LandScene* s, uint i, int x, int z, int k)
{
    int nx = x + kDx[k], nz = z + kDz[k];
    if (nx < 0 || nz < 0 || nx >= int(s->nx) || nz >= int(s->nz))
        return 0.0;
    precise float drop = s->height[i] - s->height[uint(nz) * s->nx + uint(nx)];
    precise float hold = s->talus[i] * kDist[k];
    precise float e = drop - hold;
    return max(0.0, e);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void talusShedCS(uint3 tid: SV_DispatchThreadID)
{
    LandScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= s->nx * s->nz)
        return;
    int x = int(i % s->nx), z = int(i / s->nx);
    precise float total = 0.0;
    float most = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        float e = talusExcess(s, i, x, z, k);
        total += e;
        most = max(most, e);
    }
    s->sum[i] = total;
    s->move[i] = 0.5 * most;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void talusGatherCS(uint3 tid: SV_DispatchThreadID)
{
    LandScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= s->nx * s->nz)
        return;
    int x = int(i % s->nx), z = int(i / s->nx);
    precise float gain = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        int nx = x + kDx[k], nz = z + kDz[k];
        if (nx < 0 || nz < 0 || nx >= int(s->nx) || nz >= int(s->nz))
            continue;
        uint j = uint(nz) * s->nx + uint(nx);
        if (s->sum[j] <= 0.0)
            continue;
        precise float share = s->move[j] * talusExcess(s, j, nx, nz, k ^ 1);
        gain += float(double(share) / double(s->sum[j]));
    }
    precise float d = gain - (s->sum[i] > 0.0 ? s->move[i] : 0.0);
    s->delta[i] = d;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void talusApplyCS(uint3 tid: SV_DispatchThreadID)
{
    LandScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= s->nx * s->nz)
        return;
    precise float h = s->height[i] + s->delta[i];
    s->height[i] = h;
}
