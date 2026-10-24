struct BlurPush
{
    float4* src;
    float4* dst;
    uint srcWidth;
    uint srcHeight;
    uint width;
    uint height;
    float sigma;
    int radius;
    uint vertical;
    uint pad0;
};

[[vk::push_constant]] BlurPush bp;

/**
 * One direction of the separable Gaussian, wrapping. The horizontal pass reads its input resampled to the output grid
 * (nearest), as the CPU evaluator does; the vertical one reads the horizontal result.
 */
[shader("compute")]
[numthreads(8, 8, 1)]
void texBlurCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= bp.width || id.y >= bp.height)
        return;
    int w = int(bp.width), h = int(bp.height);
    float sum = 0.0;
    for (int k = -bp.radius; k <= bp.radius; ++k)
        sum += exp(-0.5 * float(k * k) / (bp.sigma * bp.sigma));
    float4 acc = float4(0.0);
    for (int k = -bp.radius; k <= bp.radius; ++k)
    {
        float wk = exp(-0.5 * float(k * k) / (bp.sigma * bp.sigma)) / sum;
        float4 s;
        if (bp.vertical != 0)
        {
            int y = ((int(id.y) + k) % h + h) % h;
            s = bp.src[uint(y) * bp.width + id.x];
        }
        else
        {
            int x = ((int(id.x) + k) % w + w) % w;
            uint sx = uint(x) * bp.srcWidth / bp.width, sy = id.y * bp.srcHeight / bp.height;
            s = bp.src[sy * bp.srcWidth + sx];
        }
        acc += wk * s;
    }
    bp.dst[id.y * bp.width + id.x] = acc;
}
