// Shaded meshes and the environment background: Unreal's default lit model (metallic/specular/roughness, packed
// ORM, DirectX or OpenGL normal maps, masked and translucent blending), split-sum image-based lighting with
// energy-conserving GGX, a camera-relative studio rig while no environment is loaded, Khronos PBR Neutral tonemap.
// mesh_bindless.slang compiles this with LILA_BINDLESS: material maps (and UDIM tiles) come from one runtime array.
// Stylised models (Painterly, Toon, Foliage) read a style block from the frame's table; see render::packStyle.

struct MeshPush
{
    float4x4 viewProj; // background: the inverse view-projection
    float4x4 world;
    float4 eye;     // xyz camera position, w: -1 when the world matrix mirrors (flips the bitangent)
    float4 base;    // rgb base color (linear), a: roughness. Background: x blur lod, y saturation, z exposure
    float4 params;  // x: exposure, y: 1 if Z-up, z: highlight amount, w: 0 studio rig, 1 environment, 2 flat (unlit)
    float4 env;     // xy: cos/sin of the rotation, z: intensity, w: lod of the roughest mip
    float4 surface; // x metallic, y specular, z opacity, w opacity mask clip
    float3 emissive; // linear; scales the emissive map when there is one
    uint flags;      // render::MaterialFlags
    float gamma;     // display gamma
    uint style;      // word offset of the draw's style block in the frame's table; kNoStyle for default lit
    float2 displayPad;
    uint* textureTable; // the frame's table: bindless texture handles, then style blocks
    uint2 maps;         // bindless: 16-bit table handles, x: base color | normal << 16, y: ORM | emissive << 16
};

[[vk::push_constant]] MeshPush pc;

struct EnvUniforms
{
    float4 sh[9]; // irradiance / pi, rgb
};

[[vk::binding(0, 0)]] SamplerCube envMap;
[[vk::binding(1, 0)]] Sampler2D brdfLut;
[[vk::binding(2, 0)]] ConstantBuffer<EnvUniforms> envU;

static const uint kBaseColorSlot = 0;
static const uint kNormalSlot = 1;
static const uint kOrmSlot = 2; // R occlusion, G roughness, B metallic
static const uint kEmissiveSlot = 3;

#ifdef LILA_BINDLESS
[[vk::binding(0, 1)]] Sampler2D materialTextures[];

static const uint kUdimEntry = 0x80000000;

/**
 * The array index a map samples at uv. Its table entry is the index itself, or for a UDIM set a header (kUdimEntry |
 * rows), the placeholder's index, then ten indices per row of tiles; the tile comes from floor of the V-up uv.
 */
uint mapIndex(uint slot, float2 uv)
{
    uint packed = slot < 2 ? pc.maps.x : pc.maps.y;
    uint h = (packed >> ((slot & 1) * 16)) & 0xffff;
    uint head = pc.textureTable[h];
    if ((head & kUdimEntry) == 0)
        return head;
    int col = int(floor(uv.x));
    int row = int(floor(1.0 - uv.y));
    if (col < 0 || col >= 10 || row < 0 || row >= int(head & 0xffff))
        return pc.textureTable[h + 1];
    return pc.textureTable[h + 2 + uint(row) * 10 + uint(col)];
}

// Virtual texture maps (render/VirtualTexture.h). The map's table entry: kVtEntry | chunk count, the page table's
// address (two words), the feedback group, then the bindless index of each chunk image of this map's layer. The page
// table starts with a 10x10 grid of UDIM cells (entry base + 1, width, height, mip count), then per tile and level the
// page entries: 0 for missing, else kVtResident | fade << 16 | slot.
static const uint kVtEntry = 0x40000000;
static const uint kVtResident = 0x80000000;
static const uint kVtPage = 128;
static const uint kVtBorder = 4;
static const uint kVtSlot = 136;
static const uint kVtChunkSlots = 16;
static const uint kVtFeedbackCell = 4;

