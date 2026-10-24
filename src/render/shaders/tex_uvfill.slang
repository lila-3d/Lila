struct UvPush
{
    uint2* dst; // float4 texels as two pairs, or half4 texels as one
    uint width;
    uint height;
    float u0;
    float v0;
    uint half;
    uint pad;
};

[[vk::push_constant]] UvPush up;

/** What a missing mesh map reads as: (u, v, 0, 1) at each texel's centre, offset to the UDIM tile. */
[shader("compute")]
[numthreads(8, 8, 1)]
void texUvFillCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= up.width || id.y >= up.height)
        return;
    float u = up.u0 + (float(id.x) + 0.5) / float(up.width);
    float v = up.v0 + 1.0 - (float(id.y) + 0.5) / float(up.height);
    uint i = id.y * up.width + id.x;
    if (up.half != 0)
        up.dst[i] = uint2(f32tof16(u) | (f32tof16(v) << 16), f32tof16(1.0) << 16);
    else
    {
        up.dst[2 * i] = asuint(float2(u, v));
        up.dst[2 * i + 1] = asuint(float2(0.0, 1.0));
    }
}

