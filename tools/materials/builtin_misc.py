"""Plastics, rubber, glass, organics and sci-fi surfaces."""

from common import material, std, grime_noise
from matgraph import col, f


def dust_and_scuffs(m, P, scale=5):
    dust = grime_noise(m, scale=scale)
    scr = m.add("scratches", name="Scuffs", scale=6, count=5, length=0.4, width=0.0012, angleRandom=1.0)
    return m.px("""
float d0 = smoothstep(1.0 - p.y * 0.6, 1.25 - p.y * 0.6, texToFloat(a.sample(uv)));
float s = c.sample(uv) * smoothstep(0.0, 0.6, p.x);
return float4(d0, s, 0.0, 1.0);""", name="DustScuffs", a=dust, c=scr, x=P["wear"], y=P["age"])


@material("PlasticGloss", "Glossy Plastic", "Plastic/Gloss", "dielectric", 30, "Glossy injection-moulded ABS: saturated colour, faint orange peel, micro-scuffs and dust.", ["plastic", "abs", "glossy", "toy", "product"])
def plastic_gloss(m):
    P = std(m, wear=0.2, age=0.1, wear_doc="Micro-scuffs.", age_doc="Dust and yellowing.")
    c = m.param("plasticColor", "color", "#b8372d", None, None, "Colour", "The plastic's colour (sRGB).")
    tint = m.color(c, name="PlasticColor")
    peel = m.add("perlin", name="Peel", scale=64, octaves=2)
    D = dust_and_scuffs(m, P)
    base = m.px(f"""
float4 d = b.sample(uv);
float3 c0 = a.sample(uv).rgb * (1.0 + (c.sample(uv) - 0.5) * 0.03 * (0.5 + p.z));
c0 = lerp(c0, c0 * {col("#f0e6c8")}, p.y * 0.3);
c0 = lerp(c0, c0 * 1.1 + 0.04, d.y * 0.5);
c0 = lerp(c0, {col("#a8a39a")}, d.x * 0.7);
return float4(saturate(c0), 1.0);""", name="BaseColor", a=tint, b=D, c=peel, z=P["var"], y=P["age"])
    r = m.gray("0.12 + (c.sample(uv) - 0.5) * 0.04 + b.sample(uv).y * 0.3 + b.sample(uv).x * 0.7", name="Roughness", b=D, c=peel)
    h = m.gray("0.5 + (c.sample(uv) - 0.5) * 0.002 - b.sample(uv).y * 0.004", name="Height", b=D, c=peel)
    m.finish(base, r, 0.0, h, normal=0.4, ao=(0.005, 0.01, 0.2), hue=P["hue"])


@material("PlasticTextured", "Textured Plastic", "Plastic/Textured", "dielectric", 10, "Matte moulded plastic with a fine stipple texture, as on appliance housings and car interiors; high points polish with handling.", ["plastic", "textured", "stipple", "matte", "moulded", "interior"])
def plastic_textured(m):
    P = std(m, wear=0.3, age=0.2, wear_doc="Polished high points.", age_doc="Dust in the texture.")
    c = m.param("plasticColor", "color", "#3d3f43", None, None, "Colour", "The plastic's colour (sRGB).")
    tint = m.color(c, name="PlasticColor")
    st = m.add("worley", name="Stipple", scale=48, octaves=2, persistence=0.5, invert=True)
    D = dust_and_scuffs(m, P)
    base = m.px(f"""
float4 d = b.sample(uv);
float s = c.sample(uv);
float3 c0 = a.sample(uv).rgb * (0.95 + s * 0.08);
c0 = lerp(c0, {col("#8e8a82")}, d.x * (1.0 - s) * 0.8);
return float4(c0, 1.0);""", name="BaseColor", a=tint, b=D, c=st)
    r = m.gray("0.55 - c.sample(uv) * p.x * 0.3 + b.sample(uv).x * 0.3", name="Roughness", b=D, c=st, x=P["wear"])
    h = m.gray("0.5 + c.sample(uv) * 0.02", name="Height", c=st)
    m.finish(base, r, 0.0, h, normal=0.6, ao=(0.003, 0.02, 0.4), hue=P["hue"])