// The pixel being shaded, for feedback (set at the top of each fragment entry point).
static uint2 gVtPixel;

struct VtTile
{
    uint base;
    uint2 size;
    uint mips;
};

uint* vtPageTable(uint h)
{
    uint64_t a = uint64_t(pc.textureTable[h + 1]) | (uint64_t(pc.textureTable[h + 2]) << 32);
    return (uint*)a;
}

uint2 vtLevelSize(VtTile t, uint m) { return max(t.size >> m, uint2(1, 1)); }
uint2 vtPages(uint2 levelSize) { return (levelSize + kVtPage - 1) / kVtPage; }

uint vtLevelBase(VtTile t, uint m)
{
    uint b = t.base;
    for (uint k = 0; k < m; ++k)
    {
        uint2 p = vtPages(vtLevelSize(t, k));
        b += p.x * p.y;
    }
    return b;
}

/** The entry of the page holding f (tile fraction, rows down) at level m, and that page. */
uint vtEntry(uint* pt, VtTile t, uint m, float2 f, out uint2 page)
{
    uint2 ls = vtLevelSize(t, m);
    uint2 pages = vtPages(ls);
    page = min(uint2(f * float2(ls)) / kVtPage, pages - 1);
    return pt[vtLevelBase(t, m) + page.y * pages.x + page.x];
}

float4 vtSlotSample(uint h, uint entry, VtTile t, uint m, float2 f, uint2 page)
{
    uint slot = entry & 0xffff;
    uint chunk = slot / (kVtChunkSlots * kVtChunkSlots);
    if (chunk >= (pc.textureTable[h] & 0xffff))
        return float4(0.0, 0.0, 0.0, 1.0);
    uint within = slot % (kVtChunkSlots * kVtChunkSlots);
    float2 origin = float2(float(within % kVtChunkSlots), float(within / kVtChunkSlots)) * float(kVtSlot) + float(kVtBorder);
    float2 p = origin + f * float2(vtLevelSize(t, m)) - float2(page * kVtPage);
    uint index = pc.textureTable[h + 4 + chunk];
    return materialTextures[NonUniformResourceIndex(index)].SampleLevel(p / float(kVtSlot * kVtChunkSlots), 0.0);
}

/** Level m as resident: the finest resident page at or above it, fading in over the next resident one up. */
float4 vtLevel(uint h, uint* pt, VtTile t, float2 f, uint m)
{
    uint2 page;
    uint l = m;
    uint e = 0;
    for (; l < t.mips; ++l)
    {
        e = vtEntry(pt, t, l, f, page);
        if (e != 0)
            break;
    }
    if (e == 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float4 c = vtSlotSample(h, e, t, l, f, page);
    float fade = float((e >> 16) & 0xff) / 255.0;
    if (fade >= 1.0)
        return c;
    for (uint k = l + 1; k < t.mips; ++k)
    {
        uint2 up;
        uint u = vtEntry(pt, t, k, f, up);
        if (u != 0)
            return lerp(vtSlotSample(h, u, t, k, f, up), c, fade);
    }
    return c;
}

/** One pixel of each 4x4 cell (cycling over frames) asks for the page it would sample at its level. */
void vtFeedback(uint h, uint cell, uint m, VtTile t, float2 f)
{
    uint64_t a = uint64_t(pc.textureTable[0]) | (uint64_t(pc.textureTable[1]) << 32);
    if (a == 0)
        return;
    uint jitter = pc.textureTable[3];
    if (gVtPixel.x % kVtFeedbackCell != (jitter & 0xff) || gVtPixel.y % kVtFeedbackCell != (jitter >> 8))
        return;
    uint2 page = min(uint2(f * float2(vtLevelSize(t, m))) / kVtPage, vtPages(vtLevelSize(t, m)) - 1);
    uint2* cells = (uint2*)a;
    uint index = (gVtPixel.y / kVtFeedbackCell) * pc.textureTable[2] + gVtPixel.x / kVtFeedbackCell;
    cells[index] = uint2(0x80000000 | (m << 24) | (cell << 16) | (pc.textureTable[h + 3] & 0xffff), page.x | (page.y << 16));
}

/** A virtual texture map at uv (V-up uv with the row flipped, as the vertex shader hands it on): trilinear over its levels. */
float4 vtSample(uint h, float2 uv)
{
    uint* pt = vtPageTable(h);
    // Derivatives first, while every pixel of the quad is still here.
    float2 duvx = ddx(uv), duvy = ddy(uv);
    int col = int(floor(uv.x));
    int row = int(floor(1.0 - uv.y));
    if (col < 0 || col >= 10 || row < 0 || row >= 10)
        return float4(0.0, 0.0, 0.0, 1.0);
    uint cell = uint(row) * 10 + uint(col);
    VtTile t;
    t.base = pt[cell * 4];
    if (t.base == 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    t.base -= 1;
    t.size = uint2(pt[cell * 4 + 1], pt[cell * 4 + 2]);
    t.mips = pt[cell * 4 + 3];
    float2 f = float2(frac(uv.x), frac(uv.y));
    float2 dx = duvx * float2(t.size), dy = duvy * float2(t.size);
    float lod = clamp(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12)), 0.0, float(t.mips - 1));
    uint m = uint(floor(lod));
    float blend = lod - float(m);
    vtFeedback(h, cell, m, t, f);
    float4 c = vtLevel(h, pt, t, f, m);
    if (blend > 0.01 && m + 1 < t.mips)
        c = lerp(c, vtLevel(h, pt, t, f, m + 1), blend);
    return c;
}

