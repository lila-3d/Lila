// Normal to height: Poisson integration of the normal's slopes on a pyramid, coarse to fine with Jacobi relaxation.
// Heights are zero mean; the output adds 0.5. Contract in src/tex/Natives.h.
// gU: 0 normal, 1 output, 2 intensity, 3 iterations, then per level: right-hand side and two heights.

bool n2hInside(int2 p) { return p.x < int(gPass.res.x) && p.y < int(gPass.res.y); }

/** dh per texel along x and along rows (down), from a normal as Height to Normal writes it. */
float2 n2hSlope(int2 p)
{
    float3 n = nativeFitted(0, p).xyz * 2.0 - 1.0;
    float k = 1.0 / (0.05 * max(gU[2].x, 1e-4));
    float nz = max(n.z, 0.05);
    return float2(-n.x / nz * k * gPass.res.z, n.y / nz * k * gPass.res.w);
}

int2 n2hWrap(int2 p)
{
    int2 s = nativeTexel();
    return ((p % s) + s) % s;
}

void divergence(uint3 id)
{
    int2 p = int2(id.xy);
    if (!n2hInside(p))
        return;
    float r = n2hSlope(n2hWrap(p + int2(1, 0))).x;
    float l = n2hSlope(n2hWrap(p - int2(1, 0))).x;
    float d = n2hSlope(n2hWrap(p + int2(0, 1))).y;
    float u = n2hSlope(n2hWrap(p - int2(0, 1))).y;
    float v = 0.5 * (r - l) + 0.5 * (d - u);
    nativeStore(p, float4(v, 0.0, 0.0, 0.0));
}

/** gPass.uv.x: the finer level's slot. Sums 2 x 2: the coarser grid's spacing doubles, so its Laplacian is 4x. */
void coarsen(uint3 id)
{
    int2 p = int2(id.xy);
    if (!n2hInside(p))
        return;
    uint src = uint(gPass.uv.x);
    int2 q = p * 2;
    float v = texFetchBuf(src, q).x + texFetchBuf(src, q + int2(1, 0)).x + texFetchBuf(src, q + int2(0, 1)).x + texFetchBuf(src, q + int2(1, 1)).x;
    nativeStore(p, float4(v, 0.0, 0.0, 0.0));
}

void zero(uint3 id)
{
    int2 p = int2(id.xy);
    if (n2hInside(p))
        nativeStore(p, float4(0.0, 0.0, 0.0, 0.0));
}

/** gPass.uv.x: the heights' slot, uv.y: the right-hand side's slot. */
void jacobi(uint3 id)
{
    int2 p = int2(id.xy);
    if (!n2hInside(p))
        return;
    uint hs = uint(gPass.uv.x);
    float sum = texFetchBuf(hs, p - int2(1, 0)).x + texFetchBuf(hs, p + int2(1, 0)).x + texFetchBuf(hs, p - int2(0, 1)).x + texFetchBuf(hs, p + int2(0, 1)).x;
    float v = (sum - texFetchBuf(uint(gPass.uv.y), p).x) * 0.25;
    nativeStore(p, float4(v, 0.0, 0.0, 0.0));
}

/** gPass.uv.x: the coarser heights' slot. Bilinear, wrapping. */
void upsample(uint3 id)
{
    int2 p = int2(id.xy);
    if (!n2hInside(p))
        return;
    uint src = uint(gPass.uv.x);
    float2 scale = float2(nativeSize(src)) / gPass.res.xy;
    float2 c = (float2(p) + 0.5) * scale - 0.5;
    float2 f = floor(c);
    float2 t = c - f;
    int2 i = int2(f);
    float top = lerp(texFetchBuf(src, i).x, texFetchBuf(src, i + int2(1, 0)).x, t.x);
    float bottom = lerp(texFetchBuf(src, i + int2(0, 1)).x, texFetchBuf(src, i + int2(1, 1)).x, t.x);
    nativeStore(p, float4(lerp(top, bottom, t.y), 0.0, 0.0, 0.0));
}

/** gPass.uv.x: the finest heights' slot. */
void heightOut(uint3 id)
{
    int2 p = int2(id.xy);
    if (!n2hInside(p))
        return;
    float v = 0.5 + texFetchBuf(uint(gPass.uv.x), p).x;
    nativeStore(p, float4(v, v, v, 1.0));
}
