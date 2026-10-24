// Blur: separable Gaussian, radius a fraction of the width, wrapping. Contract in src/tex/Natives.h.
// gU: 0 input, 1 output, 2 radius, 3 scratch.

float blurSigma() { return max(1e-4, gU[2].x * gPass.res.x / 3.0); }

int blurReach(float sigma) { return min(int(max(gPass.res.x, gPass.res.y)), int(ceil(sigma * 3.0))); }

/** Input -> scratch, along rows; the input is read on the output's grid. */
void blurRows(uint3 id)
{
    int2 s = nativeTexel();
    int2 p = int2(id.xy);
    if (p.x >= s.x || p.y >= s.y)
        return;
    float sigma = blurSigma();
    int r = blurReach(sigma);
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float sum = 0.0;
    for (int k = -r; k <= r; ++k)
    {
        float wgt = exp(-0.5 * float(k * k) / (sigma * sigma));
        acc += wgt * nativeFitted(0, int2(((p.x + k) % s.x + s.x) % s.x, p.y));
        sum += wgt;
    }
    nativeStore(p, acc / sum);
}

/** gPass.uv.x: the rows result's slot. */
void blurColumns(uint3 id)
{
    int2 s = nativeTexel();
    int2 p = int2(id.xy);
    if (p.x >= s.x || p.y >= s.y)
        return;
    uint src = uint(gPass.uv.x);
    float sigma = blurSigma();
    int r = blurReach(sigma);
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float sum = 0.0;
    for (int k = -r; k <= r; ++k)
    {
        float wgt = exp(-0.5 * float(k * k) / (sigma * sigma));
        acc += wgt * texFetchBuf(src, int2(p.x, p.y + k));
        sum += wgt;
    }
    nativeStore(p, acc / sum);
}