// A UDIM tile samples at uv itself: the repeating sampler wraps it into the tile.
float4 sampleMap(uint slot, float2 uv)
{
    uint packed = slot < 2 ? pc.maps.x : pc.maps.y;
    uint h = (packed >> ((slot & 1) * 16)) & 0xffff;
    uint head = pc.textureTable[h];
    if ((head & kUdimEntry) == 0 && (head & kVtEntry) != 0)
        return vtSample(h, uv);
    return materialTextures[NonUniformResourceIndex(mapIndex(slot, uv))].Sample(uv);
}
#else
// Per material. Missing maps are bound to 1x1 placeholders and ignored through the flags; a UDIM set binds its first tile.
[[vk::binding(0, 1)]] Sampler2D baseColorTex; // sRGB
[[vk::binding(1, 1)]] Sampler2D normalTex;
[[vk::binding(2, 1)]] Sampler2D ormTex;
[[vk::binding(3, 1)]] Sampler2D emissiveTex;  // sRGB
[[vk::binding(4, 1)]] Sampler2D strokeTex;    // Painterly stroke sheet, linear

static uint2 gVtPixel;

float4 sampleMap(uint slot, float2 uv)
{
    switch (slot)
    {
    case kBaseColorSlot: return baseColorTex.Sample(uv);
    case kNormalSlot: return normalTex.Sample(uv);
    case kOrmSlot: return ormTex.Sample(uv);
    default: return emissiveTex.Sample(uv);
    }
}
#endif

// Screen-space ambient occlusion at the viewport's resolution; 1x1 white while it is off.
[[vk::binding(0, 2)]] Sampler2D aoTex;
[[vk::binding(1, 2)]] Sampler2D aoDepthTex;  // the single-sample prepass the AO was computed from: depth
[[vk::binding(2, 2)]] Sampler2D aoNormalTex; // and world normal

/** How far a prepass texel is from being this fragment's surface: relative depth error, plus a penalty for a normal that turns away. */
float aoMismatch(int2 q, float depth, float3 n)
{
    float err = abs(aoDepthTex.Load(int3(q, 0)).r - depth) / max(depth, 1e-9);
    float3 nq = aoNormalTex.Load(int3(q, 0)).xyz;
    return err + (dot(nq, n) < 0.9 ? 1.0 : 0.0);
}

