"""Metals. Base colours are measured reflectances (F0) as sRGB; tarnish, oxide, paint and grime are separate layers."""

from common import material, std, reveal, grime_noise
from matgraph import col, f

# Measured F0, sRGB-encoded.
STEEL = "#c6c8c9"
IRON = "#c4c7c7"
ALUMINIUM = "#f2f3f3"
CHROME = "#c4c5c5"
COPPER = "#fad0c0"
BRASS = "#f6e2ae"
GOLD = "#ffe29b"
ZINC = "#d6dadc"
GRIME = "#4a4139"


def metal_surface(m, P, color, tarnish, rough=(0.3, 0.45), aniso=False, mottle=0.06, scratch=1.0, pits=0.0, normal=0.3, tarnish_metal=1.0, grain=0.01):
    """A bare metal: mottled colour and roughness, fine scratches (wear), tarnish and grime (age), optional pitting."""
    mott = m.add("clouds", name="Mottle", scale=3, octaves=6, persistence=0.55)
    if aniso:
        fine = m.add("anisotropic", name="Brushing", scaleX=3, scaleY=220, octaves=4, persistence=0.6)
    else:
        fine = m.add("perlin", name="Fine", scale=24, octaves=4, persistence=0.5)
    scr = m.add("scratches", name="Scratches", scale=5, count=4, length=0.7, width=0.0012, angle=0.0 if aniso else 25.0, angleRandom=0.15 if aniso else 1.0)
    scr2 = m.add("scratches", name="FineScratches", scale=11, count=6, length=0.35, width=0.0007, angle=70.0, angleRandom=1.0)
    M = m.px(f"""
float s = max(c.sample(uv), d.sample(uv) * 0.7) * {f(scratch)};
float worn = saturate(s * smoothstep(0.0, 0.6, p.x) * 1.6);
return float4(a.sample(uv).r, b.sample(uv).r, worn, 1.0);""", name="Surface", a=mott, b=fine, c=scr, d=scr2, x=P["wear"])
    grime = grime_noise(m)
    vor = m.add("voronoi", name="Pits", scale=28, jitter=0.9)
    T = m.px(f"""
float g = texToFloat(a.sample(uv));
float t = texToFloat(b.sample(uv));
float pit = (1.0 - smoothstep(0.05, 0.2 + d.sample(uv) * 0.2, c.sample(uv))) * step(0.82, d.sample(uv)) * {f(pits)};
float grime = {reveal("g", "p.x * 0.7", 0.25)};
float tarnish = {reveal("1.0 - t", "p.x", 0.35)};
return float4(grime, tarnish, pit, 1.0);""", name="Age", a=grime, b=mott, c=vor["f1"], d=vor["random"], x=P["age"])
    base = m.px(f"""
float4 s = a.sample(uv);
float4 g = b.sample(uv);
float3 c0 = {col(color)} * (1.0 + (s.x - 0.5) * {f(mottle * 2)} * (0.5 + p.z)) * (1.0 + (s.y - 0.5) * 0.06);
c0 = lerp(c0, {col(tarnish)}, g.y * 0.85);
c0 = lerp(c0, {col(color)}, s.z * 0.8);
c0 = lerp(c0, c0 * 0.8, g.z);
c0 = lerp(c0, {col(GRIME)}, g.x);
return float4(saturate(c0), 1.0);""", name="BaseColor", a=M, b=T, z=P["var"])
    lo, hi = rough
    r = m.px(f"""
float4 s = a.sample(uv);
float4 g = b.sample(uv);
float r = lerp({f(lo)}, {f(hi)}, saturate(s.x * 0.6 + s.y * 0.6 - 0.1));
r = lerp(r, r + 0.15, g.y);
r = lerp(r, max(r - 0.08, 0.04), s.z);
r = lerp(r, 0.7, g.z);
r = lerp(r, 0.85, g.x);
return float4(r, r, r, 1.0);""", name="Roughness", a=M, b=T)
    metal = m.px(f"""
float4 g = b.sample(uv);
float v = 1.0 - smoothstep(0.45, 0.55, g.x) - smoothstep(0.55, 0.65, g.y) * {f(1.0 - tarnish_metal)};
v = saturate(v);
return float4(v, v, v, 1.0);""", name="MetalMask", b=T)
    h = m.px(f"""
float4 s = a.sample(uv);
float4 g = b.sample(uv);
float h = 0.5 + (s.y - 0.5) * {f(grain)} - s.z * 0.06 - g.z * 0.12 + g.x * 0.015;
return float4(h, h, h, 1.0);""", name="Height", a=M, b=T)
    m.finish(base, r, metal, h, normal=normal, ao=(0.01, 0.05, 0.6))


