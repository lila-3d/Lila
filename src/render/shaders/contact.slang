// Soft contact shadow: one blob per mesh on the ground plane, sized from its bounds.

struct ContactPush
{
    float4x4 viewProj;
    float4 center; // xyz: footprint center on the ground, w: opacity
    float4 axisA;  // xyz: half extent along the first ground axis, quad included. w: footprint edge in quad units
    float4 axisB;
};

[[vk::push_constant]] ContactPush cpc;

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv;
};

[shader("vertex")]
VSOut contactVS(uint vid: SV_VertexID)
{
    const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
    float2 uv = corners[vid];
    float3 p = cpc.center.xyz + cpc.axisA.xyz * uv.x + cpc.axisB.xyz * uv.y;
    VSOut o;
    o.pos = mul(cpc.viewProj, float4(p, 1.0));
    o.uv = uv;
    return o;
}

[shader("fragment")]
float4 contactPS(VSOut i) : SV_Target
{
    // Rounded square: square-ish for boxes, never a hard edge.
    float2 a = abs(i.uv);
    float r = pow(pow(a.x, 4.0) + pow(a.y, 4.0), 0.25);
    float falloff = 1.0 - smoothstep(cpc.axisA.w * 0.5, 1.0, r);
    return float4(0.0, 0.0, 0.0, cpc.center.w * pow(falloff, 1.5));
}
