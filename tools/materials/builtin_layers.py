"""Semantic layer materials: what material-aware painting and smart materials lay over another surface.

Each carries meta.layer: the generator that suits it as a mask, the channels it should write and their blend modes.
"""

from common import material, std, grime_noise
from matgraph import col, f


def layer_meta(m, generator, channels, blend=None):
    m.meta["layer"] = {"generator": generator, "channels": channels, "blend": blend or {}}


@material("LayerRust", "Rust", "Layer/Rust", "dielectric", 60, "Rust as a layer: powdery orange to dark brown oxide with flakes and pits, for painting onto metal.", ["layer", "rust", "corrosion", "smart"])
def layer_rust(m):
    P = std(m, wear=0.4, age=0.5, hue=False, wear_doc="Flaking.", age_doc="Darker, deeper rust.")
    layer_meta(m, "genRustSpread", ["baseColor", "roughness", "metallic", "height"])
    grain = m.add("perlin", name="Grain", scale=40, octaves=5, persistence=0.6)
    blot = m.add("clouds", name="Blotch", scale=5, octaves=6)
    cells = m.add("cells", name="Flakes", scale=22, mode="cells", jitter=1.0)
    M = m.px("""
float g = d.sample(uv);
float bl = texToFloat(a.sample(uv));
float deep = smoothstep(0.6 - p.y * 0.4, 0.9 - p.y * 0.4, bl);
float fl = smoothstep(1.0 - p.x * 0.4, 1.04 - p.x * 0.4, c.sample(uv)) * deep;
return float4(g, deep, fl, bl);""", name="Rust", a=blot, c=cells, d=grain, x=P["wear"], y=P["age"])
    base = m.px(f"""
float4 r = a.sample(uv);
float g = saturate(r.x + (r.w - 0.5) * p.z);
float3 powder = lerp({col("#7a3d1f")}, {col("#a85a2a")}, saturate(g * 1.6 - 0.35));
float3 dark = lerp({col("#3b1f14")}, {col("#5a2f1b")}, g);
float3 c0 = lerp(powder, dark, saturate(r.y * 0.8 - g * 0.6 + 0.15));
c0 = lerp(c0, lerp({col("#3a2117")}, {col("#5e3a26")}, g), r.z);
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"])
    r = m.gray("lerp(0.8, 0.96, a.sample(uv).x)", name="Roughness", a=M)
    h = m.gray("0.5 + a.sample(uv).y * 0.04 * (a.sample(uv).x + 0.5) - a.sample(uv).z * 0.06", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.7, ao=(0.02, 0.06, 0.8))


@material("LayerWetness", "Wetness", "Layer/Wetness", "dielectric", 100, "Water film as a layer: multiplies the colour darker and makes the surface glossy, with faint drip streaks.", ["layer", "wet", "water", "rain", "smart"])
def layer_wetness(m):
    P = std(m, wear=0.2, age=0.5, hue=False, wear_doc="Drip streaks.", age_doc="How saturated (darker) the surface is.")
    layer_meta(m, "genWetness", ["baseColor", "roughness", "normal"], {"baseColor": "multiply"})
    streak = m.add("directional", name="Drips", angle=90.0, scaleAlong=2, scaleAcross=40, octaves=3)
    ripple = m.add("perlin", name="Film", scale=24, octaves=3)
    base = m.gray("lerp(0.75, 0.45, p.y) - smoothstep(0.65, 0.85, texToFloat(a.sample(uv))) * p.x * 0.15", name="BaseColor", a=streak, x=P["wear"], y=P["age"])
    r = m.gray("0.06 + (c.sample(uv) - 0.5) * 0.04 * (0.5 + p.z)", name="Roughness", c=ripple, z=P["var"])
    h = m.gray("0.5 + (c.sample(uv) - 0.5) * 0.004", name="Height", c=ripple)
    m.finish(base, r, 0.0, h, normal=0.3, ao=(0.005, 0.01, 0.0))


@material("LayerDust", "Dust", "Layer/Dust", "dielectric", 60, "Settled dust as a layer: soft pale grey-beige, fully matte, thicker in drifts.", ["layer", "dust", "dusty", "smart"])
def layer_dust(m):
    P = std(m, wear=0.2, age=0.5, wear_doc="Wiped streaks.", age_doc="Dust thickness.")
    layer_meta(m, "genDust", ["baseColor", "roughness", "height"])
    drift = m.add("clouds", name="Drifts", scale=5, octaves=6)
    fluff = m.add("perlin", name="Fluff", scale=80, octaves=2)
    base = m.px(f"""
float d = texToFloat(a.sample(uv));
float3 c0 = lerp({col("#9a9284")}, {col("#c2b9a8")}, saturate(d * 0.7 + p.y * 0.3 + (c.sample(uv) - 0.5) * 0.3 * p.z));
return float4(c0, 1.0);""", name="BaseColor", a=drift, c=fluff, y=P["age"], z=P["var"])
    r = m.gray("0.95", name="Roughness")
    h = m.gray("0.5 + texToFloat(a.sample(uv)) * 0.01 * (0.5 + p.y) + (c.sample(uv) - 0.5) * 0.004", name="Height", a=drift, c=fluff, y=P["age"])
    m.finish(base, r, 0.0, h, normal=0.3, ao=(0.005, 0.01, 0.0), hue=P["hue"])


@material("LayerDirt", "Dirt", "Layer/Dirt", "organic", 80, "Grime as a layer: brown-grey dirt blotches and speckle that gather in crevices.", ["layer", "dirt", "grime", "mud", "smart"])
def layer_dirt(m):
    P = std(m, wear=0.2, age=0.5, wear_doc="Scuffed through.", age_doc="Darker, heavier grime.")
    layer_meta(m, "genDirt", ["baseColor", "roughness", "height"])
    g = grime_noise(m, scale=6)
    blot = m.add("clouds", name="Blotch", scale=4, octaves=6)
    base = m.px(f"""
float d = texToFloat(a.sample(uv));
float b0 = texToFloat(b.sample(uv));
float3 c0 = lerp({col("#7a6a56")}, {col("#4a3d30")}, saturate(d * 0.6 + p.y * 0.5 + (b0 - 0.5) * 0.4 * p.z));
return float4(c0, 1.0);""", name="BaseColor", a=g, b=blot, y=P["age"], z=P["var"])
    r = m.gray("0.88 + (texToFloat(a.sample(uv)) - 0.5) * 0.1", name="Roughness", a=g)
    h = m.gray("0.5 + texToFloat(a.sample(uv)) * 0.015", name="Height", a=g)
    m.finish(base, r, 0.0, h, normal=0.5, ao=(0.005, 0.02, 0.3), hue=P["hue"])


@material("LayerMoss", "Moss", "Layer/Moss", "organic", 40, "Moss as a layer: soft cushions of bright and olive green with fuzzy height, for growth on stone, wood and roofs.", ["layer", "moss", "growth", "green", "smart"])
def layer_moss(m):
    P = std(m, wear=0.2, age=0.4, wear_doc="Trampled, browned moss.", age_doc="Thicker, darker cushions.")
    layer_meta(m, "genMoss", ["baseColor", "roughness", "normal", "height", "ao"])
    cushions = m.add("worley", name="Cushions", scale=10, octaves=3, persistence=0.5, invert=True)
    fuzz = m.add("perlin", name="Fuzz", scale=120, octaves=2)
    tone = m.add("clouds", name="Tone", scale=4, octaves=5)
    M = m.px("""
float cu = texToFloat(a.sample(uv));
float fz = c.sample(uv);
float hgt = cu * 0.8 + fz * 0.2;
return float4(hgt, fz, d.sample(uv), 1.0);""", name="Moss", a=cushions, c=fuzz, d=tone)
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#3c5420")}, {col("#7a9a34")}, saturate(s.x * 0.8 + (s.z - 0.5) * 0.6 * (0.4 + p.z)));
c0 = lerp(c0, {col("#2e3e1a")}, p.y * 0.3 * (1.0 - s.x));
c0 = lerp(c0, {col("#7a6e3e")}, smoothstep(0.6, 0.9, s.z) * p.x);
c0 *= 0.8 + s.y * 0.3;
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"], y=P["age"], x=P["wear"])
    r = m.gray("0.9 + (a.sample(uv).y - 0.5) * 0.1", name="Roughness", a=M)
    h = m.gray("0.5 + a.sample(uv).x * 0.15 * (0.5 + p.y)", name="Height", a=M, y=P["age"])
    m.finish(base, r, 0.0, h, normal=1.0, ao=(0.01, 0.06, 0.8), hue=P["hue"])


@material("LayerClearCoat", "Clear Coat", "Layer/Clear Coat", "dielectric", 50, "A glossy clear lacquer as a layer: changes only roughness and adds a faint orange peel, over any base.", ["layer", "clear coat", "lacquer", "varnish", "gloss", "smart"])
def layer_clear_coat(m):
    P = std(m, wear=0.15, age=0.1, hue=False, wear_doc="Swirl marks and scuffs.", age_doc="Dulling.")
    layer_meta(m, None, ["roughness", "normal"])
    peel = m.add("perlin", name="Peel", scale=64, octaves=2)
    swirl = m.add("scratches", name="Swirls", scale=8, count=6, length=0.3, width=0.0006, angleRandom=1.0)
    base = m.add("color", name="Base", color="#808080")
    r = m.gray("0.04 + c.sample(uv) * p.x * 0.3 + p.y * 0.2 + (d.sample(uv) - 0.5) * 0.02 * (0.5 + p.z)", name="Roughness", c=swirl, d=peel, x=P["wear"], y=P["age"], z=P["var"])
    h = m.gray("0.5 + (d.sample(uv) - 0.5) * 0.003", name="Height", d=peel)
    m.finish(base, r, 0.0, h, normal=0.5, ao=(0.005, 0.01, 0.0))


@material("LayerPaintChip", "Paint Chips", "Layer/Paint", "dielectric", 60, "A paint coat as a layer, for masking with chips: satin enamel with thickness, so chipped edges step down to what is under.", ["layer", "paint", "chips", "enamel", "smart"])
def layer_paint_chip(m):
    P = std(m, wear=0.2, age=0.2, wear_doc="Scuffs on the paint.", age_doc="Fading and chalking.")
    layer_meta(m, "genPaintChips", ["baseColor", "roughness", "metallic", "height"])
    paint = m.param("paintColor", "color", "#b0472f", None, None, "Paint Colour", "The paint's colour (sRGB).")
    tint = m.color(paint, name="PaintColor")
    peel = m.add("perlin", name="Peel", scale=64, octaves=2)
    scr = m.add("scratches", name="Scuffs", scale=6, count=4, length=0.4, width=0.001, angleRandom=1.0)
    base = m.px("""
float3 c0 = a.sample(uv).rgb * (1.0 + (d.sample(uv) - 0.5) * 0.05 * (0.5 + p.z));
c0 = lerp(c0, dot(c0, float3(0.3, 0.4, 0.3)).xxx * 1.15 + 0.05, p.y * 0.4);
c0 = lerp(c0, c0 * 1.15, c.sample(uv) * p.x);
return float4(saturate(c0), 1.0);""", name="BaseColor", a=tint, c=scr, d=peel, x=P["wear"], y=P["age"], z=P["var"])
    r = m.gray("0.38 + p.y * 0.3 + c.sample(uv) * p.x * 0.3", name="Roughness", c=scr, x=P["wear"], y=P["age"])
    h = m.gray("0.56 + (d.sample(uv) - 0.5) * 0.004", name="Height", d=peel)
    m.finish(base, r, 0.0, h, normal=0.4, ao=(0.005, 0.01, 0.0), hue=P["hue"])


@material("LayerBareMetal", "Bare Metal", "Layer/Bare Metal", "metal", 40, "Freshly exposed steel as a layer, for edge wear: bright, finely scratched, slightly rougher where abraded.", ["layer", "bare metal", "edge wear", "steel", "scratched", "smart"])
def layer_bare_metal(m):
    P = std(m, wear=0.5, age=0.1, hue=False, wear_doc="Scratch density.", age_doc="Tarnish.")
    layer_meta(m, "genEdgeWear", ["baseColor", "roughness", "metallic", "height"])
    s1 = m.add("scratches", name="Scratches", scale=8, count=6, length=0.5, width=0.0008, angleRandom=1.0)
    mott = m.add("clouds", name="Mottle", scale=4, octaves=5)
    base = m.px(f"""
float3 c0 = {col("#cfd1d2")} * (0.95 + texToFloat(a.sample(uv)) * 0.08 * (0.5 + p.z));
c0 = lerp(c0, {col("#a8a49e")}, p.y * 0.5);
c0 *= 1.0 - c.sample(uv) * p.x * 0.1;
return float4(c0, 1.0);""", name="BaseColor", a=mott, c=s1, x=P["wear"], y=P["age"], z=P["var"])
    r = m.gray("0.28 + c.sample(uv) * p.x * 0.25 + texToFloat(a.sample(uv)) * 0.1", name="Roughness", a=mott, c=s1, x=P["wear"])
    h = m.gray("0.5 - c.sample(uv) * p.x * 0.02", name="Height", c=s1, x=P["wear"])
    m.finish(base, r, 1.0, h, normal=0.3, ao=(0.005, 0.01, 0.0))


@material("LayerBurn", "Burn", "Layer/Burn", "dielectric", 50, "Char and soot as a layer: crazed black char at the centre, sooty smudge at the edges, a few glowing-hot embers' worth of brown.", ["layer", "burn", "char", "soot", "fire", "smart"])
def layer_burn(m):
    P = std(m, wear=0.4, age=0.3, hue=False, wear_doc="Char crazing.", age_doc="Soot ashing to grey.")
    layer_meta(m, None, ["baseColor", "roughness", "height"])
    craze = m.add("cells", name="Craze", scale=14, mode="crackle", width=0.02, jitter=1.0)
    soot = m.add("clouds", name="Soot", scale=4, octaves=6)
    M = m.px("""
float cz = texToFloat(a.sample(uv)) * p.x;
float st = texToFloat(b.sample(uv));
return float4(cz, st, 0.0, 1.0);""", name="Char", a=craze, b=soot, x=P["wear"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#2a2624")}, {col("#4a3e34")}, saturate(s.y * 0.6 * (0.5 + p.z)));
c0 = lerp(c0, {col("#6e6a66")}, p.y * smoothstep(0.4, 0.8, s.y) * 0.6);
c0 = lerp(c0, {col("#1f1d1c")}, s.x);
return float4(c0, 1.0);""", name="BaseColor", a=M, y=P["age"], z=P["var"])
    r = m.gray("0.9 - a.sample(uv).x * 0.2", name="Roughness", a=M)
    h = m.gray("0.5 - a.sample(uv).x * 0.04 + (a.sample(uv).y - 0.5) * 0.01", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.8, ao=(0.01, 0.03, 0.5))


