"""Ground: soils, aggregates, vegetation and frozen surfaces. Wear is trampling and scuffing, age is wetness and debris."""

from common import material, std, grime_noise
from matgraph import col, f

BLADE = """
float2 q = uv - 0.5;
float t = saturate(q.y + 0.5);
float bend = q.x + (t * t) * 0.12;
float w = 0.085 * (1.0 - t * 0.9);
float v = (1.0 - smoothstep(w * 0.55, w, abs(bend))) * step(-0.5, q.y) * (1.0 - step(0.48, q.y));
float h = v * (0.35 + t * 0.65);
return float4(h, h, h, 1.0);"""

LEAF = """
float2 q = (uv - 0.5) * float2(1.0, 1.0);
float y = q.y * 2.0;
float halfw = 0.3 * sqrt(saturate(1.0 - y * y)) * (1.0 - 0.25 * y);
float v = 1.0 - smoothstep(halfw - 0.03, halfw, abs(q.x));
float vein = 1.0 - smoothstep(0.0, 0.015, abs(q.x));
float h = v * (0.75 + 0.25 * (1.0 - abs(q.x) / max(halfw, 0.01)) - vein * 0.1);
return float4(h, h, h, 1.0);"""

PEBBLE = """
float2 q = uv - 0.5;
float n = texToFloat(a.sample(uv * 0.5 + 0.25));
float r = length(q) * (1.0 + (n - 0.5) * 0.6);
float v = sqrt(saturate(1.0 - (r / 0.42) * (r / 0.42)));
return float4(v, v, v, 1.0);"""


@material("Soil", "Garden Soil", "Ground/Soil", "organic", 100, "Dark crumbly soil: clods, grit and small stones, damp patches and bits of root.", ["soil", "dirt", "earth", "ground", "garden"])
def soil(m):
    P = std(m, wear=0.2, age=0.3, wear_doc="Trampled, flattened areas.", age_doc="Dampness.")
    clods = m.add("worley", name="Clods", scale=14, octaves=3, persistence=0.55, invert=True)
    big = m.add("clouds", name="Lumps", scale=4, octaves=6)
    shape = m.add("perlin", name="PebbleShape", scale=3, octaves=3)
    peb = m.px(PEBBLE, name="Pebble", a=shape)
    stones = m.add("splatter", name="Stones", pattern=peb, countX=10, countY=10, perCell=1, size=0.3, sizeRandom=0.6, valueRandom=0.3, blend="max")
    grit = m.add("perlin", name="Grit", scale=128, octaves=2)
    M = m.px("""
float cl = texToFloat(a.sample(uv));
float lp = texToFloat(b.sample(uv));
float st = c.sample(uv);
float flat = smoothstep(0.7 - p.x * 0.5, 0.9 - p.x * 0.5, lp);
float hgt = lerp(cl * 0.5 + lp * 0.5, lp * 0.6 + 0.2, flat);
return float4(hgt, st, d.sample(uv), flat);""", name="Surface", a=clods, b=big, c=stones["out"], d=grit, x=P["wear"])
    damp = m.add("clouds", name="Damp", scale=3, octaves=5)
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#3e2f22")}, {col("#6b5440")}, saturate(s.x * 0.9 + (s.z - 0.5) * 0.3 * (0.5 + p.z)));
c0 = lerp(c0, lerp({col("#4e463c")}, {col("#73685a")}, s.z) * (0.7 + s.y * 0.4), smoothstep(0.2, 0.4, s.y));
float dmp = smoothstep(1.0 - p.y, 1.2 - p.y, texToFloat(b.sample(uv)) + (1.0 - s.x) * 0.3);
c0 *= lerp(1.0, 0.62, dmp);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=damp, z=P["var"], y=P["age"])
    r = m.px("""
float4 s = a.sample(uv);
float dmp = smoothstep(1.0 - p.y, 1.2 - p.y, texToFloat(b.sample(uv)) + (1.0 - s.x) * 0.3);
float r = lerp(0.95, 0.55, dmp) - smoothstep(0.2, 0.4, s.y) * 0.15;
return float4(r, r, r, 1.0);""", name="Roughness", a=M, b=damp, y=P["age"])
    h = m.gray("0.2 + a.sample(uv).x * 0.4 + a.sample(uv).y * 0.25 + (a.sample(uv).z - 0.5) * 0.02", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=1.2, ao=(0.02, 0.15, 1.0), hue=P["hue"])


