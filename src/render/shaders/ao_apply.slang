// Fast ambient occlusion: darkens the finished frame by the AO computed from the main pass's own depth.

struct ApplyPush
{
    float4 params; // x: exponent on the AO before it multiplies the display-encoded colour
};

[[vk::push_constant]] ApplyPush ap;

[[vk::binding(0, 0)]] Sampler2D aoTex;

struct ApplyOut
{
    float4 pos : SV_Position;
};

[shader("vertex")]
ApplyOut applyVS(uint vid: SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    ApplyOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 applyPS(ApplyOut i) : SV_Target
{
    float ao = aoTex.Load(int3(int2(i.pos.xy), 0)).r;
    return float4(pow(saturate(ao), ap.params.x).xxx, 1.0);
}
