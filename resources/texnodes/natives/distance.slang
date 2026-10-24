// Distance: a jump flood finds, per texel, the nearest texel inside the mask (xy) and outside it (zw), wrapping;
// -1 marks none found. Contract in src/tex/Natives.h. gU: 0 mask, 1 output, 2 maxDistance, 3 mode, 4 and 5 scratch.

float2 jumpDelta(float2 p, float2 q)
{
    float2 s = gPass.res.xy;
    float2 d = abs(p - q);
    d = min(d, s - d);
    return d * gPass.res.zw;
}

void jumpInit(uint3 id)
{
    int2 p = int2(id.xy);
    if (p.x >= int(gPass.res.x) || p.y >= int(gPass.res.y))
        return;
    float2 f = float2(p);
    bool inside = nativeFitted(0, p).x > 0.5;
    nativeStore(p, inside ? float4(f, -1.0, -1.0) : float4(-1.0, -1.0, f));
}

/** gPass.uv.x: the source slot, uv.y: the jump length in texels. */
void jumpStep(uint3 id)
{
    int2 p = int2(id.xy);
    int2 s = nativeTexel();
    if (p.x >= s.x || p.y >= s.y)
        return;
    uint src = uint(gPass.uv.x);
    int k = int(gPass.uv.y);
    float2 fp = float2(p);
    float4 best = float4(-1.0, -1.0, -1.0, -1.0);
    float bestIn = 1e30;
    float bestOut = 1e30;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            float4 c = texFetchBuf(src, p + int2(dx, dy) * k);
            if (c.x >= 0.0)
            {
                float2 d = jumpDelta(fp, c.xy);
                float dd = dot(d, d);
                if (dd < bestIn)
                {
                    bestIn = dd;
                    best.xy = c.xy;
                }
            }
            if (c.z >= 0.0)
            {
                float2 d = jumpDelta(fp, c.zw);
                float dd = dot(d, d);
                if (dd < bestOut)
                {
                    bestOut = dd;
                    best.zw = c.zw;
                }
            }
        }
    }
    nativeStore(p, best);
}

/** gPass.uv.x: the seeds' slot. Mode 0 falloff, 1 distance, 2 signed, as the CPU's distanceValue. */
void jumpResolve(uint3 id)
{
    int2 p = int2(id.xy);
    if (p.x >= int(gPass.res.x) || p.y >= int(gPass.res.y))
        return;
    float4 c = texFetchBuf(uint(gPass.uv.x), p);
    float2 fp = float2(p);
    bool inside = nativeFitted(0, p).x > 0.5;
    float dOut = c.x >= 0.0 ? min(length(jumpDelta(fp, c.xy)), 2.0) : 2.0;
    float dIn = c.z >= 0.0 ? min(length(jumpDelta(fp, c.zw)), 2.0) : 2.0;
    float halfTexel = 0.5 * min(gPass.res.z, gPass.res.w);
    int mode = int(gU[3].x);
    float v;
    if (mode == 1)
        v = dOut;
    else if (mode == 2)
        v = inside ? -(dIn - halfTexel) : dOut - halfTexel;
    else
        v = inside ? 1.0 : saturate(1.0 - dOut / max(gU[2].x, 1e-6));
    nativeStore(p, float4(v, v, v, 1.0));
}