/**
 * The AO texel for a fragment. The AO is one sample per pixel but this pass is multisampled, so where two surfaces meet
 * a pixel's AO can belong to the other surface; take the neighbour that matches this fragment's depth and normal.
 */
float sampleAo(int2 px, float depth, float3 n)
{
    uint w, h;
    aoTex.GetDimensions(w, h);
    // The 1x1 stand-in while AO is off.
    if (w == 1 && h == 1)
        return aoTex.Load(int3(0, 0, 0)).r;
    int2 hi = int2(w, h) - 1;
    int2 best = min(px, hi);
    float bestErr = aoMismatch(best, depth, n);
    if (bestErr > 1e-3)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int2 q = clamp(px + int2(x, y), int2(0), hi);
                float err = aoMismatch(q, depth, n);
                if (err < bestErr)
                {
                    bestErr = err;
                    best = q;
                }
            }
    return aoTex.Load(int3(best, 0)).r;
}

/**
 * Widens roughness by how fast the normal turns across the pixel (Tokuyoshi and Kaplanyan 2019), so highlights on
 * curved or bevelled edges thinner than a pixel stop crawling as the camera moves.
 */
float specularAntialias(float3 n, float rough)
{
    float3 du = ddx(n), dv = ddy(n);
    float variance = 0.25 * (dot(du, du) + dot(dv, dv));
    float alpha = rough * rough;
    float alpha2 = saturate(alpha * alpha + min(2.0 * variance, 0.18));
    return sqrt(sqrt(alpha2));
}

static const uint kBaseColorMap = 1 << 0;
static const uint kNormalMap = 1 << 1;
static const uint kNormalGreenUp = 1 << 2;
static const uint kMetallicMap = 1 << 3;
static const uint kRoughnessMap = 1 << 4;
static const uint kOcclusionMap = 1 << 5;
static const uint kEmissiveMap = 1 << 6;
static const uint kMasked = 1 << 7;
static const uint kTranslucent = 1 << 8;
static const uint kUv1 = 1 << 10;
static const uint kVertexColorShaded = 1 << 11;
static const uint kVertexColorUnlit = 1 << 12;
static const uint kColorChannelShift = 13; // 3 bits: 0 rgb, 1 r, 2 g, 3 b, 4 a

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos : SV_Position;
    // Centroid: with MSAA a pixel the triangle only partly covers would otherwise extrapolate past its edge.
    centroid float3 worldPos : POSITION;
    centroid float3 normal : NORMAL;
    centroid float4 tangent : TANGENT;
    centroid float2 uv : TEXCOORD;
    centroid float4 color : COLOR;
    centroid float3 objPos : TEXCOORD1;
    centroid float3 objNormal : TEXCOORD2;
};

[shader("vertex")]
VSOut meshVS(VSIn v)
{
    VSOut o;
    float4 wp = mul(pc.world, float4(v.pos, 1.0));
    o.worldPos = wp.xyz;
    o.pos = mul(pc.viewProj, wp);
    // Inverse transpose would be exact under non-uniform scale; cofactor matrix gives it without an inverse.
    float3x3 m = (float3x3)pc.world;
    float3x3 cof = float3x3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    o.normal = mul(cof, v.normal);
    o.tangent = float4(mul(m, v.tangent.xyz), v.tangent.w * pc.eye.w);
    // Lila UVs are V-up (Maya, FBX) and image rows run top down; Unreal's FBX import flips V the same way.
    float2 uv = (pc.flags & kUv1) != 0 ? v.uv1 : v.uv;
    o.uv = float2(uv.x, 1.0 - uv.y);
    o.color = v.color;
    o.objPos = v.pos;
    o.objNormal = v.normal;
    return o;
}

static const float PI = 3.14159265;

