// Scene target to display: expands the scene (or its temporal result), applies exposure, bloom and the post stack, and
// tonemaps into the frame before lines and overlays draw over it.

#include "scene_color.slang"

struct ComposePush
{
    float4 params; // x: display gamma, y: 1 to dither, z: exposure, w: tonemapper (0 neutral, 1 AgX, 2 ACES, 3 linear)
    float4 grade;  // x: contrast, y: saturation, z: 1 for the post stack, w: AgX look (0 none, 1 punchy, 2 golden)
    float4 white;  // rgb: white balance gains, w: LUT intensity (0 skips it)
    float4 lift;   // rgb, w: vignette
    float4 gammaC; // rgb, w: film grain
    float4 gain;   // rgb, w: grain seed
    float4 lens;   // x: chromatic aberration (pixels at the corner), y: lens distortion, z: sharpen, w: bloom intensity
    float4 size;   // extent, then its inverse
    float4 misc;   // x: 1 to apply the auto exposure, y: bloom scale (one over its levels)
};

[[vk::push_constant]] ComposePush cp;
[[vk::binding(0, 0)]] Sampler2D composeTex; // compressed scene, linear filtering
[[vk::binding(1, 0)]] Sampler2D bloomTex;   // half resolution, expanded radiance
[[vk::binding(2, 0)]] Sampler3D lutTex;     // display-encoded in and out
[[vk::binding(3, 0)]] Sampler2D exposureTex; // exposure.slang's state: texel 0 is the multiplier

struct ComposeOut
{
    float4 pos : SV_Position;
};

[shader("vertex")]
ComposeOut composeVS(uint vid: SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    ComposeOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float3 toSrgb(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055);
}

float3 fromSrgb(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

// Half an 8-bit step of noise, applied in sRGB space since that is where the swapchain quantizes.
float3 dither(float3 linear, float2 pixel)
{
    float noise = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))) - 0.5;
    float3 srgb = saturate(toSrgb(linear) + noise / 255.0);
    return fromSrgb(srgb);
}

