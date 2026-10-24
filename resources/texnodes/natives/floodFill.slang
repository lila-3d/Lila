// Flood fill: each texel gets its 4-connected region's bounding box relative to itself (helpers/fill.slang reads it).
// Alternating row and column sweeps grow the boxes: each sweep unions the boxes along a run of region texels.
// Contract in src/tex/Natives.h. gU: 0 mask, 1 output, 2 passes, 3 and 4 scratch.

bool fillValid(float4 e) { return e.z >= e.x && e.w >= e.y; }

void fillInit(uint3 id)
{
    int2 p = int2(id.xy);
    if (p.x >= int(gPass.res.x) || p.y >= int(gPass.res.y))
        return;
    nativeStore(p, nativeFitted(0, p).x > 0.5 ? float4(0.0, 0.0, 0.0, 0.0) : float4(0.0, 0.0, -1.0, -1.0));
}

/** One thread per row (threads 1 x h). gPass.uv.x: the source slot. */
void fillRows(uint3 id)
{
    int w = int(gPass.res.x);
    int h = int(gPass.res.y);
    int y = int(id.y);
    if (id.x != 0 || y >= h)
        return;
    uint src = uint(gPass.uv.x);
    int start = -1;
    for (int x = 0; x < w; ++x)
    {
        if (!fillValid(texFetchBuf(src, int2(x, y))))
        {
            start = x;
            break;
        }
    }
    if (start < 0)
    {
        // The whole row is one region: it wraps, so it spans the row.
        float lo = 1e9;
        float hi = -1e9;
        for (int x = 0; x < w; ++x)
        {
            float4 e = texFetchBuf(src, int2(x, y));
            lo = min(lo, e.y);
            hi = max(hi, e.w);
        }
        for (int x = 0; x < w; ++x)
            nativeStore(int2(x, y), float4(-float(x), lo, float(w - 1 - x), hi));
        return;
    }
    int j = start + 1;
    int end = start + w;
    while (j <= end)
    {
        float4 e = texFetchBuf(src, int2(j % w, y));
        if (!fillValid(e))
        {
            nativeStore(int2(j % w, y), e);
            ++j;
            continue;
        }
        int first = j;
        float4 box = float4(1e9, 1e9, -1e9, -1e9);
        while (j <= end)
        {
            float4 f = texFetchBuf(src, int2(j % w, y));
            if (!fillValid(f))
                break;
            box = float4(min(box.x, f.x + float(j)), min(box.y, f.y), max(box.z, f.z + float(j)), max(box.w, f.w));
            ++j;
        }
        for (int k = first; k < j; ++k)
            nativeStore(int2(k % w, y), float4(box.x - float(k), box.y, box.z - float(k), box.w));
    }
}

/** One thread per column (threads w x 1). gPass.uv.x: the source slot. */
void fillColumns(uint3 id)
{
    int w = int(gPass.res.x);
    int h = int(gPass.res.y);
    int x = int(id.x);
    if (id.y != 0 || x >= w)
        return;
    uint src = uint(gPass.uv.x);
    int start = -1;
    for (int y = 0; y < h; ++y)
    {
        if (!fillValid(texFetchBuf(src, int2(x, y))))
        {
            start = y;
            break;
        }
    }
    if (start < 0)
    {
        float lo = 1e9;
        float hi = -1e9;
        for (int y = 0; y < h; ++y)
        {
            float4 e = texFetchBuf(src, int2(x, y));
            lo = min(lo, e.x);
            hi = max(hi, e.z);
        }
        for (int y = 0; y < h; ++y)
            nativeStore(int2(x, y), float4(lo, -float(y), hi, float(h - 1 - y)));
        return;
    }
    int j = start + 1;
    int end = start + h;
    while (j <= end)
    {
        float4 e = texFetchBuf(src, int2(x, j % h));
        if (!fillValid(e))
        {
            nativeStore(int2(x, j % h), e);
            ++j;
            continue;
        }
        int first = j;
        float4 box = float4(1e9, 1e9, -1e9, -1e9);
        while (j <= end)
        {
            float4 f = texFetchBuf(src, int2(x, j % h));
            if (!fillValid(f))
                break;
            box = float4(min(box.x, f.x), min(box.y, f.y + float(j)), max(box.z, f.z), max(box.w, f.w + float(j)));
            ++j;
        }
        for (int k = first; k < j; ++k)
            nativeStore(int2(x, k % h), float4(box.x, box.y - float(k), box.z, box.w - float(k)));
    }
}
