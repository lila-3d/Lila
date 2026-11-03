// Terrain erosion on the GPU. Erosion.cpp's erode() is the reference and every kernel mirrors a step of it: the
// depression fill (as ordered-integer keys, so "the next float up" is key + 1 like the CPU's nextafter), steepest-descent
// receivers, drainage area and the implicit stream-power solve (both by pointer jumping along the receiver tree), and
// thermal relaxation. ErosionGpu.cpp drives them.

static const uint kNone = 0xffffffffu;
static const uint kRowThreads = 262144u; // 4096 groups of 64 per row of a 2D dispatch
static const int kDx[8] = { 1, -1, 0, 0, 1, -1, 1, -1 };
static const int kDz[8] = { 0, 0, 1, -1, 1, -1, -1, 1 };
static const float kDist[8] = { 1, 1, 1, 1, 1.41421356, 1.41421356, 1.41421356, 1.41421356 };

struct ErodeScene
{
    float* height;   // the heightfield
    uint* key;       // ordered keys of height
    uint* fill;      // filled surface as keys
    uint* coarse;    // coarse block maxima, then the coarse fill bound
    uint* flag;      // [0] a relaxation sweep changed something
    uint* receiver;
    uint* ptr0;      // jump pointers, double buffered
    uint* ptr1;
    uint* count0;    // descendants within the jump, double buffered
    uint* count1;
    float* a0;       // z = a + b * z[ptr], double buffered
    float* a1;
    float* b0;
    float* b1;
    float* uplift;
    float* hardness;
    float* edge;     // heights the edge is held at
    float* sum;      // thermal scratch
    float* move;
    float* delta;

    uint nx, nz, cnx, cnz;
    uint block, margin, pad0, pad1;
    float cellM, strength, areaExponent, upliftRate;
    float talus, pad2, pad3, pad4;
};

struct ErodePush
{
    ErodeScene* scene;
    uint pass;
    uint flip; // 0 reads the *0 buffers and writes *1, 1 the other way
    uint pad0;
    uint pad1;
};

[[vk::push_constant]] ErodePush pc;

uint toKey(float f)
{
    uint b = asuint(f);
    return (b & 0x80000000u) != 0u ? ~b : (b | 0x80000000u);
}

float fromKey(uint k)
{
    return asfloat((k & 0x80000000u) != 0u ? (k & 0x7fffffffu) : ~k);
}

bool onEdge(ErodeScene* s, uint x, uint z) { return x == 0u || z == 0u || x == s->nx - 1u || z == s->nz - 1u; }

uint cellCount(ErodeScene* s) { return s->nx * s->nz; }

[shader("compute")]
[numthreads(64, 1, 1)]
void keysCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    s->key[i] = toKey(s->height[i]);
}

/** One thread per coarse block: the highest key in it. */
[shader("compute")]
[numthreads(64, 1, 1)]
void blockMaxCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint c = tid.x + tid.y * kRowThreads;
    if (c >= s->cnx * s->cnz)
        return;
    uint cx = c % s->cnx, cz = c / s->cnx;
    uint best = 0u;
    for (uint dz = 0u; dz < s->block; ++dz)
        for (uint dx = 0u; dx < s->block; ++dx)
        {
            uint x = cx * s->block + dx, z = cz * s->block + dz;
            if (x < s->nx && z < s->nz)
                best = max(best, s->key[z * s->nx + x]);
        }
    s->coarse[c] = best;
}

/** The fill starts at an upper bound: the coarse fill of its block plus a margin for the steps within a block. */
[shader("compute")]
[numthreads(64, 1, 1)]
void initFillCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    uint x = i % s->nx, z = i / s->nx;
    uint k = s->key[i];
    if (onEdge(s, x, z))
    {
        s->fill[i] = k;
        return;
    }
    uint c = (z / s->block) * s->cnx + (x / s->block);
    uint bound = s->coarse[c];
    bound = bound > 0xffffffffu - s->margin ? 0xffffffffu : bound + s->margin;
    s->fill[i] = max(k, bound);
}

/** Lowers each fill toward max(own key, lowest neighbour + 1); in place, which only speeds the descent to the same answer. */
[shader("compute")]
[numthreads(64, 1, 1)]
void relaxCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    uint x = i % s->nx, z = i / s->nx;
    if (onEdge(s, x, z))
        return;
    uint lowest = 0xffffffffu;
    for (int k = 0; k < 8; ++k)
        lowest = min(lowest, s->fill[(z + kDz[k]) * s->nx + (x + kDx[k])]);
    uint w = max(s->key[i], lowest == 0xffffffffu ? lowest : lowest + 1u);
    if (w < s->fill[i])
    {
        s->fill[i] = w;
        s->flag[0] = 1u;
    }
}