@material("Rubber", "Rubber", "Plastic/Rubber", "dielectric", 20, "Black vulcanised rubber: soft mottled sheen, shiny abrasion and pale bloom as it ages.", ["rubber", "black", "tyre", "gasket", "matte"])
def rubber(m):
    P = std(m, wear=0.2, age=0.2, hue=False, wear_doc="Shiny abraded patches.", age_doc="Pale bloom and dust.")
    mott = m.add("clouds", name="Mottle", scale=5, octaves=6)
    grain = m.add("perlin", name="Grain", scale=96, octaves=2)
    D = dust_and_scuffs(m, P)
    base = m.px(f"""
float4 d = b.sample(uv);
float3 c0 = {col("#2c2c2c")} * (0.92 + texToFloat(a.sample(uv)) * 0.14 * (0.5 + p.z));
c0 = lerp(c0, {col("#6c6964")}, d.x * 0.6);
return float4(c0, 1.0);""", name="BaseColor", a=mott, b=D, z=P["var"])
    r = m.gray("0.82 + (texToFloat(a.sample(uv)) - 0.5) * 0.1 - smoothstep(0.6, 0.9, texToFloat(a.sample(uv))) * p.x * 0.4 + b.sample(uv).x * 0.1", name="Roughness", a=mott, b=D, x=P["wear"])
    h = m.gray("0.5 + (c.sample(uv) - 0.5) * 0.004", name="Height", c=grain)
    m.finish(base, r, 0.0, h, normal=0.5, ao=(0.005, 0.01, 0.2))