@material("SteelBare", "Bare Steel", "Metal/Steel", "metal", 100, "Clean mild steel: soft mottling, fine handling scratches, tarnish and grime as it ages.", ["steel", "metal", "bare", "industrial"])
def steel_bare(m):
    P = std(m, wear=0.35, age=0.15, hue=False, age_doc="Tarnish and grime.")
    metal_surface(m, P, STEEL, "#9c9893", rough=(0.3, 0.45))


@material("SteelBrushed", "Brushed Steel", "Metal/Steel", "metal", 50, "Stainless steel with a directional brushed finish: streaked roughness along u, crisp handling scratches.", ["steel", "stainless", "brushed", "metal", "anisotropic"])
def steel_brushed(m):
    P = std(m, wear=0.25, age=0.05, hue=False, age_doc="Fingerprints and grime.")
    metal_surface(m, P, "#cbcdcd", "#a8a7a3", rough=(0.2, 0.4), aniso=True, mottle=0.03, normal=0.25)


@material("Iron", "Cast Iron", "Metal/Iron", "metal", 60, "Rough cast iron: pitted, grainy, with dark mill-scale patches and grime in the pores.", ["iron", "cast", "metal", "rough", "pitted"])
def iron(m):
    P = std(m, wear=0.2, age=0.45, hue=False, age_doc="Mill scale, tarnish and grime.")
    metal_surface(m, P, IRON, "#5a5652", rough=(0.5, 0.75), mottle=0.06, pits=1.0, normal=0.5, tarnish_metal=0.2, grain=0.12)


@material("Aluminium", "Aluminium", "Metal/Aluminium", "metal", 80, "Rolled aluminium sheet: bright and soft-sheened, with dull oxide blooms as it ages.", ["aluminium", "aluminum", "metal", "sheet"])
def aluminium(m):
    P = std(m, wear=0.3, age=0.15, hue=False, age_doc="Dull oxide bloom and grime.")
    metal_surface(m, P, ALUMINIUM, "#c9c9c6", rough=(0.28, 0.42), mottle=0.03, tarnish_metal=0.6)


@material("AluminiumBrushed", "Brushed Aluminium", "Metal/Aluminium", "metal", 40, "Brushed aluminium: fine directional streaks, as on appliances and machined panels.", ["aluminium", "aluminum", "brushed", "metal", "anisotropic"])
def aluminium_brushed(m):
    P = std(m, wear=0.2, age=0.05, hue=False, age_doc="Dull oxide bloom and grime.")
    metal_surface(m, P, "#eceded", "#c6c6c3", rough=(0.18, 0.36), aniso=True, mottle=0.02, normal=0.2, tarnish_metal=0.6)


@material("Chrome", "Chrome", "Metal/Chrome", "metal", 30, "Polished chrome plating: near-mirror, with faint wipe marks, water spots and the odd scratch.", ["chrome", "polished", "mirror", "metal", "plating"])
def chrome(m):
    P = std(m, wear=0.15, age=0.1, hue=False, wear_doc="Fine scratches.", age_doc="Water spots and grime.")
    metal_surface(m, P, CHROME, "#a9a9a6", rough=(0.03, 0.09), mottle=0.01, scratch=0.6, normal=0.12)


@material("Gold", "Gold", "Metal/Gold", "metal", 20, "Polished yellow gold: warm, smooth, softly hammered, with light wear.", ["gold", "precious", "metal", "jewellery"])
def gold(m):
    P = std(m, wear=0.2, age=0.05, hue=False, age_doc="Dulling and grime.")
    metal_surface(m, P, GOLD, "#d9b46a", rough=(0.12, 0.25), mottle=0.03, scratch=0.7, normal=0.2)


@material("Brass", "Brass", "Metal/Brass", "metal", 40, "Brass with a warm tarnish that creeps in as it ages; handled spots stay bright.", ["brass", "metal", "tarnish", "fittings"])
def brass(m):
    P = std(m, wear=0.3, age=0.35, hue=False, age_doc="Brown tarnish and grime.")
    metal_surface(m, P, BRASS, "#a88a52", rough=(0.25, 0.4), mottle=0.05, normal=0.25, tarnish_metal=0.7)