/** Steepest descent on the filled surface (the CPU's loop and tie order), the first jump pointer and donor counts. */
[shader("compute")]
[numthreads(64, 1, 1)]
void receiversCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    uint x = i % s->nx, z = i / s->nx;
    uint r = i;
    if (!onEdge(s, x, z))
    {
        float fi = fromKey(s->fill[i]);
        float best = 0.0;
        for (int k = 0; k < 8; ++k)
        {
            uint j = (z + kDz[k]) * s->nx + (x + kDx[k]);
            float slope = (fi - fromKey(s->fill[j])) / kDist[k];
            if (slope > best)
            {
                best = slope;
                r = j;
            }
        }
    }
    s->receiver[i] = r;
    s->ptr0[i] = r == i ? kNone : r;
    s->count0[i] = 0u;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void donorsCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    if (s->ptr0[i] != kNone)
        InterlockedAdd(s->count0[s->ptr0[i]], 1u);
}

/** The next pass's counts start as this pass's (copied before), then gain each jump source's count; pointers double. */
[shader("compute")]
[numthreads(64, 1, 1)]
void areaJumpCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    uint* pIn = pc.flip == 0u ? s->ptr0 : s->ptr1;
    uint* pOut = pc.flip == 0u ? s->ptr1 : s->ptr0;
    uint* cIn = pc.flip == 0u ? s->count0 : s->count1;
    uint* cOut = pc.flip == 0u ? s->count1 : s->count0;
    uint p = pIn[i];
    if (p == kNone)
    {
        pOut[i] = kNone;
        return;
    }
    InterlockedAdd(cOut[p], cIn[i]);
    pOut[i] = pIn[p];
}

/** z_i = a_i + b_i z_receiver: the implicit stream-power step before the tree is collapsed. */
[shader("compute")]
[numthreads(64, 1, 1)]
void solveInitCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    uint r = s->receiver[i];
    float zf = fromKey(s->fill[i]);
    if (r == i)
    {
        s->a0[i] = zf;
        s->b0[i] = 0.0;
        s->ptr0[i] = kNone;
        return;
    }
    int x = int(i % s->nx), zi = int(i / s->nx), rx = int(r % s->nx), rz = int(r / s->nx);
    float dist = sqrt(float((x - rx) * (x - rx) + (zi - rz) * (zi - rz))) * s->cellM;
    float area = float(1u + s->count0[i]) * s->cellM * s->cellM;
    float k = s->strength * (1.0 - 0.9 * clamp(s->hardness[i], 0.0, 1.0));
    float f = k * pow(area, s->areaExponent) / dist;
    float raised = zf + s->upliftRate * s->uplift[i];
    s->a0[i] = raised / (1.0 + f);
    s->b0[i] = f / (1.0 + f);
    s->ptr0[i] = r;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void solveJumpCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    uint* pIn = pc.flip == 0u ? s->ptr0 : s->ptr1;
    uint* pOut = pc.flip == 0u ? s->ptr1 : s->ptr0;
    float* aIn = pc.flip == 0u ? s->a0 : s->a1;
    float* aOut = pc.flip == 0u ? s->a1 : s->a0;
    float* bIn = pc.flip == 0u ? s->b0 : s->b1;
    float* bOut = pc.flip == 0u ? s->b1 : s->b0;
    uint p = pIn[i];
    if (p == kNone)
    {
        aOut[i] = aIn[i];
        bOut[i] = bIn[i];
        pOut[i] = kNone;
        return;
    }
    aOut[i] = aIn[i] + bIn[i] * aIn[p];
    bOut[i] = bIn[i] * bIn[p];
    pOut[i] = pIn[p];
}

[shader("compute")]
[numthreads(64, 1, 1)]
void solveStoreCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    s->height[i] = pc.flip == 0u ? s->a0[i] : s->a1[i];
}

float excess(ErodeScene* s, uint i, int x, int z, int k)
{
    int nx = x + kDx[k], nz = z + kDz[k];
    if (nx < 0 || nz < 0 || nx >= int(s->nx) || nz >= int(s->nz))
        return 0.0;
    return max(0.0, s->height[i] - s->height[uint(nz) * s->nx + uint(nx)] - s->talus * kDist[k]);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void thermalShedCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    int x = int(i % s->nx), z = int(i / s->nx);
    float total = 0.0, most = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        float e = excess(s, i, x, z, k);
        total += e;
        most = max(most, e);
    }
    s->sum[i] = total;
    s->move[i] = 0.5 * most;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void thermalGatherCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
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
        float e = excess(s, j, nx, nz, k ^ 1);
        gain += s->move[j] * e / s->sum[j];
    }
    s->delta[i] = gain - (s->sum[i] > 0.0 ? s->move[i] : 0.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void thermalApplyCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    s->height[i] += s->delta[i];
}

/** The edge is base level: back to where it started after thermal moved it. */
[shader("compute")]
[numthreads(64, 1, 1)]
void holdEdgeCS(uint3 tid: SV_DispatchThreadID)
{
    ErodeScene* s = pc.scene;
    uint i = tid.x + tid.y * kRowThreads;
    if (i >= cellCount(s))
        return;
    if (onEdge(s, i % s->nx, i / s->nx))
        s->height[i] = s->edge[i];
}
