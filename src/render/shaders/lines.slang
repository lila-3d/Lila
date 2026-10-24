struct LinesPush
{
    float4x4 viewProj;
    /** Clip z = z * x + w * y: pulls the line toward the camera along its view ray (see depthPull in Renderer.cpp). */
    float4 depthPull;
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
    o.pos.z = o.pos.z * pc.depthPull.x + o.pos.w * pc.depthPull.y;
    o.color = v.color;
    return o;
}

[shader("fragment")]
float4 linesPS(VSOut i) : SV_Target
{
    return i.color;
}