@material("Copper", "Copper", "Metal/Copper", "metal", 40, "Copper sheet: rosy when polished, darkening to brown tarnish with age.", ["copper", "metal", "tarnish"])
def copper(m):
    P = std(m, wear=0.3, age=0.35, hue=False, age_doc="Brown tarnish and grime.")
    metal_surface(m, P, COPPER, "#b07658", rough=(0.25, 0.42), mottle=0.06, normal=0.25, tarnish_metal=0.6)


@material("CopperPatina", "Copper Patina", "Metal/Copper", "metal", 60, "Weathered copper: brown tarnish under a crusty blue-green verdigris that gathers in low areas and streaks.", ["copper", "patina", "verdigris", "weathered", "metal"])
def copper_patina(m):
    P = std(m, wear=0.25, age=0.6, hue=False, wear_doc="Rubbed-back spots showing copper.", age_doc="Verdigris coverage.")
    clouds = m.add("clouds", name="Blotches", scale=4, octaves=7, persistence=0.6, mode="billow")
    streak = m.add("directional", name="Streaks", angle=90.0, scaleAlong=3, scaleAcross=18, octaves=5)
    crust = m.add("worley", name="Crust", scale=20, octaves=3, persistence=0.5)
    fine = m.add("perlin", name="Grain", scale=48, octaves=3)
    M = m.px("""
float bl = texToFloat(a.sample(uv));
float st = texToFloat(b.sample(uv));
float cov = bl * 0.8 + st * 0.22 + (d.sample(uv) - 0.5) * 0.25;
float pat = smoothstep(0.8 - p.x * 0.9, 0.8 - p.x * 0.9 + 0.2, cov);
float rub = smoothstep(0.62, 0.7, d.sample(uv) * 0.5 + bl * 0.6) * p.y;
pat *= 1.0 - rub;
float crust = smoothstep(0.1, 0.7, 1.0 - c.sample(uv)) * pat;
return float4(pat, crust, rub, st);""", name="Patina", a=clouds, b=streak, c=crust, d=fine, x=P["age"], y=P["wear"])
    base = m.px(f"""
float4 s = a.sample(uv);
float g = texToFloat(b.sample(uv));
float3 cu = lerp(lerp({col("#c08a70")}, {col("#d8a88e")}, g), {col("#f0c4ae")}, s.z);
float3 v = lerp({col("#5e9483")}, {col("#9cc4b0")}, saturate(s.y * 1.3 + (g - 0.5) * 0.8 * (0.5 + p.z)));
v = lerp(v, {col("#456f62")}, saturate((0.45 - s.w) * 1.2));
v = lerp(v, {col("#7a8a6a")}, (1.0 - s.x) * 0.5);
return float4(lerp(cu, v, s.x), 1.0);""", name="BaseColor", a=M, b=fine, z=P["var"])
    r = m.gray("lerp(0.38, 0.88, a.sample(uv).x) - a.sample(uv).y * 0.05", name="Roughness", a=M)
    met = m.gray("1.0 - smoothstep(0.42, 0.5, a.sample(uv).x)", name="MetalMask", a=M)
    h = m.gray("0.5 + a.sample(uv).x * 0.06 + a.sample(uv).y * 0.05", name="Height", a=M)
    m.finish(base, r, met, h, normal=0.6, ao=(0.02, 0.06, 0.8))


