#include "tex_formats.slang"

// Virtual texture pages from a texture graph (render/TexEval.h recordPages): a page's layer composed from plan
// outputs, packed in the layer's format for a buffer-to-image copy into its slot.

static const uint kHalfSource = 0x100;
static const uint kWholeSource = 0x200;

struct ComposePush
{
    uint4 srcSize; // per channel: width | height << 16
    uint4 srcMode; // per channel: component | kHalfSource | kWholeSource (a whole buffer sampled at the page's uv, else the page's own region)
    float4 constants;
    uint* dst;
    // Per channel, its buffer: float4 texels as two pairs, half4 as one; null for a constant.
    uint2* src0;
    uint2* src1;
    uint2* src2;
    uint2* src3;
    uint width; // the page region
    uint height;
    uint format;
    uint srgb;
    uint groupsX;
    uint words;
    float x0; // the region's first texel, and the level's size, for whole sources
    float y0;
    float levelW;
    float levelH;
};

[[vk::push_constant]] ComposePush cp;

uint2* source(uint c)
{
    switch (c)
    {
    case 0: return cp.src0;
    case 1: return cp.src1;
    case 2: return cp.src2;
    default: return cp.src3;
    }
}

float component(uint c, uint i)
{
    uint mode = cp.srcMode[c];
    uint k = mode & 3;
    if ((mode & kHalfSource) != 0)
    {
        uint2 q = source(c)[i];
        return f16tof32((k < 2 ? q.x : q.y) >> ((k & 1) * 16));
    }
    uint2 q = source(c)[2 * i + k / 2];
    return asfloat((k & 1) != 0 ? q.y : q.x);
}

float wholeAt(uint c, int2 p, int w, int h)
{
    int x = ((p.x % w) + w) % w;
    int y = ((p.y % h) + h) % h;
    return component(c, uint(y * w + x));
}

float channelValue(uint c, uint x, uint y)
{
    if (source(c) == nullptr)
        return cp.constants[c];
    if ((cp.srcMode[c] & kWholeSource) == 0)
        return component(c, y * cp.width + x);
    int w = int(cp.srcSize[c] & 0xffff), h = int(cp.srcSize[c] >> 16);
    float2 p = (float2(cp.x0 + float(x), cp.y0 + float(y)) + 0.5) * float2(float(w) / cp.levelW, float(h) / cp.levelH) - 0.5;
    float2 f = floor(p);
    float2 t = p - f;
    int2 i = int2(f);
    float a = wholeAt(c, i, w, h), b = wholeAt(c, i + int2(1, 0), w, h), d = wholeAt(c, i + int2(0, 1), w, h), e = wholeAt(c, i + int2(1, 1), w, h);
    return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y);
}

uint encodeByte(uint texel, uint c)
{
    float v = channelValue(c, texel % cp.width, texel / cp.width);
    return unorm8(cp.srgb != 0 && cp.format == kRGBA8 && c < 3 ? toSrgb(v) : v);
}

uint encodeHalf(uint texel, uint c)
{
    return f32tof16(channelValue(c, texel % cp.width, texel / cp.width));
}

/** One 32-bit word of the page's tightly packed texels per thread. */
[shader("compute")]
[numthreads(64, 1, 1)]
void texComposeCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    uint k = flatIndex(gid, local, cp.groupsX);
    if (k >= cp.words)
        return;
    uint texels = cp.width * cp.height;
    uint word = 0;
    switch (cp.format)
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
        word = asuint(channelValue(0, k % cp.width, k / cp.width));
        break;
    }
    cp.dst[k] = word;
}
