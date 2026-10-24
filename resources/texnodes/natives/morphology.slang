// Dilate and erode: separable square maximum or minimum, radius a fraction of the size, wrapping. Contract in
// src/tex/Natives.h. gU: 0 input, 1 output, 2 radius, 3 scratch.

int morphRadius(float size) { return max(0, int(floor(gU[2].x * size + 0.5))); }

float4 morphRows(int2 p, bool isMax)
{
    int2 s = nativeTexel();
    int r = morphRadius(gPass.res.x);
    float4 acc = nativeFitted(0, p);
    for (int k = -r; k <= r; ++k)
    {
        float4 v = nativeFitted(0, int2(((p.x + k) % s.x + s.x) % s.x, p.y));
        acc = isMax ? max(acc, v) : min(acc, v);
    }
    return acc;
}

float4 morphColumns(int2 p, bool isMax)
{
    uint src = uint(gPass.uv.x);
    int r = morphRadius(gPass.res.y);
    float4 acc = texFetchBuf(src, p);
    for (int k = -r; k <= r; ++k)
    {
        float4 v = texFetchBuf(src, int2(p.x, p.y + k));
        acc = isMax ? max(acc, v) : min(acc, v);
    }
    return acc;
}

bool morphInside(int2 p) { return p.x < int(gPass.res.x) && p.y < int(gPass.res.y); }

void dilateRows(uint3 id)
{
    if (morphInside(int2(id.xy)))
        nativeStore(int2(id.xy), morphRows(int2(id.xy), true));
}

/** gPass.uv.x: the rows result's slot. */
void dilateColumns(uint3 id)
{
    if (morphInside(int2(id.xy)))
        nativeStore(int2(id.xy), morphColumns(int2(id.xy), true));
}

void erodeRows(uint3 id)
{
    if (morphInside(int2(id.xy)))
        nativeStore(int2(id.xy), morphRows(int2(id.xy), false));
}

/** gPass.uv.x: the rows result's slot. */
void erodeColumns(uint3 id)
{
    if (morphInside(int2(id.xy)))
        nativeStore(int2(id.xy), morphColumns(int2(id.xy), false));
}