@material("RubberTread", "Rubber Tread", "Plastic/Rubber", "dielectric", 30, "Tyre-style rubber tread: chevron blocks, deep grooves and fine sipes, worn smooth on the contact faces.", ["rubber", "tread", "tyre", "tire", "pattern"])
def rubber_tread(m):
    P = std(m, wear=0.3, age=0.2, hue=False, wear_doc="Contact faces worn smooth.", age_doc="Dust and mud in the grooves.")
    dirt = grime_noise(m, scale=5)
    M = m.px("""
float2 g = float2(uv.x * 4.0, uv.y * 8.0);
float2 fr = frac(g);
float row = floor(g.y);
float x = fr.x + abs(fr.y - 0.5) * 0.6 - (fmod(row, 2.0) * 0.5);
float bx = frac(x);
float block = smoothstep(0.08, 0.12, bx) * (1.0 - smoothstep(0.88, 0.92, bx)) * smoothstep(0.04, 0.08, fr.y) * (1.0 - smoothstep(0.92, 0.96, fr.y));
float sipe = 1.0 - smoothstep(0.0, 0.012, abs(frac(bx * 3.0) - 0.5) * 0.33);
float hgt = block * (1.0 - sipe * 0.6);
return float4(hgt, block, sipe, 1.0);""", name="Tread")
    base = m.px(f"""
float4 t = a.sample(uv);
float3 c0 = {col("#2b2b2b")} * (0.85 + t.x * 0.2);
float d = smoothstep(1.0 - p.y * 0.8, 1.2 - p.y * 0.8, texToFloat(b.sample(uv)) + (1.0 - t.y) * 0.4);
c0 = lerp(c0, {col("#6a5e4e")}, d * 0.7);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=dirt, y=P["age"])
    r = m.gray("lerp(0.92, 0.85 - p.x * 0.35, a.sample(uv).y)", name="Roughness", a=M, x=P["wear"])
    h = m.gray("0.2 + a.sample(uv).x * 0.6", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=1.0, ao=(0.02, 0.2, 1.0))


def glass(m, P, rough, opacity, frost):
    m.with_channel("opacity")
    fr = m.add("perlin", name="Frost", scale=96, octaves=3)
    smudge = m.add("clouds", name="Smudges", scale=4, octaves=6)
    scr = m.add("scratches", name="Scratches", scale=5, count=3, length=0.5, width=0.0008, angleRandom=1.0)
    M = m.px("""
float sm = smoothstep(1.0 - p.y * 0.7, 1.3 - p.y * 0.7, texToFloat(a.sample(uv)));
float sc = c.sample(uv) * smoothstep(0.0, 0.6, p.x);
return float4(sm, sc, d.sample(uv), 1.0);""", name="Dirt", a=smudge, c=scr, d=fr, x=P["wear"], y=P["age"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = {col("#d6dedc")} * (1.0 + (s.z - 0.5) * 0.02 * (0.5 + p.z));
c0 = lerp(c0, {col("#9a958a")}, s.x * 0.5);
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"])
    r = m.gray(f"{f(rough)} + (a.sample(uv).z - 0.5) * {f(frost * 0.2)} + a.sample(uv).x * 0.4 + a.sample(uv).y * 0.3", name="Roughness", a=M)
    op = m.gray(f"saturate({f(opacity)} + a.sample(uv).x * 0.4 + a.sample(uv).y * 0.3)", name="Opacity", a=M)
    h = m.gray(f"0.5 + (a.sample(uv).z - 0.5) * {f(frost * 0.01)} - a.sample(uv).y * 0.002", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.5, ao=(0.005, 0.01, 0.0), hue=P["hue"], opacity=op)


@material("GlassClear", "Clear Glass", "Glass/Clear", "dielectric", 50, "Window glass: almost invisible, with smudges and fine scratches that catch the light. Translucent: uses the opacity channel.", ["glass", "clear", "window", "transparent"])
def glass_clear(m):
    P = std(m, wear=0.1, age=0.15, wear_doc="Fine scratches.", age_doc="Smudges and grime.")
    glass(m, P, 0.03, 0.12, 0.0)


@material("GlassFrosted", "Frosted Glass", "Glass/Frosted", "dielectric", 30, "Acid-etched frosted glass: soft milky diffusion with a fine satin texture. Translucent: uses the opacity channel.", ["glass", "frosted", "etched", "satin", "translucent"])
def glass_frosted(m):
    P = std(m, wear=0.1, age=0.1, wear_doc="Polished, clearer patches.", age_doc="Smudges and grime.")
    glass(m, P, 0.45, 0.55, 1.0)


@material("Skin", "Skin Base", "Organic/Skin", "organic", 10, "A neutral skin base: pores, fine crossing wrinkles, subtle redness and tone variation. Hue shifts the tone.", ["skin", "human", "character", "pores", "organic"])
def skin(m):
    P = std(m, wear=0.2, age=0.2, wear_doc="Deeper wrinkles.", age_doc="Freckles and blotches.")
    pores = m.add("worley", name="Pores", scale=72, octaves=1)
    w1 = m.add("directional", name="Wrinkles", angle=30.0, scaleAlong=3, scaleAcross=64, octaves=3)
    w2 = m.add("directional", name="Wrinkles2", angle=150.0, scaleAlong=3, scaleAcross=56, octaves=3)
    blot = m.add("clouds", name="Redness", scale=4, octaves=6)
    M = m.px("""
float po = 1.0 - smoothstep(0.0, 0.18, a.sample(uv).r);
float k1 = 1.0 - smoothstep(0.0, 0.03, abs(texToFloat(b.sample(uv)) - 0.5));
float k2 = 1.0 - smoothstep(0.0, 0.03, abs(c.sample(uv) - 0.5));
float wr = max(k1, k2 * 0.7) * (0.4 + p.x);
return float4(po, wr, d.sample(uv), 1.0);""", name="Detail", a=pores, b=w1, c=w2, d=blot, x=P["wear"])
    freck = m.add("worley", name="Freckles", scale=60, octaves=1, invert=True)
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#c99a80")}, {col("#d6aa90")}, s.z * (0.4 + p.z * 0.6));
c0 = lerp(c0, {col("#c47e6c")}, smoothstep(0.55, 0.85, s.z) * 0.35);
c0 *= 1.0 - s.x * 0.06 - s.y * 0.05;
c0 = lerp(c0, {col("#b07a62")}, smoothstep(1.0 - p.y * 0.2, 1.05 - p.y * 0.2, texToFloat(b.sample(uv))) * 0.45);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=freck, z=P["var"], y=P["age"])
    r = m.gray("0.48 + a.sample(uv).x * 0.15 + a.sample(uv).y * 0.08 + (a.sample(uv).z - 0.5) * 0.06", name="Roughness", a=M)
    h = m.gray("0.5 - a.sample(uv).x * 0.006 - a.sample(uv).y * 0.006", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.6, ao=(0.003, 0.01, 0.3), hue=P["hue"])


@material("Bone", "Bone", "Organic/Bone", "organic", 15, "Weathered bone: ivory surface over porous, open bone where it is worn, fine cracks and brown staining.", ["bone", "ivory", "skeleton", "organic", "porous"])
def bone(m):
    P = std(m, wear=0.3, age=0.3, wear_doc="Worn through to porous bone.", age_doc="Cracks and staining.")
    porous = m.add("worley", name="Porous", scale=30, octaves=3, persistence=0.5)
    fibre = m.add("directional", name="Fibre", angle=90.0, scaleAlong=2, scaleAcross=40, octaves=4)
    expo = m.add("clouds", name="Exposed", scale=4, octaves=6)
    cracks = m.add("cells", name="Cracks", scale=4, mode="crackle", width=0.006, jitter=1.0)
    M = m.px("""
float ex = smoothstep(0.85 - p.x * 0.4, 0.9 - p.x * 0.4, texToFloat(c.sample(uv)));
float po = smoothstep(0.1, 0.3, a.sample(uv).r);
float ck = texToFloat(d.sample(uv)) * smoothstep(0.1, 0.8, p.y);
return float4(ex, po, texToFloat(b.sample(uv)), ck);""", name="Bone", a=porous, b=fibre, c=expo, d=cracks, x=P["wear"], y=P["age"])
    stain = grime_noise(m, scale=4)
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#d4c8aa")}, {col("#e2d8c0")}, s.z * (0.5 + p.z * 0.5));
float3 por = lerp({col("#8a7a5e")}, {col("#c6b48e")}, s.y);
c0 = lerp(c0, por, s.x);
c0 = lerp(c0, {col("#5a4632")}, s.w * 0.8);
c0 = lerp(c0, c0 * {col("#b89870")}, smoothstep(1.0 - p.y * 0.6, 1.3 - p.y * 0.6, texToFloat(b.sample(uv))));
return float4(c0, 1.0);""", name="BaseColor", a=M, b=stain, z=P["var"], y=P["age"])
    r = m.gray("lerp(0.55, 0.85, a.sample(uv).x) + a.sample(uv).w * 0.1", name="Roughness", a=M)
    h = m.gray("0.5 - a.sample(uv).x * (1.0 - a.sample(uv).y) * 0.08 + (a.sample(uv).z - 0.5) * 0.01 - a.sample(uv).w * 0.03", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.8, ao=(0.01, 0.05, 0.7), hue=P["hue"])