float D_GGX(float nh, float a)
{
    float a2 = a * a;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float V_SmithGGX(float nv, float nl, float a)
{
    float a2 = a * a;
    float gv = nl * sqrt(nv * nv * (1.0 - a2) + a2);
    float gl = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

float3 F_Schlick(float3 f0, float vh)
{
    return f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
}

struct Surface
{
    float3 diffuse; // base color * (1 - metallic)
    float3 f0;      // 0.08 * specular for dielectrics, base color for metals
    float rough;
    float ao;   // material occlusion
    float ssao; // screen-space occlusion
};

float3 light(float3 n, float3 v, float3 l, float3 radiance, Surface s)
{
    float3 h = normalize(v + l);
    float nl = saturate(dot(n, l));
    float nv = max(dot(n, v), 1e-4);
    float a = s.rough * s.rough;
    float3 F = F_Schlick(s.f0, saturate(dot(v, h)));
    float3 spec = D_GGX(saturate(dot(n, h)), a) * V_SmithGGX(nv, nl, a) * F;
    float3 diff = (1.0 - F) * s.diffuse / PI;
    return (diff + spec) * radiance * nl;
}

// Khronos PBR Neutral: keeps hue and saturation of base colors intact up to the shoulder.
float3 neutralTonemap(float3 c)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression)
        return c;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(c, newPeak.xxx, g);
}

// Maya-style viewport gamma on the tonemapped linear color; 1 leaves it alone.
float3 displayGamma(float3 c)
{
    return pow(max(c, 0.0), 1.0 / max(pc.gamma, 0.01));
}

// Half an 8-bit step of noise, applied in sRGB space since that is where the swapchain quantizes.
float3 dither(float3 linear, float2 pixel)
{
    float noise = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))) - 0.5;
    float3 srgb = select(linear <= 0.0031308, linear * 12.92, 1.055 * pow(max(linear, 0.0), 1.0 / 2.4) - 0.055);
    srgb = saturate(srgb + noise / 255.0);
    return select(srgb <= 0.04045, srgb / 12.92, pow((srgb + 0.055) / 1.055, 2.4));
}

/** World direction to environment space: the environment's own up is +Y, rotated around it by the pref. */
float3 toEnv(float3 d)
{
    if (pc.params.y > 0.5)
        d = float3(d.x, d.z, -d.y);
    float c = pc.env.x, s = pc.env.y;
    return float3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

float3 irradiance(float3 n)
{
    float3 e = envU.sh[0].rgb * 0.282095;
    e += envU.sh[1].rgb * (0.488603 * n.y) + envU.sh[2].rgb * (0.488603 * n.z) + envU.sh[3].rgb * (0.488603 * n.x);
    e += envU.sh[4].rgb * (1.092548 * n.x * n.y) + envU.sh[5].rgb * (1.092548 * n.y * n.z);
    e += envU.sh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) + envU.sh[7].rgb * (1.092548 * n.x * n.z);
    e += envU.sh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return max(e, 0.0);
}

// Jimenez et al. 2016: bright surfaces bounce light back into their own creases, so occlusion darkens them less.
float3 multiBounce(float visibility, float3 albedo)
{
    float3 a = 2.0404 * albedo - 0.3324;
    float3 b = -4.7951 * albedo + 0.6417;
    float3 c = 2.7552 * albedo + 0.6903;
    return max(visibility, ((visibility * a + b) * visibility + c) * visibility);
}

// Lagarde and de Rousiers 2014.
float specularOcclusion(float nv, float visibility, float rough)
{
    return saturate(pow(nv + visibility, exp2(-16.0 * rough - 1.0)) - 1.0 + visibility);
}

