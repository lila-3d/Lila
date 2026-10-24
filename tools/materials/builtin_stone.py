"""Stone, masonry and plaster."""

from common import material, std, grime_noise
from matgraph import col, f


@material("Granite", "Granite", "Stone/Granite", "dielectric", 60, "Honed granite: interlocking feldspar, quartz and biotite grains in pink, grey and black, softly polished.", ["granite", "stone", "speckled", "countertop", "polished"])
def granite(m):
    P = std(m, wear=0.2, age=0.1, wear_doc="Dulling and fine scratches.", age_doc="Stains.")
    g1 = m.add("cells", name="Feldspar", scale=26, mode="cells", jitter=1.0)
    g2 = m.add("cells", name="Quartz", scale=67, mode="cells", jitter=1.0)
    edge = m.add("cells", name="GrainEdges", scale=26, mode="f2MinusF1", jitter=1.0)
    broad = m.add("clouds", name="Broad", scale=3, octaves=5)
    M = m.px("""
float f1 = texToFloat(a.sample(uv));
float f2 = texToFloat(b.sample(uv));
float e = c.sample(uv);
float br = d.sample(uv);
float mica = step(0.87 - (br - 0.5) * 0.15, f2);
float spar = smoothstep(0.38, 0.46, f1 + (f2 - 0.5) * 0.15);
return float4(spar, mica, f1, e);""", name="Grains", a=g1, b=g2, c=edge, d=broad)
    stains = grime_noise(m, scale=3)
    base = m.px(f"""
float4 g = a.sample(uv);
float3 quartz = lerp({col("#8f8d8a")}, {col("#b5b2ad")}, frac(g.z * 7.0));
float3 spar = lerp({col("#b88e7c")}, {col("#d3c4b8")}, frac(g.z * 3.0)) ;
spar = lerp({col("#c9c3bb")}, spar, 0.5 + p.z * 0.5);
float3 c0 = lerp(quartz, spar, g.x);
c0 = lerp(c0, {col("#2b2826")}, g.y);
c0 *= 0.94 + smoothstep(0.0, 0.08, g.w) * 0.06;
float st = smoothstep(1.0 - p.y * 0.5, 1.2 - p.y * 0.5, texToFloat(b.sample(uv)));
c0 = lerp(c0, c0 * {col("#c8b89c")}, st);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=stains, z=P["var"], y=P["age"])
    scr = m.add("scratches", name="Scratches", scale=6, count=3, length=0.5, width=0.001, angleRandom=1.0)
    r = m.gray("0.18 + a.sample(uv).y * 0.12 + frac(a.sample(uv).z * 5.0) * 0.06 + c.sample(uv) * p.x * 0.3 + p.x * 0.25", name="Roughness", a=M, c=scr, x=P["wear"])
    h = m.gray("0.5 + smoothstep(0.0, 0.06, a.sample(uv).w) * 0.004 - c.sample(uv) * p.x * 0.01", name="Height", a=M, c=scr, x=P["wear"])
    m.finish(base, r, 0.0, h, normal=0.5, ao=(0.01, 0.02, 0.3), hue=P["hue"])


@material("Marble", "White Marble", "Stone/Marble", "dielectric", 120, "Polished white marble with soft grey clouding and branching veins.", ["marble", "stone", "white", "veined", "polished"])
def marble(m):
    P = std(m, wear=0.15, age=0.1, wear_doc="Etching and dulling.", age_doc="Yellowing and stains.")
    n = m.add("perlin", name="VeinNoise", scale=2, octaves=4, persistence=0.45)
    wn = m.add("perlin", name="WarpNoise", scale=3, octaves=6, persistence=0.55)
    n2 = m.add("perlin", name="FineVeins", scale=4, octaves=4, persistence=0.5)
    cloud = m.add("clouds", name="Clouding", scale=4, octaves=6)
    M = m.px("""
float2 o = float2(texToFloat(b.sample(uv)), texToFloat(b.sample(uv + float2(0.31, 0.57)))) - 0.5;
float v1 = texToFloat(a.sample(uv + o * 0.45));
float v2 = c.sample(uv + o * 0.3);
float cl = d.sample(uv);
float main = 1.0 - smoothstep(0.0, 0.006 + cl * 0.01, abs(v1 - 0.5));
float halo = 1.0 - smoothstep(0.0, 0.05, abs(v1 - 0.5));
float fine = (1.0 - smoothstep(0.0, 0.004, abs(v2 - 0.53))) * smoothstep(0.25, 0.5, cl) * 0.8;
return float4(main, halo, fine, cl);""", name="Veins", a=n, b=wn, c=n2, d=cloud)
    base = m.px(f"""
float4 v = a.sample(uv);
float3 c0 = lerp({col("#e9e7e2")}, {col("#d0cfcb")}, saturate((v.w - 0.35) * 1.4 * (0.4 + p.z)));
c0 = lerp(c0, {col("#b9b9b7")}, v.y * 0.5);
c0 = lerp(c0, {col("#6f727a")}, v.x * 0.85);
c0 = lerp(c0, {col("#8e9096")}, v.z * 0.7);
c0 = lerp(c0, c0 * {col("#f2e6cc")}, p.y * 0.6);
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"], y=P["age"])
    r = m.gray("0.08 + a.sample(uv).x * 0.05 + p.x * 0.35 * (0.5 + a.sample(uv).w)", name="Roughness", a=M, x=P["wear"])
    h = m.gray("0.5 - a.sample(uv).x * 0.002", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.3, ao=(0.01, 0.02, 0.2), hue=P["hue"])


@material("Sandstone", "Sandstone", "Stone/Sandstone", "dielectric", 100, "Bedded sandstone: warm sandy layers, the soft ones weathered back, gritty grain and pitting.", ["sandstone", "stone", "layered", "sedimentary", "warm"])
def sandstone(m):
    P = std(m, wear=0.4, age=0.25, wear_doc="Erosion of the soft beds.", age_doc="Dark weathering crust and lichen.")
    beds = m.add("directional", name="Beds", angle=0.0, scaleAlong=2, scaleAcross=10, octaves=6, persistence=0.6)
    wn = m.add("perlin", name="BedWarp", scale=3, octaves=4)
    bedsW = m.add("warp", name="BedsWarped", input=beds, warp=wn, mode="gradient", intensity=2.0)
    grit = m.add("perlin", name="Grit", scale=96, octaves=2)
    pits = m.add("worley", name="Pits", scale=18, octaves=2, invert=False)
    M = m.px("""
float b0 = texToFloat(a.sample(uv));
float soft = smoothstep(0.35, 0.65, b0);
float pit = smoothstep(0.12, 0.0, d.sample(uv)) * 0.7;
return float4(b0, soft, c.sample(uv), pit);""", name="Bedding", a=bedsW, c=grit, d=pits)
    weather = grime_noise(m, scale=3)
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#b88d5e")}, {col("#d6bb92")}, s.x);
c0 = lerp(c0, {col("#a8714e")}, smoothstep(0.6, 0.85, frac(s.x * 3.7)) * 0.5 * (0.4 + p.z));
c0 *= 0.92 + s.z * 0.16;
float wt = smoothstep(1.0 - p.y * 0.7, 1.2 - p.y * 0.7, texToFloat(b.sample(uv)));
c0 = lerp(c0, c0 * 0.55, wt * 0.8);
c0 = lerp(c0, c0 * 0.8, s.w);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=weather, z=P["var"], y=P["age"])
    r = m.gray("0.82 + a.sample(uv).z * 0.1", name="Roughness", a=M)
    h = m.gray("0.5 - a.sample(uv).y * 0.08 * (0.5 + p.x) + (a.sample(uv).z - 0.5) * 0.02 - a.sample(uv).w * 0.06", name="Height", a=M, x=P["wear"])
    m.finish(base, r, 0.0, h, normal=1.0, ao=(0.02, 0.08, 0.8), hue=P["hue"])


