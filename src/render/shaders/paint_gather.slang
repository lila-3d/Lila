// A paint channel tile's texels gathered from its 128x128 RGBA16F pages into a rectangle of float4 texels, at the tile's
// own size or a smaller level of it (a box of up to 4x4 samples per texel), clamped at the tile's edges. For virtual
// texture pages of a texture graph reading paint (render/PaintGpu.h recordGather).

static const uint kPage = 128;

struct GatherPush
{
    uint64_t* table; // per page (row major), its RGBA16F texels' address or 0 where nothing is painted
    uint2* dst;      // float4 texels as two pairs
    int x0;          // the rectangle's first texel, in level texels
    int y0;
    uint width;      // the rectangle
    uint height;
    uint tileW;
    uint tileH;
    uint levelW;
    uint levelH;
    uint pagesX;
    uint dstStride;
    uint groupsX;
    uint pad;
};

[[vk::push_constant]] GatherPush gp;

float4 texelAt(int2 p)
{
    uint2 q = uint2(clamp(p, int2(0, 0), int2(int(gp.tileW), int(gp.tileH)) - 1));
    uint64_t page = gp.table[(q.y / kPage) * gp.pagesX + q.x / kPage];
    if (page == 0)
        return float4(0.0);
    uint2* texels = (uint2*)page;
    uint2 h = texels[(q.y % kPage) * kPage + q.x % kPage];
    return float4(f16tof32(h.x), f16tof32(h.x >> 16), f16tof32(h.y), f16tof32(h.y >> 16));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void paintGatherCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    uint i = (gid.y * gp.groupsX + gid.x) * 64 + local.x;
    if (i >= gp.width * gp.height)
        return;
    uint x = i % gp.width, y = i / gp.width;
    int2 level = int2(gp.x0 + int(x), gp.y0 + int(y));
    level = clamp(level, int2(0, 0), int2(int(gp.levelW), int(gp.levelH)) - 1);
    float2 scale = float2(float(gp.tileW) / float(gp.levelW), float(gp.tileH) / float(gp.levelH));
    float4 v;
    if (scale.x <= 1.0 && scale.y <= 1.0)
        v = texelAt(int2(floor((float2(level) + 0.5) * scale)));
    else
    {
        uint2 n = uint2(clamp(ceil(scale), float2(1.0), float2(4.0)));
        v = float4(0.0);
        for (uint sy = 0; sy < n.y; ++sy)
            for (uint sx = 0; sx < n.x; ++sx)
                v += texelAt(int2(floor((float2(level) + (float2(sx, sy) + 0.5) / float2(n)) * scale)));
        v /= float(n.x * n.y);
    }
    uint o = y * gp.dstStride + x;
    gp.dst[2 * o] = asuint(v.xy);
    gp.dst[2 * o + 1] = asuint(v.zw);
}