@material("Scales", "Reptile Scales", "Organic/Scales", "organic", 20, "Overlapping reptile scales: each its own green-olive, darker at the overlap, glossier on the crown.", ["scales", "reptile", "snake", "lizard", "dragon", "organic"])
def scales(m):
    P = std(m, wear=0.2, age=0.2, wear_doc="Scuffed, dulled crowns.", age_doc="Shedding: pale dry scales.")
    sc = m.add("scales", name="Scales", scale=12, size=1.25)
    mott = m.add("clouds", name="Pattern", scale=3, octaves=5)
    base = m.px(f"""
float hgt = texToFloat(a.sample(uv));
float r = texToFloat(b.sample(uv));
float pat = c.sample(uv);
float3 c0 = lerp({col("#3e5a2e")}, {col("#6e8a3c")}, saturate(r * 0.6 * (0.4 + p.z) + pat * 0.6));
c0 = lerp(c0, {col("#2a3420")}, smoothstep(0.55, 0.75, pat) * 0.7);
c0 *= 0.55 + hgt * 0.55;
c0 = lerp(c0, {col("#b8b49a")}, step(1.0 - p.y * 0.3, frac(r * 7.0)) * 0.6);
return float4(c0, 1.0);""", name="BaseColor", a=sc["height"], b=sc["random"], c=mott, z=P["var"], y=P["age"])
    r = m.gray("lerp(0.65, 0.35, texToFloat(a.sample(uv))) + p.x * 0.2", name="Roughness", a=sc["height"], x=P["wear"])
    h = m.gray("0.3 + texToFloat(a.sample(uv)) * 0.4", name="Height", a=sc["height"])
    m.finish(base, r, 0.0, h, normal=1.0, ao=(0.01, 0.08, 0.8), hue=P["hue"])


