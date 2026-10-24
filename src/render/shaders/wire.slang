// Wireframe over shaded meshes: same positions as the mesh, flat color.

struct WirePush
{
    float4x4 viewProj;
    float4x4 world;
    float4 color;
    /** Clip z = z * x + w * y: pulls the wire toward the camera along its view ray (see depthPull in Renderer.cpp). */
    float4 depthPull;
};

[[vk::push_constant]] WirePush wpc;

struct WireIn
{
    float3 pos : POSITION;
};

[shader("vertex")]
float4 wireVS(WireIn v) : SV_Position
{
    float4 p = mul(wpc.viewProj, mul(wpc.world, float4(v.pos, 1.0)));
    p.z = p.z * wpc.depthPull.x + p.w * wpc.depthPull.y;
    return p;
}

[shader("fragment")]
float4 wirePS() : SV_Target
{
    return wpc.color;
}