@material("Slate", "Natural Slate", "Stone/Slate", "dielectric", 80, "Riven slate: blue-grey cleft layers stepping across the face, with faint rusty mineral staining.", ["slate", "stone", "riven", "cleft", "dark"])
def slate(m):
    P = std(m, wear=0.3, age=0.2, wear_doc="Chipped cleft edges.", age_doc="Mineral staining.")
    cl = m.add("crystal", name="Cleft", scale=4, octaves=3, persistence=0.45)
    streak = m.add("directional", name="Grain", angle=20.0, scaleAlong=2, scaleAcross=30, octaves=4)
    stain = m.add("clouds", name="Stain", scale=3, octaves=6)
    M = m.px("""
float k = texToFloat(a.sample(uv));
float steps = floor(k * 6.0) / 6.0;
float edge = smoothstep(0.0, 0.05, frac(k * 6.0));
return float4(steps, edge, texToFloat(b.sample(uv)), c.sample(uv));""", name="Layers", a=cl, b=streak, c=stain)
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#3b4048")}, {col("#545a63")}, saturate(s.x * 0.8 + (s.z - 0.5) * 0.3));
c0 = lerp(c0, {col("#6a6158")}, smoothstep(0.55, 0.8, s.w) * p.y * (0.5 + p.z));
c0 = lerp(c0 * 1.2, c0, s.y);
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"], y=P["age"])
    r = m.gray("0.6 + (a.sample(uv).z - 0.5) * 0.15 + (1.0 - a.sample(uv).y) * 0.15", name="Roughness", a=M)
    h = m.gray("0.4 + a.sample(uv).x * 0.15 + (a.sample(uv).z - 0.5) * 0.01", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=1.2, ao=(0.02, 0.06, 0.6), hue=P["hue"])


