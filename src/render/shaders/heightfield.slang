// Heightfields drawn straight from their heights: the grid's vertices come from the vertex index, every step-th sample,
// so a terrain needs no mesh and only its heights on the GPU. Lit by a key light and sky in the scene target's encoding.

#include "scene_color.slang"

struct HeightfieldPush
{
    float4x4 viewProj;
    float4 model0; // world rows of the heightfield's placement
    float4 model1;
    float4 model2;
    float4 grid;   // x: samples along X, y: samples along Z, z: step, w: cell size
    float4 origin; // x, y: grid origin (X, Z), z: exposure, w: 1 when selected
    float4 color;  // linear base colour
    float4 light;  // xyz: direction toward the key light, w: up axis (0 Y, 1 Z)
    float* heights;
    uint cols;
    uint rows;
};

[[vk::push_constant]] HeightfieldPush pc;

struct VSOut
{
    float4 pos : SV_Position;
    float3 normal : NORMAL;
};

float heightAt(int x, int z)
{
    int nx = int(pc.grid.x), nz = int(pc.grid.y);
    x = clamp(x, 0, nx - 1);
    z = clamp(z, 0, nz - 1);
    return pc.heights[z * nx + x];
}

[shader("vertex")]
VSOut heightfieldVS(uint vid: SV_VertexID)
{
    static const int2 kCorner[6] = { int2(0, 0), int2(1, 0), int2(1, 1), int2(0, 0), int2(1, 1), int2(0, 1) };
    uint quad = vid / 6u;
    int2 c = kCorner[vid % 6u];
    int step = int(pc.grid.z);
    int nx = int(pc.grid.x), nz = int(pc.grid.y);
    int x = min((int(quad % pc.cols) + c.x) * step, nx - 1);
    int z = min((int(quad / pc.cols) + c.y) * step, nz - 1);
    float cell = pc.grid.w;
    float3 local = float3(pc.origin.x + float(x) * cell, heightAt(x, z), pc.origin.y + float(z) * cell);
    float4 p = float4(local, 1.0);
    float3 world = float3(dot(pc.model0, p), dot(pc.model1, p), dot(pc.model2, p));
    // Normal from the neighbours a step away, carried into the world (the placement has no shear).
    float dx = (heightAt(x + step, z) - heightAt(x - step, z)) / (2.0 * float(step) * cell);
    float dz = (heightAt(x, z + step) - heightAt(x, z - step)) / (2.0 * float(step) * cell);
    float3 n = float3(-dx, 1.0, -dz);
    float3 wn = float3(dot(pc.model0.xyz, n), dot(pc.model1.xyz, n), dot(pc.model2.xyz, n));
    VSOut o;
    o.pos = mul(pc.viewProj, float4(world, 1.0));
    o.normal = normalize(wn);
    return o;
}

[shader("fragment")]
float4 heightfieldPS(VSOut i) : SV_Target
{
    float3 n = normalize(i.normal);
    float3 up = pc.light.w > 0.5 ? float3(0, 0, 1) : float3(0, 1, 0);
    float key = max(dot(n, normalize(pc.light.xyz)), 0.0);
    float sky = 0.5 + 0.5 * dot(n, up);
    float3 base = pc.color.rgb;
    if (pc.origin.w > 0.5)
        base = lerp(base, float3(1.0, 0.55, 0.15), 0.25);
    float3 c = base * (0.9 * key + 0.35 * sky + 0.05);
    return float4(sceneCompress(c * pc.origin.z), 1.0);
}