float3 imageBased(float3 n, float3 v, Surface s)
{
    float nv = max(dot(n, v), 1e-4);
    float2 ab = brdfLut.SampleLevel(float2(nv, s.rough), 0).rg;
    float3 specular = s.f0 * ab.x + ab.y;
    // Multiple-scattering compensation (Fdez-Aguera): rough surfaces would otherwise lose energy.
    float3 energy = 1.0 + s.f0 * (1.0 / max(ab.x + ab.y, 1e-4) - 1.0);
    float3 prefiltered = envMap.SampleLevel(toEnv(reflect(-v, n)), s.rough * pc.env.w).rgb;
    float3 diffuse = irradiance(toEnv(n)) * s.diffuse * (1.0 - specular * energy) * multiBounce(s.ssao, s.diffuse);
    float3 reflected = prefiltered * specular * energy * specularOcclusion(nv, s.ssao, s.rough);
    // Material AO darkens indirect light only, as in Unreal; image-based light is all indirect.
    return (diffuse + reflected) * s.ao * pc.env.z;
}

float3 studioRig(float3 n, float3 v, Surface s)
{
    float3 up = pc.params.y > 0.5 ? float3(0, 0, 1) : float3(0, 1, 0);
    // Camera-relative rig, so every side you look at is readable: key above-left, fill opposite, rim behind.
    float3 fwd = -v;
    float3 right = normalize(cross(fwd, up) + 1e-5);
    float3 camUp = cross(right, fwd);
    float3 key = normalize(-fwd * 0.6 + camUp * 0.7 - right * 0.5);
    float3 fill = normalize(-fwd * 0.8 - camUp * 0.1 + right * 0.6);
    float3 rim = normalize(fwd * 0.9 + camUp * 0.4);

    float3 c = light(n, v, key, float3(2.6), s);
    c += light(n, v, fill, float3(0.9), s);
    c += light(n, v, rim, float3(1.2), s);
    // Sky/ground hemisphere ambient, neutral gray; metals get theirs through f0.
    float hemi = dot(n, up) * 0.5 + 0.5;
    c += (s.diffuse * multiBounce(s.ssao, s.diffuse) + s.f0 * 0.5 * s.ssao) * lerp(0.10, 0.28, hemi) * s.ao;
    return c;
}

/** The vertex color as shown: one channel isolates as grayscale, display-encoded so a 0.5 mask reads as mid gray. */
float3 vertexColor(float4 c, uint flags)
{
    uint channel = (flags >> kColorChannelShift) & 7;
    if (channel == 0)
        return max(c.rgb, 0.0);
    float v = saturate(channel == 1 ? c.r : channel == 2 ? c.g : channel == 3 ? c.b : c.a);
    return (v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4)).xxx;
}

#include "stylised.slang"