@material("Cobbles", "Cobblestones", "Stone/Cobbles", "dielectric", 150, "Rounded cobbles set in sandy joints: each stone its own grey-brown, polished tops, grit and moss in the gaps.", ["cobbles", "cobblestone", "street", "paving", "stone"])
def cobbles(m):
    P = std(m, wear=0.4, age=0.3, wear_doc="Polish on the stone tops.", age_doc="Moss and dirt in the joints.")
    cells = m.add("cells", name="Stones", scale=7, mode="f2MinusF1", jitter=0.85)
    ids = m.add("cells", name="StoneId", scale=7, mode="cells", jitter=0.85)
    lump = m.add("perlin", name="Lumps", scale=20, octaves=4)
    joint = m.add("dirt", name="JointFill", scale=12, amount=0.5, contrast=2.0, grain=0.8)
    M = m.px("""
float e = c.sample(uv);
float id = d.sample(uv);
float l = texToFloat(a.sample(uv));
float dome = sqrt(saturate(e / 0.32)) * (0.85 + id * 0.15) + (l - 0.5) * 0.12;
float stone = smoothstep(0.02, 0.07, e + (l - 0.5) * 0.03);
return float4(dome, stone, id, l);""", name="Shape", a=lump, c=cells, d=ids)
    base = m.px(f"""
float4 s = a.sample(uv);
float j = texToFloat(b.sample(uv));
float3 st = lerp({col("#6e6a64")}, {col("#9a948b")}, frac(s.z * 3.1));
st = lerp(st, {col("#857563")}, step(0.7, frac(s.z * 7.3)) * (0.3 + p.z * 0.6));
st *= 0.85 + s.w * 0.3;
st = lerp(st, st * 1.15, smoothstep(0.7, 1.0, s.x) * p.x);
float3 jc = lerp({col("#6a5f50")}, {col("#9c8f78")}, j);
float moss = smoothstep(0.3, 0.9, j) * smoothstep(0.0, 1.0, p.y);
jc = lerp(jc, {col("#4c5a2c")}, moss);
float3 c0 = lerp(jc, st, s.y);
c0 = lerp(c0, c0 * 0.7, (1.0 - smoothstep(0.0, 0.25, s.x)) * s.y * 0.6);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=joint, z=P["var"], x=P["wear"], y=P["age"])
    r = m.gray("lerp(0.92, lerp(0.75, 0.45, smoothstep(0.6, 1.0, a.sample(uv).x) * p.x), a.sample(uv).y)", name="Roughness", a=M, x=P["wear"])
    h = m.gray("lerp(0.15 + texToFloat(b.sample(uv)) * 0.05, 0.25 + a.sample(uv).x * 0.6, a.sample(uv).y)", name="Height", a=M, b=joint)
    m.finish(base, r, 0.0, h, normal=1.2, ao=(0.04, 0.25, 1.0), hue=P["hue"])


@material("Bricks", "Red Bricks", "Stone/Bricks", "dielectric", 100, "Fired clay bricks in running bond: each brick its own red-brown, sand-faced, chipped arrises, recessed lime mortar.", ["bricks", "brick", "wall", "masonry", "red"])
def bricks(m):
    P = std(m, wear=0.3, age=0.25, wear_doc="Chipped brick edges.", age_doc="Soot, grime and white efflorescence.")
    b = m.add("bricks", name="Bond", rows=13, columns=4, offset=0.5, mortar=0.09, bevel=0.06)
    chip = m.add("perlin", name="Chip", scale=40, octaves=4, persistence=0.6)
    sand = m.add("perlin", name="Sand", scale=128, octaves=2)
    M = m.px("""
float mask = texToFloat(a.sample(uv));
float ch = texToFloat(b.sample(uv));
float face = smoothstep(0.0, 0.6, mask - (ch - 0.5) * (0.5 + p.x * 1.2) * (1.0 - mask));
return float4(face, mask, c.sample(uv), d.sample(uv));""", name="Faces", a=b["mask"], b=chip, c=b["random"], d=sand, x=P["wear"])
    grime = grime_noise(m, scale=4)
    base = m.px(f"""
float4 s = a.sample(uv);
float r = s.z;
float3 br = lerp({col("#8c3f2c")}, {col("#a85a3e")}, frac(r * 3.3));
br = lerp(br, {col("#6e3326")}, step(0.82, frac(r * 5.7)) * 0.8);
br = lerp(br, {col("#b4785a")}, step(0.88, frac(r * 9.1)) * 0.6);
br = lerp(lerp({col("#9a4a34")}, {col("#9a4a34")}, 0.5), br, 0.4 + p.z * 0.6);
br *= 0.88 + s.w * 0.2;
float3 mo = {col("#b4ac9e")} * (0.9 + s.w * 0.15);
float3 c0 = lerp(mo, br, s.x);
float g = texToFloat(b.sample(uv));
c0 = lerp(c0, c0 * 0.55, smoothstep(1.0 - p.y * 0.6, 1.2 - p.y * 0.6, g) * 0.8);
float eff = smoothstep(0.7, 0.9, g * (1.0 - s.x * 0.5)) * p.y;
c0 = lerp(c0, {col("#d8d4cc")}, eff * 0.5);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=grime, z=P["var"], y=P["age"])
    r = m.gray("lerp(0.92, 0.8, a.sample(uv).x) + (a.sample(uv).w - 0.5) * 0.08", name="Roughness", a=M)
    h = m.gray("0.3 + a.sample(uv).x * 0.25 + (a.sample(uv).w - 0.5) * 0.02 + (c.sample(uv) - 0.5) * 0.02 * a.sample(uv).x", name="Height", a=M, c=chip)
    m.finish(base, r, 0.0, h, normal=1.0, ao=(0.02, 0.1, 0.9), hue=P["hue"])


