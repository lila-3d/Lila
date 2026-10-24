#include "tex_formats.slang"

struct StorePush
{
    uint2* arena; // float4 texels as two pairs, or half4 texels as one
    uint* dst;
    // Per output channel: arena offset in pairs, width, height, component (kConstant: constants[c]) | kHalfSource.
    uint4 src[4];
    float4 constants;
    uint width;
    uint height;
    uint format;
    uint srgb;
    uint words;
    uint groupsX;
    uint origin; // the rectangle written: x | y << 16
    uint extent; // width | height << 16
};

[[vk::push_constant]] StorePush sp;

static const uint kHalfSource = 0x100;

/** Component s.w of texel i of a source buffer. */
float component(uint4 s, uint i)
{
    uint c = s.w & 3;
    if ((s.w & kHalfSource) != 0)
    {
        uint2 q = sp.arena[s.x + i];
        return f16tof32((c < 2 ? q.x : q.y) >> ((c & 1) * 16));
    }
    uint2 q = sp.arena[s.x + 2 * i + c / 2];
    return asfloat((c & 1) != 0 ? q.y : q.x);
}

float fetch(uint4 s, int2 p)
{
    int w = int(s.y), h = int(s.z);
    int x = ((p.x % w) + w) % w;
    int y = ((p.y % h) + h) % h;
    return component(s, uint(y * w + x));
}

/** Channel c of the output texel (x, y): the source buffer resampled bilinearly (wrapping) to the output size. */
float channelValue(uint c, uint x, uint y)
{
    uint4 s = sp.src[c];
    if ((s.w & 0xff) >= kConstant)
        return sp.constants[c];
    if (s.y == sp.width && s.z == sp.height)
        return component(s, y * s.y + x);
    float2 p = (float2(float(x), float(y)) + 0.5) * float2(float(s.y) / float(sp.width), float(s.z) / float(sp.height)) - 0.5;
    float2 f = floor(p);
    float2 t = p - f;
    int2 i = int2(f);
    float a = fetch(s, i), b = fetch(s, i + int2(1, 0)), d = fetch(s, i + int2(0, 1)), e = fetch(s, i + int2(1, 1));
    return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y);
}

uint2 texelXY(uint texel)
{
    uint w = sp.extent & 0xffff;
    return uint2(sp.origin & 0xffff, sp.origin >> 16) + uint2(texel % w, texel / w);
}

uint encodeByte(uint texel, uint c)
{
    uint2 p = texelXY(texel);
    float v = channelValue(c, p.x, p.y);
    return unorm8(sp.srgb != 0 && sp.format == kRGBA8 && c < 3 ? toSrgb(v) : v);
}

uint encodeHalf(uint texel, uint c)
{
    uint2 p = texelXY(texel);
    return f32tof16(channelValue(c, p.x, p.y));
}

/** One 32-bit word of the rectangle's tightly packed texels per thread, for a buffer-to-image copy. */
[shader("compute")]
[numthreads(64, 1, 1)]
void texStoreCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    uint k = flatIndex(gid, local, sp.groupsX);
    if (k >= sp.words)
        return;
    uint texels = (sp.extent & 0xffff) * (sp.extent >> 16);
    uint word = 0;
    switch (sp.format)
    {
    case kR8:
        for (uint j = 0; j < 4; ++j)
            if (k * 4 + j < texels)
                word |= encodeByte(k * 4 + j, 0) << (8 * j);
        break;
    case kRG8:
        for (uint j = 0; j < 2; ++j)
            if (k * 2 + j < texels)
                word |= (encodeByte(k * 2 + j, 0) | (encodeByte(k * 2 + j, 1) << 8)) << (16 * j);
        break;
    case kRGBA8:
        for (uint c = 0; c < 4; ++c)
            word |= encodeByte(k, c) << (8 * c);
        break;
    case kR16F:
        for (uint j = 0; j < 2; ++j)
            if (k * 2 + j < texels)
                word |= encodeHalf(k * 2 + j, 0) << (16 * j);
        break;
    case kRGBA16F:
        word = encodeHalf(k / 2, (k % 2) * 2) | (encodeHalf(k / 2, (k % 2) * 2 + 1) << 16);
        break;
    default:
    {
        uint2 p = texelXY(k);
        word = asuint(channelValue(0, p.x, p.y));
        break;
    }
    }
    sp.dst[k] = word;
}

