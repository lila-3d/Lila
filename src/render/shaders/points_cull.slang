// Thins a dense cloud of component dots before it is drawn: the nearest point in each cell of a screen grid a dot or
// so across is kept and the rest dropped, since a dot covers its cell anyway. pointsNearestCS records each cell's
// nearest depth, then pointsKeepCS lets the point at that depth (one per cell) through to a compact list, counted for
// an indirect draw with the points pipeline.

struct PointIn
{
    float3 pos;
    uint state; // as points.slang: above 2 is hidden
};

struct CullPush
{
    float4x4 viewProj;
    uint4 grid;      // x, y: cells across and down; z: cell size in pixels; w: points
    float4 viewport; // x, y: size in pixels
    PointIn* points;
    uint* cells;     // per cell its nearest depth's bits, then from grid.x * grid.y on the point that claimed it, plus one
    PointIn* out;
    uint* args;      // VkDrawIndirectCommand
};

[[vk::push_constant]] CullPush cp;

/** The point's cell and depth bits (reversed Z: larger is nearer), or false when it doesn't draw. */
bool cellOf(PointIn p, out uint cell, out uint depth)
{
    cell = 0;
    depth = 0;
    if (p.state > 2)
        return false;
    const float4 c = mul(cp.viewProj, float4(p.pos, 1.0));
    if (c.w <= 1e-6)
        return false;
    const float3 ndc = c.xyz / c.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
        return false;
    const float2 px = (ndc.xy * 0.5 + 0.5) * cp.viewport.xy;
    const uint2 xy = min(uint2(px / float(cp.grid.z)), cp.grid.xy - 1);
    cell = xy.y * cp.grid.x + xy.x;
    depth = asuint(ndc.z);
    return true;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void pointsNearestCS(uint3 id : SV_DispatchThreadID)
{
    uint cell, depth;
    if (id.x < cp.grid.w && cellOf(cp.points[id.x], cell, depth))
        InterlockedMax(cp.cells[cell], depth);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void pointsKeepCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= cp.grid.w)
        return;
    const PointIn p = cp.points[id.x];
    uint cell, depth;
    if (!cellOf(p, cell, depth) || cp.cells[cell] != depth)
        return;
    // Points at exactly the same depth share a cell: the first to claim it draws.
    uint prev;
    InterlockedCompareExchange(cp.cells[cp.grid.x * cp.grid.y + cell], 0u, id.x + 1u, prev);
    if (prev != 0u)
        return;
    uint slot;
    InterlockedAdd(cp.args[0], 1u, slot);
    cp.out[slot] = p;
}