def rust_material(m, P, flakes, pits):
    spread = m.add("clouds", name="Spread", scale=4, octaves=7, persistence=0.6)
    spots = m.add("worley", name="Spots", scale=9, octaves=3, persistence=0.55, invert=True)
    cells = m.add("cells", name="Flakes", scale=22, mode="cells", jitter=1.0)
    wobble = m.add("perlin", name="FlakeWarp", scale=8, octaves=4)
    flake = m.add("warp", name="FlakeShape", input=cells["out"], warp=wobble, mode="gradient", intensity=1.5)
    grain = m.add("perlin", name="Grain", scale=40, octaves=5, persistence=0.6)
    M = m.px(f"""
float s = texToFloat(a.sample(uv)) * 0.65 + texToFloat(b.sample(uv)) * 0.5 - 0.1;
float t = 1.0 - p.x * 1.15;
float cov = smoothstep(t, t + 0.1, s);
float deep = smoothstep(t + 0.08, t + 0.4, s);
float fl = smoothstep(1.0 - p.y * {f(flakes)}, 1.0 - p.y * {f(flakes)} + 0.1, texToFloat(c.sample(uv))) * deep;
return float4(cov, deep, fl, d.sample(uv));""", name="Rust", a=spread, b=spots, c=flake, d=grain, x=P["age"], y=P["wear"])
    steel_mott = m.add("clouds", name="SteelMottle", scale=3, octaves=5)
    base = m.px(f"""
float4 r = a.sample(uv);
float sm = texToFloat(b.sample(uv));
float g = saturate(r.w + (sm - 0.5) * p.z);
float3 steel = {col("#b9bbbc")} * (1.0 + (sm - 0.5) * 0.1);
float3 powder = lerp({col("#7a3d1f")}, {col("#a85a2a")}, saturate(g * 1.6 - 0.35));
float3 dark = lerp({col("#3b1f14")}, {col("#5a2f1b")}, g);
float3 rust = lerp(powder, dark, saturate(r.y * 0.8 - g * 0.6 + 0.15));
rust = lerp(rust, {col("#94673a")}, smoothstep(0.72, 0.92, g) * 0.45);
float halo = r.x * (1.0 - r.x) * 4.0;
float3 c0 = lerp(steel, rust, r.x);
c0 = lerp(c0, {col("#5c4535")}, halo * 0.45);
c0 = lerp(c0, lerp({col("#3a2117")}, {col("#5e3a26")}, g), r.z);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=steel_mott, z=P["var"])
    r = m.gray("lerp(0.42, lerp(0.8, 0.96, a.sample(uv).w), a.sample(uv).x)", name="Roughness", a=M)
    met = m.gray("1.0 - smoothstep(0.36, 0.44, a.sample(uv).x)", name="MetalMask", a=M)
    h = m.px(f"""
float4 r = a.sample(uv);
float h = 0.5 + r.x * 0.03 + r.y * 0.04 * (r.w + 0.5) - r.z * 0.07 - r.y * (1.0 - r.w) * {f(pits)} * 0.04;
return float4(h, h, h, 1.0);""", name="Height", a=M)
    m.finish(base, r, met, h, normal=0.7, ao=(0.02, 0.06, 0.9))


@material("RustLight", "Light Rust", "Metal/Rust", "metal", 80, "Steel just starting to rust: orange blooms and spots over a still-bright surface.", ["rust", "steel", "corrosion", "light"])
def rust_light(m):
    P = std(m, wear=0.2, age=0.4, hue=False, wear_doc="Flaking where the rust is deep.", age_doc="Rust coverage.")
    rust_material(m, P, 0.3, 0.2)


@material("RustMedium", "Rusted Steel", "Metal/Rust", "metal", 80, "Steel half taken by rust: patches of orange and brown oxide with darker deep rust in their middles.", ["rust", "steel", "corrosion"])
def rust_medium(m):
    P = std(m, wear=0.4, age=0.6, hue=False, wear_doc="Flaking where the rust is deep.", age_doc="Rust coverage.")
    rust_material(m, P, 0.5, 0.5)


@material("RustHeavy", "Heavy Rust", "Metal/Rust", "metal", 80, "Steel lost under thick, flaking, pitted rust; only scraps of metal show.", ["rust", "steel", "corrosion", "heavy", "flaking"])
def rust_heavy(m):
    P = std(m, wear=0.45, age=0.95, hue=False, wear_doc="Flaking where the rust is deep.", age_doc="Rust coverage.")
    rust_material(m, P, 0.8, 1.0)


@material("SteelPainted", "Painted Steel", "Metal/Steel", "metal", 100, "Industrial enamel over steel: orange-peel paint that chips at wear to bare and rusting metal.", ["steel", "paint", "painted", "chipped", "industrial"])
def steel_painted(m):
    P = std(m, wear=0.35, age=0.25, wear_doc="Paint chipped back to steel.", age_doc="Grime and rust in the chips.")
    paint = m.param("paintColor", "color", "#3b5d73", None, None, "Paint Colour", "The enamel's colour (sRGB).")
    chips = m.add("perlin", name="Chips", scale=6, octaves=7, persistence=0.62)
    edge = m.add("cells", name="ChipShape", scale=24, mode="f2MinusF1", jitter=1.0)
    peel = m.add("perlin", name="OrangePeel", scale=96, octaves=2)
    grime = grime_noise(m)
    M = m.px("""
float n = texToFloat(a.sample(uv)) + c.sample(uv) * 0.18;
float t = 0.8 - p.x * 0.45;
float chip = smoothstep(t, t + 0.015, n);
float rim = smoothstep(t - 0.04, t, n) - chip;
float g = texToFloat(b.sample(uv));
float grime = smoothstep(1.0 - p.y * 0.6, 1.0 - p.y * 0.6 + 0.3, g);
return float4(chip, saturate(rim), grime, d.sample(uv));""", name="Paint", a=chips, b=grime, c=edge, d=peel, x=P["wear"], y=P["age"])
    tint = m.color(paint, name="PaintColor")
    base = m.px(f"""
float4 s = a.sample(uv);
float3 pc = b.sample(uv).rgb * (1.0 + (s.w - 0.5) * 0.06 * (0.5 + p.z));
pc = lerp(pc, pc * 0.8 + {col("#3a342c")} * 0.2, s.z * 0.6);
float3 steel = lerp({col(STEEL)}, {col("#7a4a2c")}, saturate(s.z * 1.5 + s.y));
float3 c0 = lerp(pc, steel, s.x);
c0 = lerp(c0, pc * 1.15, s.y * 0.5);
return float4(saturate(c0), 1.0);""", name="BaseColor", a=M, b=tint, z=P["var"])
    r = m.gray("lerp(lerp(0.32, 0.5, a.sample(uv).w) + a.sample(uv).z * 0.3, lerp(0.35, 0.8, a.sample(uv).z), a.sample(uv).x)", name="Roughness", a=M)
    met = m.gray("step(0.5, a.sample(uv).x) * (1.0 - smoothstep(0.45, 0.55, a.sample(uv).z))", name="MetalMask", a=M)
    h = m.gray("0.5 + (1.0 - a.sample(uv).x) * 0.04 + (a.sample(uv).w - 0.5) * 0.004", name="Height", a=M)
    m.finish(base, r, met, h, normal=0.8, ao=(0.01, 0.04, 0.6), hue=P["hue"])


@material("SteelGalvanised", "Galvanised Steel", "Metal/Steel", "metal", 60, "Hot-dip galvanised steel: zinc spangle crystals of varying sheen, whitening with zinc oxide as it ages.", ["steel", "galvanised", "galvanized", "zinc", "spangle", "metal"])
def steel_galvanised(m):
    P = std(m, wear=0.2, age=0.3, hue=False, age_doc="White zinc oxide bloom.")
    span = m.add("cells", name="Spangle", scale=7, mode="cells", jitter=1.0)
    span2 = m.add("cells", name="SpangleFine", scale=19, mode="cells", jitter=1.0)
    bloom = m.add("clouds", name="Bloom", scale=4, octaves=6)
    scr = m.add("scratches", name="Scratches", scale=6, count=3, length=0.6, width=0.001, angleRandom=1.0)
    M = m.px("""
float s = texToFloat(a.sample(uv)) * 0.7 + texToFloat(b.sample(uv)) * 0.3;
float o = smoothstep(1.0 - p.x, 1.0 - p.x + 0.3, c.sample(uv));
float w = d.sample(uv) * smoothstep(0.0, 0.5, p.y);
return float4(s, o, w, 1.0);""", name="Zinc", a=span, b=span2, c=bloom, d=scr, x=P["age"], y=P["wear"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 z = {col(ZINC)} * (0.9 + s.x * 0.16 * (0.5 + p.z));
z = lerp(z, {col("#c9cac6")}, s.y * 0.9);
return float4(saturate(z), 1.0);""", name="BaseColor", a=M, z=P["var"])
    r = m.gray("lerp(lerp(0.18, 0.5, a.sample(uv).x), 0.75, a.sample(uv).y) - a.sample(uv).z * 0.1", name="Roughness", a=M)
    met = m.gray("1.0 - smoothstep(0.65, 0.75, a.sample(uv).y)", name="MetalMask", a=M)
    h = m.gray("0.5 + a.sample(uv).x * 0.01 + a.sample(uv).y * 0.01 - a.sample(uv).z * 0.04", name="Height", a=M)
    m.finish(base, r, met, h, normal=0.3, ao=(0.01, 0.03, 0.4))
