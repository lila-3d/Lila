// The scene target's encoding. Scene shaders write linear exposed radiance compressed as c / (1 + max(c)) (Karis), so
// MSAA resolves, blending and temporal antialiasing average in a bounded space; the compose pass undoes it, then
// tonemaps. Colours given for display (grid, wires, image planes, highlights) go through fromDisplay so the default
// tonemap shows them exactly as given.

float3 sceneCompress(float3 c)
{
    c = max(c, 0.0);
    return c / (1.0 + max(c.r, max(c.g, c.b)));
}

float3 sceneExpand(float3 c)
{
    return c / max(1.0 - max(c.r, max(c.g, c.b)), 1.0 / 65504.0);
}

// Khronos PBR Neutral: keeps hue and saturation of base colors intact up to the shoulder.
float3 neutralTonemap(float3 c)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression)
        return c;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(c, newPeak.xxx, g);
}

/**
 * neutralTonemap undone, highlight desaturation included. Peaks cap at 0.9: brighter ones need radiance so high that
 * the tonemap washes them to white and their antialiased edges bleed out wide.
 */
float3 neutralInverse(float3 t)
{
    t = clamp(t, 0.0, 1.0);
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float maxPeak = 0.9;
    const float d = 1.0 - startCompression;
    float newPeak = max(t.r, max(t.g, t.b));
    if (newPeak > maxPeak)
    {
        t *= maxPeak / newPeak;
        newPeak = maxPeak;
    }
    if (newPeak >= startCompression)
    {
        float peak = d * d / (1.0 - newPeak) - d + startCompression;
        float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
        t = max((t - g * newPeak) / (1.0 - g), 0.0);
        t *= peak / newPeak;
    }
    // The toe took x - 6.25 x^2 off every channel, x being the smallest, which left that one at 6.25 x^2.
    float y = min(t.r, min(t.g, t.b));
    float offset = 0.04;
    if (y < 0.04)
    {
        float x = sqrt(max(y, 0.0) / 6.25);
        offset = x - 6.25 * x * x;
    }
    return t + offset;
}

/** A display-linear colour (what the default tonemap and gamma show) as the scene target stores it. */
float3 fromDisplay(float3 c, float gamma)
{
    c = pow(max(c, 0.0), max(gamma, 0.01));
    return sceneCompress(neutralInverse(c));
}
