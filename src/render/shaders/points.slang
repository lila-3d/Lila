// Component dots: one point per vertex, a square of constant pixel size, coloured and sized by its state.

struct PointsPush
{
    float4x4 viewProj;
    /** Clip z = z * x + w * y: pulls the dot toward the camera along its view ray (see depthPull in Renderer.cpp). */
    float4 depthPull;
    float4 colors[3]; // linear: normal, selected, mirror
    float4 sizes;     // device pixels: x normal, y selected
};

[[vk::push_constant]] PointsPush pc;

struct VSIn
{
    float3 pos : POSITION;
    uint state : STATE;
};

struct VSOut
{
    float4 pos : SV_Position;
    nointerpolation float4 color : COLOR;
    [[vk::builtin("PointSize")]] float size : PSIZE;
};

[shader("vertex")]
VSOut pointsVS(VSIn v)
{
    VSOut o;
    o.pos = mul(pc.viewProj, float4(v.pos, 1.0));
    o.pos.z = o.pos.z * pc.depthPull.x + o.pos.w * pc.depthPull.y;
    o.color = pc.colors[min(v.state, 2u)];
    o.size = v.state == 1 ? pc.sizes.y : pc.sizes.x;
    // Hidden points land outside the clip volume.
    if (v.state > 2)
        o.pos = float4(2.0, 2.0, 2.0, 1.0);
    return o;
}

[shader("fragment")]
float4 pointsPS(VSOut i) : SV_Target
{
    return i.color;
}