@material("Mud", "Wet Mud", "Ground/Mud", "organic", 150, "Churned wet mud: slumped ridges, glossy films and water pooling in the hollows.", ["mud", "wet", "ground", "dirt", "puddles"])
def mud(m):
    P = std(m, wear=0.4, age=0.5, wear_doc="Churned ruts and ridges.", age_doc="Water level in the hollows.")
    slump = m.add("clouds", name="Slump", scale=3, octaves=6, persistence=0.5)
    ruts = m.add("directional", name="Ruts", angle=10.0, scaleAlong=2, scaleAcross=8, octaves=4)
    fine = m.add("perlin", name="Fine", scale=40, octaves=3)
    M = m.px("""
float s = texToFloat(a.sample(uv));
float r = texToFloat(b.sample(uv));
float hgt = s * 0.7 + (abs(r - 0.5) * 2.0) * 0.5 * p.x + (c.sample(uv) - 0.5) * 0.05;
float water = 1.0 - smoothstep(0.2 + p.y * 0.4, 0.24 + p.y * 0.4, hgt);
return float4(hgt, water, c.sample(uv), 1.0);""", name="Surface", a=slump, b=ruts, c=fine, x=P["wear"], y=P["age"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#3a2c20")}, {col("#5e4a36")}, saturate(s.x * 1.1 + (s.z - 0.5) * 0.4 * (0.5 + p.z)));
c0 = lerp(c0, {col("#2e261e")}, s.y * 0.85);
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"])
    r = m.gray("lerp(lerp(0.35, 0.75, a.sample(uv).x), 0.04, a.sample(uv).y)", name="Roughness", a=M)
    h = m.gray("max(a.sample(uv).x, 0.2 + p.y * 0.4) * 0.5 + 0.2", name="Height", a=M, y=P["age"])
    m.finish(base, r, 0.0, h, normal=1.0, ao=(0.03, 0.1, 0.8), hue=P["hue"])


@material("Sand", "Beach Sand", "Ground/Sand", "dielectric", 120, "Fine dry sand with wind ripples, scattered darker grains and the odd shell fleck.", ["sand", "beach", "desert", "ripples", "ground"])
def sand(m):
    P = std(m, wear=0.3, age=0.15, wear_doc="Scuffed, footprinted ripples.", age_doc="Damp, darker sand.")
    wn = m.add("perlin", name="RippleWarp", scale=3, octaves=4)
    grain = m.add("perlin", name="Grain", scale=200, octaves=1)
    specks = m.add("cells", name="Specks", scale=150, mode="cells", jitter=1.0)
    scuff = m.add("clouds", name="Scuff", scale=5, octaves=5)
    M = m.px("""
float w = texToFloat(a.sample(uv));
float ph = (uv.y * 14.0 + (w - 0.5) * 3.0 + sin(uv.x * 6.2832 * 2.0 + w * 3.0) * 0.4);
float rip = pow(abs(sin(ph * 3.14159)), 1.5);
float sc = smoothstep(0.75 - p.x * 0.3, 0.9 - p.x * 0.3, texToFloat(d.sample(uv)));
rip = lerp(rip, 0.5, sc);
return float4(rip, b.sample(uv).r, c.sample(uv), sc);""", name="Ripples", a=wn, b=grain, c=specks, d=scuff, x=P["wear"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = {col("#c9b48a")} * (0.92 + s.x * 0.08 + (s.y - 0.5) * 0.12 * (0.5 + p.z));
c0 = lerp(c0, {col("#8a7660")}, step(0.975, s.z) * 0.7);
c0 = lerp(c0, {col("#e6dcc6")}, step(0.985, frac(s.z * 13.0)) * 0.6);
c0 *= 1.0 - p.y * 0.35;
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"], y=P["age"])
    r = m.gray("0.9 - p.y * 0.25 + (a.sample(uv).y - 0.5) * 0.06", name="Roughness", a=M, y=P["age"])
    h = m.gray("0.5 + a.sample(uv).x * 0.05 + (a.sample(uv).y - 0.5) * 0.01", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=1.0, ao=(0.01, 0.03, 0.4), hue=P["hue"])


@material("Gravel", "Gravel", "Ground/Gravel", "dielectric", 80, "Loose rounded gravel in mixed greys and buffs, piled over packed fines.", ["gravel", "pebbles", "stones", "ground", "path"])
def gravel(m):
    P = std(m, wear=0.2, age=0.2, wear_doc="Stones pressed into the fines.", age_doc="Dust and mud on the stones.")
    shape = m.add("perlin", name="PebbleShape", scale=3, octaves=3)
    peb = m.px(PEBBLE, name="Pebble", a=shape)
    big = m.add("splatter", name="Large", pattern=peb, countX=9, countY=9, perCell=3, size=0.75, sizeRandom=0.4, valueRandom=0.2, blend="max")
    small = m.add("splatter", name="Small", pattern=peb, countX=18, countY=18, perCell=3, size=0.7, sizeRandom=0.5, valueRandom=0.3, blend="max")
    fines = m.add("perlin", name="Fines", scale=96, octaves=2)
    M = m.px("""
float bg = c.sample(uv);
float sm = d.sample(uv);
float topBig = step(sm * 0.6, bg);
float hgt = max(bg, sm * 0.6);
float4 colr = lerp(b.sample(uv), a.sample(uv), topBig);
return float4(hgt, colr.r, colr.g, step(0.02, hgt));""", name="Stones", a=big["color"], b=small["color"], c=big["out"], d=small["out"])
    dust = grime_noise(m, scale=4)
    base = m.px(f"""
float4 s = a.sample(uv);
float k = s.y;
float3 st = lerp({col("#7d7a74")}, {col("#a59e92")}, k);
st = lerp(st, {col("#b3a088")}, step(0.65, s.z) * (0.4 + p.z * 0.6));
st = lerp(st, {col("#5a5853")}, step(0.85, frac(s.z * 5.0)) * 0.7);
st *= 0.8 + s.x * 0.25;
float3 fn = {col("#8a7d6a")} * (0.85 + texToFloat(c.sample(uv)) * 0.3);
float3 c0 = lerp(fn, st, s.w);
c0 = lerp(c0, {col("#8a7d6a")}, smoothstep(1.0 - p.y * 0.7, 1.2 - p.y * 0.7, texToFloat(b.sample(uv))) * 0.6);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=dust, c=fines, z=P["var"], y=P["age"])
    r = m.gray("lerp(0.95, 0.72 - a.sample(uv).x * 0.15, a.sample(uv).w)", name="Roughness", a=M)
    h = m.gray("0.1 + a.sample(uv).x * (0.8 - p.x * 0.4) + (texToFloat(b.sample(uv)) - 0.5) * 0.02", name="Height", a=M, b=fines, x=P["wear"])
    m.finish(base, r, 0.0, h, normal=1.5, ao=(0.02, 0.2, 1.0), hue=P["hue"])


@material("Grass", "Lawn Grass", "Ground/Grass", "organic", 60, "Dense short grass seen from above: overlapping blades in mixed greens with yellowed tips, soil glimpsed between.", ["grass", "lawn", "ground", "vegetation", "green"])
def grass(m):
    P = std(m, wear=0.2, age=0.25, wear_doc="Trampled, flattened patches.", age_doc="Dry, yellowed blades.")
    blade = m.px(BLADE, name="Blade")
    density = m.add("clouds", name="Density", scale=4, octaves=4)
    b1 = m.add("splatter", name="Blades", pattern=blade, mask=density, countX=10, countY=10, perCell=14, size=1.3, sizeRandom=0.4, rotationRandom=1.0, valueRandom=0.25, maskSize=0.3, blend="max")
    b2 = m.add("splatter", name="BladesFine", pattern=blade, countX=18, countY=18, perCell=12, size=1.3, sizeRandom=0.5, rotationRandom=1.0, valueRandom=0.3, blend="max")
    M = m.px("""
float h1 = c.sample(uv);
float h2 = d.sample(uv) * 0.7;
float top = step(h2, h1);
float hgt = max(h1, h2);
float r = lerp(b.sample(uv).r, a.sample(uv).r, top);
return float4(hgt, r, step(0.02, hgt), top);""", name="Sward", a=b1["color"], b=b2["color"], c=b1["out"], d=b2["out"])
    dry = m.add("clouds", name="Dry", scale=5, octaves=5)
    base = m.px(f"""
float4 s = a.sample(uv);
float k = s.y;
float3 g0 = lerp({col("#3f5a22")}, {col("#6f8a32")}, saturate(s.x * 1.2));
g0 = lerp(g0, {col("#58702a")}, step(0.6, k) * (0.3 + p.z * 0.7));
g0 = lerp(g0, {col("#8e8f3e")}, step(0.88, k) * (0.3 + p.z * 0.7));
float dr = smoothstep(1.0 - p.y * 0.8, 1.2 - p.y * 0.8, texToFloat(b.sample(uv)) * 0.6 + k * 0.5);
g0 = lerp(g0, lerp({col("#a39a5a")}, {col("#c2b070")}, s.x), dr);
float3 under = lerp({col("#2c3a1a")}, {col("#3a2e20")}, smoothstep(0.6, 1.0, texToFloat(b.sample(uv))) * 0.6);
float3 c0 = lerp(under, g0, s.z);
c0 *= 0.7 + s.x * 0.4;
return float4(c0, 1.0);""", name="BaseColor", a=M, b=dry, z=P["var"], y=P["age"])
    r = m.gray("lerp(0.95, 0.62 + (1.0 - a.sample(uv).x) * 0.2, a.sample(uv).z)", name="Roughness", a=M)
    h = m.gray("0.1 + a.sample(uv).x * (0.8 - p.x * 0.5)", name="Height", a=M, x=P["wear"])
    m.finish(base, r, 0.0, h, normal=1.0, ao=(0.02, 0.2, 1.0), hue=P["hue"])


@material("ForestFloor", "Forest Floor", "Ground/Forest", "organic", 120, "Leaf litter over dark humus: fallen leaves in browns and ochres, twigs, needles and moss.", ["forest", "leaves", "litter", "ground", "autumn", "woodland"])
def forest_floor(m):
    P = std(m, wear=0.2, age=0.4, wear_doc="Trodden, broken-down litter.", age_doc="Decay: darker, wetter leaves and more moss.")
    leaf = m.px(LEAF, name="Leaf")
    seg = m.add("sdfSegment", name="Twig", startX=0.1, startY=0.5, endX=0.9, endY=0.5, thickness=0.025)
    twig = m.add("sdfToMask", name="TwigMask", input=seg, mode="fill", softness=2.0)
    leaves = m.add("splatter", name="Leaves", pattern=leaf, countX=8, countY=8, perCell=5, size=0.9, sizeRandom=0.4, rotationRandom=1.0, valueRandom=0.2, blend="top")
    leaves2 = m.add("splatter", name="SmallLeaves", pattern=leaf, countX=14, countY=14, perCell=4, size=0.8, sizeRandom=0.4, rotationRandom=1.0, valueRandom=0.2, blend="top")
    twigs = m.add("splatter", name="Twigs", pattern=twig, countX=5, countY=5, perCell=2, size=0.9, sizeRandom=0.5, rotationRandom=1.0, valueRandom=0.1, blend="max")
    M = m.px("""
float l1 = c.sample(uv);
float l2 = d.sample(uv) * 0.8;
float top = step(l2, l1);
float hgt = max(l1, l2);
float k = lerp(b.sample(uv).r, a.sample(uv).r, top);
return float4(hgt, k, step(0.02, hgt), top);""", name="Litter", a=leaves["color"], b=leaves2["color"], c=leaves["out"], d=leaves2["out"])
    moss = m.add("clouds", name="Moss", scale=4, octaves=6)
    hum = m.add("perlin", name="Humus", scale=30, octaves=4)
    base = m.px(f"""
float4 s = a.sample(uv);
float k = s.y;
float3 lf = lerp({col("#6e4424")}, {col("#9a6a32")}, k);
lf = lerp(lf, {col("#a8823a")}, step(0.7, k) * (0.4 + p.z * 0.6));
lf = lerp(lf, {col("#5a4630")}, step(0.85, frac(k * 7.0)) * 0.7);
lf = lerp(lf, lf * 0.6, p.y * 0.5);
float3 hm = lerp({col("#2a2018")}, {col("#43352a")}, texToFloat(c.sample(uv)));
float mo = smoothstep(0.75 - p.y * 0.4, 0.95 - p.y * 0.4, texToFloat(b.sample(uv))) * (1.0 - s.z * 0.7);
hm = lerp(hm, {col("#4d5e26")}, mo);
float3 c0 = lerp(hm, lf * (0.75 + s.x * 0.35), s.z);
return float4(c0, 1.0);""", name="LitterColor", a=M, b=moss, c=hum, z=P["var"], y=P["age"])
    tw = m.px(f"""
float t = c.sample(uv);
float3 c0 = lerp(a.sample(uv).rgb, {col("#4a3626")} * (0.8 + texToFloat(b.sample(uv)) * 0.4), step(0.3, t));
return float4(c0, 1.0);""", name="BaseColor", a=base, b=hum, c=twigs["out"])
    r = m.gray("lerp(0.9, lerp(0.7, 0.45, p.y), a.sample(uv).z)", name="Roughness", a=M, y=P["age"])
    h = m.gray("0.15 + a.sample(uv).x * 0.35 * (1.0 - p.x * 0.5) + c.sample(uv) * 0.25", name="Height", a=M, c=twigs["out"], x=P["wear"])
    m.finish(tw, r, 0.0, h, normal=1.2, ao=(0.02, 0.15, 1.0), hue=P["hue"])


@material("Snow", "Fresh Snow", "Ground/Snow", "dielectric", 200, "Soft wind-shaped fresh snow: gentle drifts, faint sastrugi, glinting crystals, blue in the hollows.", ["snow", "winter", "ground", "white", "drifts"])
def snow(m):
    P = std(m, wear=0.2, age=0.2, wear_doc="Trodden, compacted snow.", age_doc="Crust, slush and grit.")
    drift = m.add("clouds", name="Drifts", scale=3, octaves=6, persistence=0.5)
    wind = m.add("directional", name="Sastrugi", angle=15.0, scaleAlong=2, scaleAcross=14, octaves=4)
    glint = m.add("cells", name="Glints", scale=120, mode="cells", jitter=1.0)
    grit = grime_noise(m, scale=6)
    M = m.px("""
float d0 = texToFloat(a.sample(uv));
float w = texToFloat(b.sample(uv));
float hgt = d0 * 0.8 + w * 0.2;
float gl = step(0.985, c.sample(uv));
float gr = smoothstep(1.0 - p.y * 0.5, 1.2 - p.y * 0.5, texToFloat(d.sample(uv)));
return float4(hgt, gl, gr, 1.0);""", name="Surface", a=drift, b=wind, c=glint, d=grit, y=P["age"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#c9d6e2")}, {col("#e8ecef")}, smoothstep(0.2, 0.7, s.x));
c0 = lerp(c0, {col("#a49a8c")}, s.z * 0.6);
c0 = lerp(c0, c0 * {col("#dfe6ee")}, p.y * 0.4);
return float4(c0, 1.0);""", name="BaseColor", a=M, y=P["age"])
    r = m.gray("lerp(0.75, 0.55, p.x) - a.sample(uv).y * 0.4 - p.y * 0.2", name="Roughness", a=M, x=P["wear"], y=P["age"])
    h = m.gray("0.2 + a.sample(uv).x * (0.6 - p.x * 0.3)", name="Height", a=M, x=P["wear"])
    m.finish(base, r, 0.0, h, normal=0.8, ao=(0.04, 0.15, 0.6), hue=P["hue"])


@material("Ice", "Ice", "Ground/Ice", "dielectric", 150, "Clear blue lake ice: smooth glassy surface, white fracture lines and trapped bubbles.", ["ice", "frozen", "winter", "glassy", "cracks"])
def ice(m):
    P = std(m, wear=0.2, age=0.3, wear_doc="Skate scratches and frosting.", age_doc="Fractures and trapped bubbles.")
    cracks = m.add("cells", name="Fractures", scale=4, mode="crackle", width=0.008, jitter=1.0)
    wn = m.add("perlin", name="Warp", scale=6, octaves=4)
    crackW = m.add("warp", name="FractureWarp", input=cracks, warp=wn, mode="gradient", intensity=0.8)
    bub = m.add("worley", name="Bubbles", scale=36, octaves=2, invert=True)
    depth = m.add("clouds", name="Depth", scale=3, octaves=6)
    scr = m.add("scratches", name="SkateMarks", scale=4, count=4, length=0.9, width=0.0015, angle=10.0, angleRandom=0.4)
    M = m.px("""
float ck = texToFloat(a.sample(uv)) * smoothstep(0.0, 0.6, p.y);
float b0 = smoothstep(0.85, 0.95, texToFloat(b.sample(uv))) * p.y;
float sc = d.sample(uv) * p.x;
return float4(ck, b0, c.sample(uv), sc);""", name="Ice", a=crackW, b=bub, c=depth, d=scr, x=P["wear"], y=P["age"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#5f8ca6")}, {col("#9cc2d4")}, s.z * (0.6 + p.z * 0.4));
c0 = lerp(c0, {col("#dbe8ef")}, saturate(s.x + s.y * 0.7 + s.w * 0.5));
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"])
    r = m.gray("0.03 + a.sample(uv).w * 0.4 + a.sample(uv).x * 0.3", name="Roughness", a=M)
    h = m.gray("0.5 - a.sample(uv).x * 0.03 - a.sample(uv).w * 0.01", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.6, ao=(0.01, 0.03, 0.3), hue=P["hue"])
