//! Photoshop-style blend modes on straight colours, by mode index.

float3 blendOverlay(float3 b, float3 f)
{
    return lerp(2.0 * b * f, 1.0 - 2.0 * (1.0 - b) * (1.0 - f), step(0.5, b));
}

float3 blendSoftLight(float3 b, float3 f)
{
    float3 dark = b - (1.0 - 2.0 * f) * b * (1.0 - b);
    float3 g = lerp(((16.0 * b - 12.0) * b + 4.0) * b, sqrt(b), step(0.25, b));
    float3 light = b + (2.0 * f - 1.0) * (g - b);
    return lerp(dark, light, step(0.5, f));
}

/** 0 normal, 1 add, 2 subtract, 3 multiply, 4 screen, 5 overlay, 6 darken, 7 lighten, 8 difference, 9 soft light. */
float3 blendMode(int mode, float3 b, float3 f)
{
    switch (mode)
    {
    case 1: return b + f;
    case 2: return b - f;
    case 3: return b * f;
    case 4: return 1.0 - (1.0 - b) * (1.0 - f);
    case 5: return blendOverlay(b, f);
    case 6: return min(b, f);
    case 7: return max(b, f);
    case 8: return abs(b - f);
    case 9: return blendSoftLight(b, f);
    default: return f;
    }
}