def panel_paint(m, P, M, paint, edges_metal=True):
    """M holds (height, seam, edge, panel random). Paint over aluminium, chipping on the edges, grime in the seams."""
    chip = m.add("perlin", name="ChipNoise", scale=24, octaves=5, persistence=0.6)
    grime = grime_noise(m, scale=5)
    W = m.px("""
float4 s = a.sample(uv);
float ch = smoothstep(1.0 - p.x * 0.9, 1.05 - p.x * 0.9, s.z * 0.9 + texToFloat(b.sample(uv)) * 0.4);
float g = smoothstep(1.0 - p.y * 0.8, 1.2 - p.y * 0.8, texToFloat(c.sample(uv)) * 0.6 + s.y * 0.6);
return float4(ch, g, 0.0, 1.0);""", name="Wear", a=M, b=chip, c=grime, x=P["wear"], y=P["age"])
    tint = m.color(paint, name="PaintColor")
    pcol = m.px("""
float4 s = a.sample(uv);
return float4(b.sample(uv).rgb * (1.0 + (s.w - 0.5) * 0.12 * (0.3 + p.z)), s.y);""", name="PanelTone", a=M, b=tint, z=P["var"])
    base = m.px(f"""
float4 pc = a.sample(uv);
float4 w = b.sample(uv);
float3 c0 = lerp(pc.rgb, {col("#e6e7e8")}, w.x);
c0 = lerp(c0, {col("#3a3632")}, w.y * 0.7);
c0 = lerp(c0, c0 * 0.4, pc.a);
return float4(c0, 1.0);""", name="BaseColor", a=pcol, b=W)
    met = m.gray("b.sample(uv).x * (1.0 - b.sample(uv).y)", name="MetalMask", b=W)
    r = m.gray("lerp(0.38 + (a.sample(uv).w - 0.5) * 0.08, 0.3, b.sample(uv).x) + b.sample(uv).y * 0.4 + a.sample(uv).y * 0.3", name="Roughness", a=M, b=W)
    h = m.gray("0.3 + a.sample(uv).x * 0.4 - b.sample(uv).x * 0.004", name="Height", a=M, b=W)
    return base, r, met, h


