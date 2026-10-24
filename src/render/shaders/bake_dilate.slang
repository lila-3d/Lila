// Jump flood dilation: every texel finds its nearest covered texel (Chebyshev distance, so the reach matches the CPU's
// ring flood, ties to the Euclidean nearer one). The CPU then copies values from the seeds, so exact maps stay exact.

struct DilatePush
{
    uint* covered; // one byte per texel
    uint* seedsIn;
    uint* seedsOut;
    uint width;
    uint height;
    int step;
    uint pad;
};

[[vk::push_constant]] DilatePush dp;

static const uint kNone = 0xffffffffu;

bool isCovered(uint i)
{
    return ((dp.covered[i >> 2] >> ((i & 3) * 8)) & 0xff) != 0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void jfaInitCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= dp.width || id.y >= dp.height)
        return;
    uint i = id.y * dp.width + id.x;
    dp.seedsOut[i] = isCovered(i) ? i : kNone;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void jfaStepCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= dp.width || id.y >= dp.height)
        return;
    int2 p = int2(id.xy);
    uint best = kNone;
    int bestCheb = 0x7fffffff;
    int bestEuclid = 0x7fffffff;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 q = p + int2(dx, dy) * dp.step;
            if (any(q < 0) || q.x >= int(dp.width) || q.y >= int(dp.height))
                continue;
            uint s = dp.seedsIn[uint(q.y) * dp.width + uint(q.x)];
            if (s == kNone)
                continue;
            int2 sp = int2(int(s % dp.width), int(s / dp.width));
            int2 d = abs(sp - p);
            int cheb = max(d.x, d.y);
            int euclid = d.x * d.x + d.y * d.y;
            if (cheb < bestCheb || (cheb == bestCheb && (euclid < bestEuclid || (euclid == bestEuclid && s < best))))
            {
                best = s;
                bestCheb = cheb;
                bestEuclid = euclid;
            }
        }
    dp.seedsOut[id.y * dp.width + id.x] = best;
}