@material("LayerSnowCover", "Snow Cover", "Layer/Snow", "dielectric", 100, "A dusting to a blanket of snow as a layer: bright soft snow with glints, piling higher where it settles.", ["layer", "snow", "winter", "cover", "smart"])
def layer_snow_cover(m):
    P = std(m, wear=0.2, age=0.3, hue=False, wear_doc="Trodden, compacted snow.", age_doc="Thicker cover.")
    layer_meta(m, "genDust", ["baseColor", "roughness", "normal", "height"])
    drift = m.add("clouds", name="Drifts", scale=4, octaves=6)
    glint = m.add("cells", name="Glints", scale=120, mode="cells", jitter=1.0)
    base = m.px(f"""
float d = texToFloat(a.sample(uv));
float3 c0 = lerp({col("#c8d4e0")}, {col("#e8ecef")}, smoothstep(0.2, 0.7, d) * (0.6 + p.z * 0.4));
return float4(c0, 1.0);""", name="BaseColor", a=drift, z=P["var"])
    r = m.gray("lerp(0.75, 0.55, p.x) - step(0.985, c.sample(uv)) * 0.4", name="Roughness", c=glint, x=P["wear"])
    h = m.gray("0.5 + texToFloat(a.sample(uv)) * 0.08 * (0.4 + p.y)", name="Height", a=drift, y=P["age"])
    m.finish(base, r, 0.0, h, normal=0.6, ao=(0.02, 0.05, 0.4))


