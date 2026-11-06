// Landform kernels on the GPU: the erosion filter (ErosionFilter.cpp's erosionFilterOffset per sample) and thermal
// erosion with a talus per sample (Landform.cpp's thermalMaterial). LandformGpu.cpp drives them; the CPU versions are the
// reference and these mirror their arithmetic in float.

static const uint kRowThreads = 262144u; // 4096 groups of 64 per row of a 2D dispatch
static const int kDx[8] = { 1, -1, 0, 0, 1, -1, 1, -1 };
static const int kDz[8] = { 0, 0, 1, -1, 1, -1, -1, 1 };
static const float kDist[8] = { 1, 1, 1, 1, 1.41421356, 1.41421356, 1.41421356, 1.41421356 };
static const float kTau = 6.28318530718;
static const float kReach = 1.25;

struct LandScene
{
    float* height;
    float* offset; // filter output
    float* talus;  // tan(angle) * cell per sample
    float* sum;
    float* move;
    float* delta;

    uint nx, nz;
    float cell, originX;
    float originZ, scale, strength, lacunarity;
    float gain, slopeFull;
    int octaves;
    uint seed;
};

struct LandPush
{
    LandScene* scene;
    uint pass;
    uint pad0;
};

[[vk::push_constant]] LandPush pc;

uint hash3(int x, int z, uint seed)
{
    uint h = uint(x) * 0x8da6b343u ^ uint(z) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float unitOf(uint h) { return float(h & 0xffffffu) / 16777216.0; }

float filterOffset(LandScene* s, float wx, float wz, float gx, float gz)
{
    float total = 0.0, dx = 0.0, dz = 0.0;
    float spacing = max(1.0, s->scale);
    float depth = s->strength * max(1.0, s->scale);
    for (int o = 0; o < s->octaves; ++o)
    {
        float ux = gx + dx, uz = gz + dz;
        float slope = sqrt(ux * ux + uz * uz);
        float f = 1.0 / spacing;
        if (slope > 1e-9)
        {
            float px = -uz / slope, pz = ux / slope;
            float qx = wx * f, qz = wz * f;
            int cx = int(floor(qx)), cz = int(floor(qz));
            float sw = 0.0, sv = 0.0, sdx = 0.0, sdz = 0.0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                {
                    uint hsh = hash3(cx + i, cz + j, s->seed * 31u + uint(o));
                    float ox = float(cx + i) + 0.25 + 0.5 * unitOf(hsh);
                    float oz = float(cz + j) + 0.25 + 0.5 * unitOf((hsh * 0x9e3779b9u) >> 8);
                    float ddx = qx - ox, ddz = qz - oz;
                    float t = 1.0 - (ddx * ddx + ddz * ddz) / (kReach * kReach);
                    if (t <= 0.0)
                        continue;
                    float w = t * t * t;
                    float phase = (ddx * px + ddz * pz) * kTau;
                    sw += w;
                    sv += w * cos(phase);
                    float sn = -sin(phase) * kTau * w;
                    sdx += sn * px;
                    sdz += sn * pz;
                }
            if (sw > 1e-12)
            {
                float fade = clamp(slope / max(1e-6, s->slopeFull), 0.0, 1.0);
                float a = depth * fade;
                total += a * sv / sw;
                dx += a * sdx / sw * f;
                dz += a * sdz / sw * f;
            }
        }
        spacing /= max(1.01, s->lacunarity);
        depth *= clamp(s->gain, 0.0, 1.0);
    }
    return total;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void filterCS(uint3 tid: SV_DispatchThreadID)
{
    LandScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= s->nx * s->nz)
        return;
    int x = int(i % s->nx), z = int(i / s->nx);
    int x0 = max(0, x - 1), x1 = min(int(s->nx) - 1, x + 1);
    int z0 = max(0, z - 1), z1 = min(int(s->nz) - 1, z + 1);
    float gx = (s->height[uint(z) * s->nx + uint(x1)] - s->height[uint(z) * s->nx + uint(x0)]) / (float(x1 - x0) * s->cell);
    float gz = (s->height[uint(z1) * s->nx + uint(x)] - s->height[uint(z0) * s->nx + uint(x)]) / (float(z1 - z0) * s->cell);
    s->offset[i] = filterOffset(s, s->originX + float(x) * s->cell, s->originZ + float(z) * s->cell, gx, gz);
}

float talusExcess(LandScene* s, uint i, int x, int z, int k)
{
    int nx = x + kDx[k], nz = z + kDz[k];
    if (nx < 0 || nz < 0 || nx >= int(s->nx) || nz >= int(s->nz))
        return 0.0;
    return max(0.0, s->height[i] - s->height[uint(nz) * s->nx + uint(nx)] - s->talus[i] * kDist[k]);
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
    float total = 0.0, most = 0.0;
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
    float gain = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        int nx = x + kDx[k], nz = z + kDz[k];
        if (nx < 0 || nz < 0 || nx >= int(s->nx) || nz >= int(s->nz))
            continue;
        uint j = uint(nz) * s->nx + uint(nx);
        if (s->sum[j] <= 0.0)
            continue;
        gain += s->move[j] * talusExcess(s, j, nx, nz, k ^ 1) / s->sum[j];
    }
    s->delta[i] = gain - (s->sum[i] > 0.0 ? s->move[i] : 0.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void talusApplyCS(uint3 tid: SV_DispatchThreadID)
{
    LandScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= s->nx * s->nz)
        return;
    s->height[i] += s->delta[i];
}
