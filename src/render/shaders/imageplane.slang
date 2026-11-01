// Reference images on world-space quads, shown in display space: no lighting, exposure, gamma or tonemapping. They draw
// over the composed frame, depth tested against the scene.

struct ImagePlanePush
{
    float4x4 viewProj;
    float4 corners[4]; // xyz: top left, top right, bottom right, bottom left
    float4 uv;         // u0, v0, u1, v1
    float4 params;     // opacity, brightness, contrast, grayscale
    float4 texel;      // xy: half a texel, to keep bilinear taps off the far edge of the repeating sampler; w: 1 behind the scene
    float4 outline;    // rgb: line color, display-encoded; w: 1 draws only the image's edges
};

[[vk::push_constant]] ImagePlanePush pc;
[[vk::binding(0, 0)]] Sampler2D image; // sRGB view, so samples arrive linear

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
VSOut imagePlaneVS(uint id : SV_VertexID)
{
    const uint order[6] = {0, 1, 2, 0, 2, 3};
    uint c = order[id];
    float2 t = float2(c == 1 || c == 2 ? 1.0 : 0.0, c >= 2 ? 1.0 : 0.0);
    VSOut o;
    o.pos = mul(pc.viewProj, float4(pc.corners[c].xyz, 1.0));
    // Behind the scene: at the far plane, so only what nothing covers takes it (reversed-Z clears to 0).
    if (pc.texel.w > 0.5)
        o.pos.z = 0.0;
    o.uv = lerp(pc.uv.xy, pc.uv.zw, t);
    return o;
}

float3 toDisplay(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055);
}

float3 toLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

float luma(float2 uv)
{
    float4 c = image.Sample(clamp(uv, pc.texel.xy, 1.0 - pc.texel.xy));
    return dot(toDisplay(c.rgb), float3(0.2126, 0.7152, 0.0722)) * c.a;
}

/** Sobel edge strength of the image's display luminance around uv, steps of about 1.5 texels. */
float edge(float2 uv)
{
    float2 s = pc.texel.xy * 3.0;
    float tl = luma(uv + float2(-s.x, -s.y)), t = luma(uv + float2(0, -s.y)), tr = luma(uv + float2(s.x, -s.y));
    float l = luma(uv + float2(-s.x, 0)), r = luma(uv + float2(s.x, 0));
    float bl = luma(uv + float2(-s.x, s.y)), b = luma(uv + float2(0, s.y)), br = luma(uv + float2(s.x, s.y));
    float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
    float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);
    return sqrt(gx * gx + gy * gy);
}

[shader("fragment")]
float4 imagePlanePS(VSOut i) : SV_Target
{
    if (pc.outline.w > 0.5)
    {
        // Contrast steepens the line threshold; brightness lowers it.
        float e = saturate((edge(i.uv) - 0.08 - pc.params.y * 0.5) * 4.0 * pc.params.z);
        float ea = e * pc.params.x;
        if (ea < 0.002)
            discard;
        return float4(toLinear(pc.outline.rgb), ea);
    }
    float4 c = image.Sample(clamp(i.uv, pc.texel.xy, 1.0 - pc.texel.xy));
    float a = c.a * pc.params.x;
    // Clear texels write no depth, so geometry behind a cut-out reference stays visible.
    if (a < 0.002)
        discard;
    float3 d = toDisplay(c.rgb);
    if (pc.params.w > 0.5)
        d = dot(d, float3(0.2126, 0.7152, 0.0722));
    d = saturate((d - 0.5) * pc.params.z + 0.5 + pc.params.y);
    return float4(toLinear(d), a);
}