@material("Concrete", "Cast Concrete", "Stone/Concrete", "dielectric", 200, "Board-free cast concrete: cloudy grey, pinhole air voids, a few exposed aggregate patches and rain streaks.", ["concrete", "cement", "grey", "urban", "brutalist"])
def concrete(m):
    P = std(m, wear=0.25, age=0.3, wear_doc="Exposed aggregate where the skin wore off.", age_doc="Rain streaks and stains.")
    cloud = m.add("clouds", name="Blotches", scale=4, octaves=7, persistence=0.58)
    voids = m.add("worley", name="Voids", scale=40, octaves=1)
    agg = m.add("cells", name="Aggregate", scale=60, mode="cells", jitter=1.0)
    patch = m.add("perlin", name="WearPatches", scale=5, octaves=6, persistence=0.6)
    M = m.px("""
float cl = texToFloat(a.sample(uv));
float v = 1.0 - smoothstep(0.0, 0.07, c.sample(uv));
float hole = v * step(0.6, texToFloat(b.sample(uv + 0.3)));
float pa = smoothstep(0.82 - p.x * 0.3, 0.86 - p.x * 0.3, d.sample(uv));
return float4(cl, hole, pa, texToFloat(b.sample(uv)));""", name="Surface", a=cloud, b=agg, c=voids, d=patch, x=P["wear"])
    streak = m.add("directional", name="Streaks", angle=90.0, scaleAlong=2, scaleAcross=36, octaves=4)
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = {col("#8f8d88")} * (0.9 + (s.x - 0.5) * 0.25 * (0.5 + p.z) + 0.1);
float3 ag = lerp({col("#6e6a64")}, {col("#a9a49a")}, s.w);
c0 = lerp(c0, ag, s.z * 0.85);
c0 = lerp(c0, c0 * 0.6, s.y);
float st = smoothstep(0.55, 0.8, texToFloat(b.sample(uv))) * p.y;
c0 = lerp(c0, c0 * 0.72, st);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=streak, z=P["var"], y=P["age"])
    r = m.gray("0.82 + (a.sample(uv).x - 0.5) * 0.1 + a.sample(uv).z * 0.06", name="Roughness", a=M)
    h = m.gray("0.5 + (a.sample(uv).x - 0.5) * 0.02 - a.sample(uv).y * 0.08 + a.sample(uv).z * (a.sample(uv).w - 0.5) * 0.06", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.8, ao=(0.01, 0.04, 0.6), hue=P["hue"])


