// Wireframe over shaded meshes: same positions as the mesh, flat color.

struct WirePush
{
    float4x4 viewProj;
    float4x4 world;
    float4 color;
};

[[vk::push_constant]] WirePush wpc;

struct WireIn
{
    float3 pos : POSITION;
};

[shader("vertex")]
float4 wireVS(WireIn v) : SV_Position
{
    return mul(wpc.viewProj, mul(wpc.world, float4(v.pos, 1.0)));
}

[shader("fragment")]
float4 wirePS() : SV_Target
{
    return wpc.color;
}
