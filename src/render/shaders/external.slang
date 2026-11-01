// An external viewport renderer's image, drawn in place of Lila's scene: colour blended over the background, and its
// depth written where it hit something so depth-tested wires, grid and overlays hide behind its surfaces. Drawn in display
// space over the composed frame, so its colours come through exactly.

struct ExternalPush
{
    float4 size;   // image extent, then its inverse
    float4 projZ;  // proj[2][2], proj[3][2], proj[2][3], proj[3][3]: view-space z to clip z and w
    float4 params; // x: 1 when there is depth, y: 1 for rows bottom up, z: 1 for premultiplied colour, w: colour multiplier
    float4 unproject; // with params.x 2: OpenGL window depth to view z, (x * ndc + y) / (z + w * (1 - ndc))
};

[[vk::push_constant]] ExternalPush pc;
[[vk::binding(0, 0)]] Sampler2D colorTex; // sRGB view for 8-bit images, so samples arrive linear
[[vk::binding(1, 0)]] Sampler2D depthTex; // distance along the view axis, cm; 0 or less where nothing was hit

struct ExternalVSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
ExternalVSOut externalVS(uint vid: SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    ExternalVSOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}

struct ExternalPSOut
{
    float4 color : SV_Target;
    float depth : SV_Depth;
};

[shader("fragment")]
ExternalPSOut externalPS(ExternalVSOut i)
{
    ExternalPSOut o;
    float2 uv = pc.params.y > 0.5 ? float2(i.uv.x, 1.0 - i.uv.y) : i.uv;
    o.color = colorTex.Sample(clamp(uv, 0.5 * pc.size.zw, 1.0 - 0.5 * pc.size.zw));
    if (pc.params.z > 0.5 && o.color.a > 0.0 && o.color.a < 1.0)
        o.color.rgb /= o.color.a;
    o.color.rgb *= pc.params.w;
    // Reversed-Z: 0 is the far plane the target was cleared to, so a miss leaves the depth as it was.
    o.depth = 0.0;
    if (pc.params.x > 0.5)
    {
        int2 p = clamp(int2(uv * pc.size.xy), int2(0), int2(pc.size.xy) - 1);
        float d = depthTex.Load(int3(p, 0)).r;
        if (pc.params.x > 1.5)
        {
            // 1 - ndc taken from 1 - d keeps the precision near the far plane that ndc itself loses.
            float e = 2.0 * (1.0 - d);
            d = d < 1.0 && o.color.a > 0.0 ? -(pc.unproject.x * (1.0 - e) + pc.unproject.y) / (pc.unproject.z + pc.unproject.w * e) : 0.0;
        }
        if (d > 0.0 && d < 3.0e38)
        {
            float z = -d;
            o.depth = saturate((pc.projZ.x * z + pc.projZ.y) / (pc.projZ.z * z + pc.projZ.w));
        }
    }
    if (o.color.a < 0.002 && o.depth <= 0.0)
        discard;
    return o;
}