// AgX (Sobotka), the analytic fit by Benjamin Wrensch: Rec.709 in, display-linear Rec.709 out.
float3 agxContrast(float3 x)
{
    float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 agx(float3 c, uint look)
{
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992, 0.878468636469772,
                                    0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368, 1.15190312990417,
                                     -0.0980434501171241, -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    float3 v = mul(max(c, 1e-10), inset);
    v = saturate((log2(v) - minEv) / (maxEv - minEv));
    v = agxContrast(v);
    if (look != 0)
    {
        float3 slope = 1.0, power = 1.0;
        float sat = 1.0;
        if (look == 1)
        {
            power = 1.35;
            sat = 1.4;
        }
        else
        {
            slope = float3(1.0, 0.9, 0.5);
            power = 0.8;
            sat = 0.8;
        }
        float luma = dot(v, float3(0.2126, 0.7152, 0.0722));
        v = pow(max(v * slope, 0.0), power);
        v = luma + sat * (v - luma);
    }
    v = mul(v, outset);
    return pow(max(v, 0.0), 2.2);
}

// ACES filmic fit (Stephen Hill): sRGB to ACES AP1 through the RRT and ODT fit, back to display-linear sRGB.
float3 aces(float3 c)
{
    const float3x3 inputM = float3x3(0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777);
    const float3x3 outputM = float3x3(1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276, 1.07602);
    float3 v = mul(inputM, c);
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return saturate(mul(outputM, a / b));
}

float3 tonemap(float3 c, uint mode, uint look)
{
    if (mode == 1)
        return agx(c, look);
    if (mode == 2)
        return aces(c);
    if (mode == 3)
        return saturate(c);
    return neutralTonemap(c);
}

float3 sceneAt(float2 uv)
{
    return sceneExpand(composeTex.SampleLevel(uv, 0).rgb);
}

float hash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

[shader("fragment")]
float4 composePS(ComposeOut i) : SV_Target
{
    const bool stack = cp.grade.z > 0.5;
    float2 uv = i.pos.xy * cp.size.zw;
    const float alpha = composeTex.Load(int3(int2(i.pos.xy), 0)).a;
    float3 c;
    if (!stack)
        c = sceneExpand(composeTex.Load(int3(int2(i.pos.xy), 0)).rgb);
    else
    {
        // Barrel or pincushion distortion around the centre, then a radial colour split.
        float2 centred = uv * 2.0 - 1.0;
        float aspect = cp.size.x * cp.size.w;
        float2 d = centred * float2(aspect, 1.0);
        float r2 = dot(d, d) / (aspect * aspect + 1.0);
        float2 warped = centred * (1.0 + cp.lens.y * r2);
        float2 base = warped * 0.5 + 0.5;
        if (cp.lens.x > 0.0)
        {
            float2 shift = warped * cp.lens.x * cp.size.zw;
            c = float3(sceneAt(base + shift).r, sceneAt(base).g, sceneAt(base - shift).b);
        }
        else
            c = sceneAt(base);
        if (cp.lens.z > 0.0)
        {
            // Unsharp mask on the four neighbours.
            float3 n = sceneAt(base + float2(cp.size.z, 0)) + sceneAt(base - float2(cp.size.z, 0)) + sceneAt(base + float2(0, cp.size.w)) +
                       sceneAt(base - float2(0, cp.size.w));
            c = max(c + cp.lens.z * (c - 0.25 * n), 0.0);
        }
        if (cp.lens.w > 0.0)
            c = lerp(c, bloomTex.SampleLevel(base, 0).rgb * cp.misc.y, cp.lens.w);
        uv = base;
    }

    float exposure = cp.params.z;
    if (cp.misc.x > 0.5)
        exposure *= exposureTex.Load(int3(0, 0, 0)).r;
    c *= exposure;

    if (stack)
    {
        c *= cp.white.rgb;
        // Contrast around 18% grey in log space, then saturation around the pixel's luminance.
        const float grey = 0.18;
        c = grey * pow(max(c / grey, 0.0), cp.grade.x);
        float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = max(luma + cp.grade.y * (c - luma), 0.0);
    }

    c = tonemap(c, uint(cp.params.w + 0.5), uint(cp.grade.w + 0.5));
    c = pow(max(c, 0.0), 1.0 / max(cp.params.x, 0.01));

    if (stack)
    {
        // Lift, gamma, gain on the display-encoded picture, as in a grading suite.
        float3 s = toSrgb(saturate(c));
        s = saturate(s * cp.gain.rgb + cp.lift.rgb * (1.0 - s));
        s = pow(s, 1.0 / max(cp.gammaC.rgb, 0.01));
        if (cp.white.w > 0.0)
        {
            float3 dims;
            lutTex.GetDimensions(dims.x, dims.y, dims.z);
            float3 coord = s * ((dims - 1.0) / dims) + 0.5 / dims;
            s = lerp(s, lutTex.SampleLevel(coord, 0).rgb, cp.white.w);
        }
        if (cp.lift.w > 0.0)
        {
            float2 v = (uv - 0.5) * float2(cp.size.x * cp.size.w, 1.0);
            s *= 1.0 - cp.lift.w * smoothstep(0.25, 1.0, dot(v, v) * 1.6);
        }
        if (cp.gammaC.w > 0.0)
        {
            // Grain strongest in the mid-tones, as on film.
            float n = hash(i.pos.xy + cp.gain.w * 97.0) + hash(i.pos.xy * 1.37 + cp.gain.w * 61.0) - 1.0;
            float luma = dot(s, float3(0.2126, 0.7152, 0.0722));
            s = saturate(s + n * cp.gammaC.w * 0.12 * (1.0 - abs(luma * 2.0 - 1.0) * 0.6));
        }
        c = fromSrgb(s);
    }
    if (cp.params.y > 0.5)
        c = dither(c, i.pos.xy);
    return float4(c, alpha);
}
