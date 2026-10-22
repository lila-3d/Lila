struct LinesPush
{
    float4x4 viewProj;
};

[[vk::push_constant]] LinesPush pc;

struct VSIn
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
VSOut linesVS(VSIn v)
{
    VSOut o;
    o.pos = mul(pc.viewProj, float4(v.pos, 1.0));
    o.color = v.color;
    return o;
}

[shader("fragment")]
float4 linesPS(VSOut i) : SV_Target
{
    return i.color;
}
