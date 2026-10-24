// Virtual texture feedback: the cells the mesh pass wrote (a page request per sampled pixel of each 4x4 cell, see
// mesh.slang vtFeedback) deduped into a list for the host. A request is two words: group | cell << 16 | mip << 24 |
// 1 << 31, and the page's x | y << 16. The table is open addressing over a fingerprint of the request; a fingerprint
// collision between two requests drops one of them for this frame, which the next frames ask for again.

struct DedupePush
{
    uint2* cells;
    uint* table;
    uint* list; // count, then (from word 4) the unique requests
    uint count;
    uint groupsX;
    uint hashMask;
    uint cap;
};

[[vk::push_constant]] DedupePush dp;

uint hash32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vtDedupeCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    uint i = (gid.y * dp.groupsX + gid.x) * 64 + local.x;
    if (i >= dp.count)
        return;
    uint2 k = dp.cells[i];
    if ((k.x & 0x80000000u) == 0)
        return;
    uint h = hash32(k.x ^ hash32(k.y));
    uint fp = hash32(h ^ 0x9e3779b9u) | 1u;
    uint s = h & dp.hashMask;
    for (uint probe = 0; probe < 32; ++probe)
    {
        uint orig;
        InterlockedCompareExchange(dp.table[s], 0u, fp, orig);
        if (orig == 0u)
        {
            uint at;
            InterlockedAdd(dp.list[0], 1u, at);
            if (at < dp.cap)
            {
                dp.list[4 + at * 2] = k.x;
                dp.list[4 + at * 2 + 1] = k.y;
            }
            return;
        }
        if (orig == fp)
            return;
        s = (s + 1u) & dp.hashMask;
    }
}