@material("SciFiPanels", "Sci-Fi Panels", "Sci-Fi/Panels", "metal", 200, "Hull plating: large and subdivided painted panels, recessed seams, corner bolts, vent slots, chipped edges.", ["sci-fi", "scifi", "panels", "hull", "spaceship", "plating"])
def scifi_panels(m):
    P = std(m, wear=0.3, age=0.25, wear_doc="Paint chipped off the panel edges.", age_doc="Grime in the seams.")
    paint = m.param("paintColor", "color", "#a9adb1", None, None, "Paint Colour", "The hull paint (sRGB).")
    M = m.px("""
float2 g1 = uv * 2.0;
float2 c1 = floor(g1);
float r1 = texRand(uint2(c1 + 17.0), 7u);
float sub = r1 > 0.45 ? 2.0 : 1.0;
float2 g = uv * 2.0 * sub;
float2 c2 = floor(g);
float2 fr = frac(g);
float r2 = texRand(uint2(c2 + 31.0), 11u);
float2 e2 = min(fr, 1.0 - fr) / (2.0 * sub);
float ed = min(e2.x, e2.y);
float seam = 1.0 - smoothstep(0.002, 0.005, ed);
float bevel = smoothstep(0.003, 0.012, ed);
float2 bq = min(fr, 1.0 - fr) / (2.0 * sub);
float bolt = 1.0 - smoothstep(0.004, 0.006, length(bq - 0.018));
float vent = 0.0;
if (r2 > 0.72)
{
    float2 vq = fr - 0.5;
    float inside = step(abs(vq.x), 0.3) * step(abs(vq.y), 0.22);
    vent = inside * (1.0 - smoothstep(0.25, 0.45, abs(frac(vq.y * 9.0) - 0.5) * 2.0));
}
float hgt = bevel * 0.7 + bolt * 0.15 - vent * 0.35 + (r2 - 0.5) * 0.02;
float edge = (1.0 - smoothstep(0.005, 0.03, ed)) + bolt * 0.5;
return float4(saturate(hgt + 0.2), saturate(seam + vent), saturate(edge), r2);""", name="Panels")
    base, r, met, h = panel_paint(m, P, M, paint)
    m.finish(base, r, met, h, normal=1.0, ao=(0.01, 0.1, 0.9), hue=P["hue"])