@material("Plaster", "Lime Plaster", "Stone/Plaster", "dielectric", 150, "Hand-trowelled lime plaster: soft sweeping trowel marks, chalky off-white, hairline cracks as it ages.", ["plaster", "wall", "lime", "render", "trowel", "interior"])
def plaster(m):
    P = std(m, wear=0.2, age=0.3, wear_doc="Chips back to the base coat.", age_doc="Hairline cracks and discolouration.")
    trowel = m.add("gabor", name="Trowel", scale=3, impulses=6, frequency=0.8, angleRandom=1.0)
    soft = m.add("clouds", name="Soft", scale=3, octaves=6)
    cracks = m.add("cells", name="Cracks", scale=5, mode="crackle", width=0.012, jitter=1.0)
    crackW = m.add("warp", name="CrackWarp", input=cracks, warp=soft, mode="gradient", intensity=0.6)
    chips = m.add("perlin", name="Chips", scale=10, octaves=6, persistence=0.62)
    M = m.px("""
float tr = texToFloat(a.sample(uv));
float sf = texToFloat(b.sample(uv));
float ck = texToFloat(c.sample(uv)) * smoothstep(0.65 - p.y * 0.4, 0.85 - p.y * 0.4, sf);
float chip = smoothstep(0.82 - p.x * 0.25, 0.84 - p.x * 0.25, d.sample(uv));
return float4(tr, sf, ck, chip);""", name="Surface", a=trowel, b=soft, c=crackW, d=chips, x=P["wear"], y=P["age"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = {col("#d9d2c4")} * (0.97 + (s.y - 0.5) * 0.12 * (0.4 + p.z) + (s.x - 0.5) * 0.05);
c0 = lerp(c0, c0 * {col("#e8dcc0")}, smoothstep(0.5, 0.9, s.y) * p.y);
c0 = lerp(c0, {col("#9a8f80")}, s.w * 0.8);
c0 = lerp(c0, c0 * 0.72, s.z);
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"], y=P["age"])
    r = m.gray("0.82 + (a.sample(uv).x - 0.5) * 0.12", name="Roughness", a=M)
    h = m.gray("0.5 + (a.sample(uv).x - 0.5) * 0.04 + (a.sample(uv).y - 0.5) * 0.03 - a.sample(uv).z * 0.04 - a.sample(uv).w * 0.08", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.8, ao=(0.01, 0.04, 0.5), hue=P["hue"])


@material("CeramicTiles", "Ceramic Tiles", "Stone/Tiles", "dielectric", 60, "Glazed square wall tiles: glossy glaze varying tile to tile, each set slightly off-true, cementitious grout.", ["tiles", "ceramic", "glazed", "bathroom", "kitchen", "grout"])
def ceramic_tiles(m):
    P = std(m, wear=0.2, age=0.25, wear_doc="Chipped tile edges and crazing.", age_doc="Dirty grout.")
    glaze = m.param("glazeColor", "color", "#d9d5cb", None, None, "Glaze Colour", "The glaze's colour (sRGB).")
    t = m.add("tiles", name="Grid", countX=5, countY=5, gap=0.035, bevel=0.06)
    craze = m.add("cells", name="Crazing", scale=14, mode="crackle", width=0.004, jitter=1.0)
    rip = m.add("perlin", name="GlazeRipple", scale=16, octaves=3)
    dirt = grime_noise(m, scale=6)
    M = m.px("""
float mask = texToFloat(a.sample(uv));
float3 tu = b.sample(uv).xyz;
float r = c.sample(uv);
float tilt = dot(tu.xy - 0.5, float2(cos(r * 40.0), sin(r * 40.0))) * 0.03;
return float4(mask, r, tilt, 1.0);""", name="Tiles", a=t["mask"], b=t["tileUv"], c=t["random"])
    tint = m.color(glaze, name="GlazeColor")
    base = m.px(f"""
float4 s = a.sample(uv);
float3 gl = b.sample(uv).rgb * (1.0 + (s.y - 0.5) * 0.08 * (0.3 + p.z * 1.4));
float g = d.sample(uv);
float3 grout = lerp({col("#a39d92")}, {col("#6e675c")}, smoothstep(0.3, 0.9, g) * p.y);
float3 c0 = lerp(grout, gl, smoothstep(0.2, 0.5, s.x));
c0 = lerp(c0, c0 * 0.85, c.sample(uv) * p.x);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=tint, c=craze, d=dirt, z=P["var"], y=P["age"], x=P["wear"])
    r = m.gray("lerp(0.9, 0.08 + (texToFloat(b.sample(uv)) - 0.5) * 0.04, smoothstep(0.3, 0.6, a.sample(uv).x))", name="Roughness", a=M, b=rip)
    h = m.gray("0.3 + a.sample(uv).x * 0.2 + a.sample(uv).z * a.sample(uv).x + (texToFloat(b.sample(uv)) - 0.5) * 0.004 * a.sample(uv).x - c.sample(uv) * p.x * 0.004", name="Height", a=M, b=rip, c=craze, x=P["wear"])
    m.finish(base, r, 0.0, h, normal=0.8, ao=(0.01, 0.05, 0.7), hue=P["hue"])