@material("LayerBlood", "Blood", "Layer/Blood", "organic", 40, "Blood as a layer: glossy dark red drops, splashes and smears, browning and dulling as it dries.", ["layer", "blood", "gore", "splatter", "smart"])
def layer_blood(m):
    P = std(m, wear=0.3, age=0.3, wear_doc="Smeared, wiped streaks.", age_doc="Drying: browner, duller, cracked.")
    layer_meta(m, "genDrips", ["baseColor", "roughness", "height"])
    drop = m.add("shape", name="Drop", shape="circle", size=0.7, softness=2.0)
    drops = m.add("splatter", name="Drops", pattern=drop["mask"], countX=8, countY=8, perCell=3, size=0.35, sizeRandom=0.8, valueRandom=0.2, blend="max")
    smear = m.add("directional", name="Smear", angle=20.0, scaleAlong=2, scaleAcross=16, octaves=5)
    pool = m.add("clouds", name="Pool", scale=4, octaves=6)
    cracks = m.add("cells", name="DryCracks", scale=18, mode="crackle", width=0.01, jitter=1.0)
    M = m.px("""
float dr = c.sample(uv);
float sm = smoothstep(0.62 - p.x * 0.2, 0.75 - p.x * 0.2, texToFloat(a.sample(uv))) * p.x;
float pl = texToFloat(b.sample(uv));
float thick = saturate(max(dr, sm * 0.6) + smoothstep(0.55, 0.8, pl) * 0.5);
return float4(thick, pl, d.sample(uv) * smoothstep(0.4, 0.9, p.y), 1.0);""", name="Blood", a=smear, b=pool, c=drops["out"], d=cracks, x=P["wear"], y=P["age"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 fresh = lerp({col("#7a1010")}, {col("#4e0a0a")}, saturate(s.x * 0.8 + (s.y - 0.5) * 0.4 * (0.5 + p.z)));
float3 dried = lerp({col("#5a2a1e")}, {col("#40201a")}, s.x);
float3 c0 = lerp(fresh, dried, p.y);
c0 = lerp(c0, {col("#3a1c16")}, s.z * 0.6);
return float4(c0, 1.0);""", name="BaseColor", a=M, y=P["age"], z=P["var"])
    r = m.gray("lerp(0.12, 0.6, p.y) + a.sample(uv).z * 0.2", name="Roughness", a=M, y=P["age"])
    h = m.gray("0.5 + a.sample(uv).x * 0.03 * (1.0 - p.y * 0.5)", name="Height", a=M, y=P["age"])
    m.finish(base, r, 0.0, h, normal=0.6, ao=(0.005, 0.01, 0.2), hue=P["hue"])
