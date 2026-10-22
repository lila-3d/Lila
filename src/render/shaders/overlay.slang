struct OverlayPush
{
    float2 invSize; // 1 / viewport size in pixels
};

[[vk::push_constant]] OverlayPush pc;
[[vk::binding(0, 0)]] Sampler2D atlas;

struct VSIn
{
    float2 pos : POSITION; // pixels, origin top left
    float2 uv : TEXCOORD;  // u < 0 means untextured
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

[shader("vertex")]
VSOut overlayVS(VSIn v)
{
    VSOut o;
    o.pos = float4(v.pos * pc.invSize * 2.0 - 1.0, 0.0, 1.0);
    o.uv = v.uv;
    o.color = v.color;
    return o;
}

[shader("fragment")]
float4 overlayPS(VSOut i) : SV_Target
{
    float4 c = i.color;
    if (i.uv.x >= 0.0)
        c.a *= atlas.SampleLevel(i.uv, 0).r;
    return c;
}