@material("SciFiTrim", "Sci-Fi Trim Sheet", "Sci-Fi/Trims", "metal", 100, "A horizontal trim sheet: grooved strip, glowing light strip, ribbed vent, bolted plate, chamfered rails and hazard stripes. Emissive.", ["sci-fi", "scifi", "trim", "trim sheet", "emissive", "hazard"])
def scifi_trim(m):
    P = std(m, wear=0.3, age=0.2, wear_doc="Paint chipped off the edges.", age_doc="Grime in the recesses.")
    glow = m.param("glowColor", "color", "#38d0ff", None, None, "Glow Colour", "The light strip's colour.")
    M = m.px("""
float v = uv.y;
float hgt = 0.5;
float seam = 0.0;
float edge = 0.0;
float band = 0.0;
float lv;
if (v < 0.14) { lv = v / 0.14; hgt = 0.55 - (1.0 - smoothstep(0.0, 0.08, abs(frac(lv * 3.0) - 0.5) - 0.3)) * 0.2; band = 0.0; }
else if (v < 0.26) { lv = (v - 0.14) / 0.12; hgt = 0.4 + smoothstep(0.0, 0.2, lv) * (1.0 - smoothstep(0.8, 1.0, lv)) * 0.1; band = 1.0; }
else if (v < 0.48) { lv = (v - 0.26) / 0.22; hgt = 0.45 + 0.2 * abs(sin(uv.x * 3.14159 * 48.0)); band = 2.0; }
else if (v < 0.7) { lv = (v - 0.48) / 0.22; float2 bq = float2(frac(uv.x * 8.0) - 0.5, lv - 0.5); float bolt = 1.0 - smoothstep(0.08, 0.1, length(bq * float2(1.0, 0.35))); hgt = 0.6 + bolt * 0.1; band = 3.0; edge = bolt * 0.5; }
else if (v < 0.86) { lv = (v - 0.7) / 0.16; hgt = 0.5 + min(lv, 1.0 - lv) * 0.6; band = 4.0; }
else { lv = (v - 0.86) / 0.14; hgt = 0.55; band = 5.0; }
float bandEdge = min(lv, 1.0 - lv);
seam = 1.0 - smoothstep(0.0, 0.04, bandEdge);
edge = max(edge, 1.0 - smoothstep(0.04, 0.12, bandEdge));
hgt -= seam * 0.2;
return float4(hgt, seam, edge, band / 5.0);""", name="Bands")
    chip = m.add("perlin", name="ChipNoise", scale=24, octaves=5, persistence=0.6)
    grime = grime_noise(m, scale=5)
    W = m.px("""
float4 s = a.sample(uv);
float ch = smoothstep(1.0 - p.x * 0.9, 1.05 - p.x * 0.9, s.z * 0.9 + texToFloat(b.sample(uv)) * 0.4);
float g = smoothstep(1.0 - p.y * 0.8, 1.2 - p.y * 0.8, texToFloat(c.sample(uv)) * 0.6 + s.y * 0.6);
return float4(ch, g, 0.0, 1.0);""", name="Wear", a=M, b=chip, c=grime, x=P["wear"], y=P["age"])
    tint = m.color(glow, name="GlowColor")
    base = m.px(f"""
float4 s = a.sample(uv);
float4 w = b.sample(uv);
int band = int(round(s.w * 5.0));
float3 c0 = {col("#5d6166")};
if (band == 1) c0 = {col("#2a2c30")};
if (band == 2) c0 = {col("#c2c5c8")};
if (band == 4) c0 = {col("#d0d2d4")};
if (band == 5) c0 = frac((uv.x - uv.y) * 12.0) < 0.5 ? {col("#d8a520")} : {col("#2a2a2a")};
c0 *= 1.0 + (p.z - 0.5) * 0.1;
c0 = lerp(c0, {col("#d8dadc")}, w.x * (band == 1 ? 0.0 : 1.0));
c0 = lerp(c0, {col("#3a3632")}, w.y * 0.7);
c0 = lerp(c0, c0 * 0.45, s.y);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=W, z=P["var"])
    em = m.px("""
float4 s = a.sample(uv);
float on = (abs(s.w - 0.2) < 0.05 ? 1.0 : 0.0);
float lv = saturate((uv.y - 0.14) / 0.12);
float strip = smoothstep(0.3, 0.4, lv) * (1.0 - smoothstep(0.6, 0.7, lv));
return float4(b.sample(uv).rgb * on * strip, 1.0);""", name="Emissive", a=M, b=tint)
    met = m.px("""
float4 s = a.sample(uv);
int band = int(round(s.w * 5.0));
float v = (band == 2 || band == 4) ? 1.0 : b.sample(uv).x;
v *= 1.0 - b.sample(uv).y;
return float4(v, v, v, 1.0);""", name="MetalMask", a=M, b=W)
    r = m.gray("0.35 + a.sample(uv).y * 0.3 + b.sample(uv).y * 0.4 - b.sample(uv).x * 0.1", name="Roughness", a=M, b=W)
    h = m.gray("a.sample(uv).x", name="Height", a=M)
    m.finish(base, r, met, h, normal=1.0, ao=(0.01, 0.1, 0.9), emissive=em)


@material("SciFiHex", "Hex Plating", "Sci-Fi/Panels", "metal", 80, "Armoured hexagonal plating: bevelled hex tiles in two finishes, dark seams and worn edges.", ["sci-fi", "scifi", "hex", "hexagon", "armour", "plating"])
def scifi_hex(m):
    P = std(m, wear=0.3, age=0.2, wear_doc="Paint chipped off the hex edges.", age_doc="Grime in the seams.")
    paint = m.param("paintColor", "color", "#4f5a52", None, None, "Paint Colour", "The plating paint (sRGB).")
    hx = m.add("hex", name="Hexes", scale=10, gap=0.05, bevel=0.12)
    M = m.px("""
float mask = texToFloat(a.sample(uv));
float r = c.sample(uv);
float hgt = 0.2 + mask * 0.5 + (r - 0.5) * 0.03 * mask;
float seam = 1.0 - smoothstep(0.0, 0.2, mask);
float edge = (1.0 - smoothstep(0.6, 0.95, mask)) * step(0.05, mask);
return float4(hgt, seam, edge, r);""", name="Hex", a=hx["mask"], c=hx["random"])
    base, r, met, h = panel_paint(m, P, M, paint)
    m.finish(base, r, met, h, normal=1.0, ao=(0.01, 0.1, 0.9), hue=P["hue"])