[shader("fragment")]
float4 meshPS(VSOut i, bool front : SV_IsFrontFace) : SV_Target
{
    gVtPixel = uint2(i.pos.xy);
    uint flags = pc.flags;
    if ((flags & kVertexColorUnlit) != 0)
        return float4(dither(vertexColor(i.color, flags), i.pos.xy), 1.0);
    float3 baseColor = pc.base.rgb;
    float opacity = pc.surface.z;
    if ((flags & kBaseColorMap) != 0)
    {
        float4 t = sampleMap(kBaseColorSlot, i.uv);
        baseColor = t.rgb;
        opacity *= t.a;
    }
    if ((flags & kMasked) != 0 && opacity < pc.surface.w)
        discard;
    if ((flags & kVertexColorShaded) != 0)
        baseColor *= vertexColor(i.color, flags);

    float3 ng = normalize(i.normal);
    float3 n = ng;
    if ((flags & kNormalMap) != 0)
    {
        float3 tn = sampleMap(kNormalSlot, i.uv).xyz * 2.0 - 1.0;
        // The bitangent points up the texture (V-up); DirectX maps store green pointing down it.
        if ((flags & kNormalGreenUp) == 0)
            tn.y = -tn.y;
        float3 t = normalize(i.tangent.xyz - ng * dot(ng, i.tangent.xyz) + 1e-7);
        float3 b = cross(ng, t) * (i.tangent.w < 0.0 ? -1.0 : 1.0);
        n = normalize(t * tn.x + b * tn.y + ng * max(tn.z, 1e-3));
    }
    const uint model = styleModel();
    Stroke stroke = { float3(0, 0, 1), 0.5, 0.5 };
    if (model == kPainterly && styleVec(0).z * styleVec(0).w > 0.0)
    {
        stroke = painterlyStroke(i, ng, front);
        n = bendNormal(n, ng, stroke);
    }
    if (!front)
        n = -n;

    Surface s;
    float metallic = pc.surface.x;
    s.rough = pc.base.a;
    s.ao = 1.0;
    s.ssao = 1.0;
    if ((flags & kTranslucent) == 0)
    {
        s.ssao = sampleAo(int2(i.pos.xy), i.pos.z, front ? ng : -ng);
    }
    if ((flags & (kMetallicMap | kRoughnessMap | kOcclusionMap)) != 0)
    {
        float3 orm = sampleMap(kOrmSlot, i.uv).rgb;
        if ((flags & kOcclusionMap) != 0)
            s.ao = orm.r;
        if ((flags & kRoughnessMap) != 0)
            s.rough = orm.g;
        if ((flags & kMetallicMap) != 0)
            metallic = orm.b;
    }
    s.rough = specularAntialias(n, clamp(s.rough, 0.04, 1.0));
    metallic = saturate(metallic);
    s.diffuse = baseColor * (1.0 - metallic);
    s.f0 = lerp(float3(0.08 * pc.surface.y), baseColor, metallic);

    float3 v = normalize(pc.eye.xyz - i.worldPos);
    float3 c;
    if (pc.params.w > 1.5)
        c = baseColor;
    else if (model == kToon)
        c = toonShade(n, v, baseColor, s);
    else
    {
        if (model == kPainterly)
            painterlySurface(stroke, n, v, baseColor, metallic, s);
        else if (model == kFoliage)
            foliageSurface(i, n, baseColor, metallic, s);
        c = pc.params.w > 0.5 ? imageBased(n, v, s) : studioRig(n, v, s);
        if (model == kFoliage)
            c = foliageLight(c, n, v, s);
    }
    float3 emissive = pc.emissive;
    if ((flags & kEmissiveMap) != 0)
        emissive *= sampleMap(kEmissiveSlot, i.uv).rgb;
    c += emissive;
    c *= pc.params.x;
    c = displayGamma(neutralTonemap(c));
    c = lerp(c, float3(1.0), pc.params.z);
    float alpha = (flags & kTranslucent) != 0 ? saturate(opacity) : 1.0;
    return float4(dither(c, i.pos.xy), alpha);
}

// Single-sample depth and world normal for the ambient occlusion pass. Masked materials cut out as in meshPS.
[shader("fragment")]
float4 prepassPS(VSOut i, bool front : SV_IsFrontFace) : SV_Target
{
    gVtPixel = uint2(i.pos.xy);
    if ((pc.flags & kMasked) != 0)
    {
        float opacity = pc.surface.z;
        if ((pc.flags & kBaseColorMap) != 0)
            opacity *= sampleMap(kBaseColorSlot, i.uv).a;
        if (opacity < pc.surface.w)
            discard;
    }
    float3 n = normalize(i.normal);
    return float4(front ? n : -n, 1.0);
}

struct BgOut
{
    float4 pos : SV_Position;
    float2 ndc;
};

[shader("vertex")]
BgOut backgroundVS(uint vid: SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    BgOut o;
    o.ndc = uv * 2.0 - 1.0;
    o.pos = float4(o.ndc, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 backgroundPS(BgOut i) : SV_Target
{
    float4 nearH = mul(pc.viewProj, float4(i.ndc, 1.0, 1.0));
    float4 farH = mul(pc.viewProj, float4(i.ndc, 0.001, 1.0));
    float3 rd = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);

    float3 c = envMap.SampleLevel(toEnv(rd), pc.base.x).rgb * pc.env.z * pc.base.z;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(luma.xxx, c, pc.base.y);
    c = displayGamma(neutralTonemap(c * pc.params.x));
    return float4(dither(c, i.pos.xy), 1.0);
}
