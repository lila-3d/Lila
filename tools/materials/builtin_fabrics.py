"""Fabrics and leather. Woven ones draw each thread crossing explicitly so the weave stays crisp at any resolution."""

from common import material, std, grime_noise
from matgraph import col, f


def woven(m, P, count, thread_col, weft_col, rough=(0.85, 0.95), slub=0.3, fuzz=0.3, normal=1.0):
    w = m.add("weave", name="Weave", count=count, width=0.85)
    slubs = m.add("anisotropic", name="Slubs", scaleX=2, scaleY=count * 2, octaves=3, persistence=0.5)
    slubsV = m.add("anisotropic", name="SlubsWeft", scaleX=count * 2, scaleY=2, octaves=3, persistence=0.5)
    fz = m.add("perlin", name="Fuzz", scale=count * 2, octaves=2)
    M = m.px(f"""
float h = texToFloat(a.sample(uv));
float dir = texToFloat(b.sample(uv));
float s = lerp(texToFloat(c.sample(uv)), d.sample(uv), dir);
float hgt = h * (1.0 + (s - 0.5) * {f(slub)});
return float4(hgt, dir, s, 1.0);""", name="Threads", a=w["height"], b=w["direction"], c=slubs, d=slubsV)
    wearN = m.add("clouds", name="WearPatches", scale=4, octaves=5)
    stains = grime_noise(m, scale=3)
    base = m.px(f"""
float4 t = a.sample(uv);
float3 c0 = lerp({col(thread_col)}, {col(weft_col)}, t.y) * (0.82 + t.x * 0.22) * (1.0 + (t.z - 0.5) * 0.25 * (0.4 + p.z));
float wr = smoothstep(1.0 - p.x * 0.6, 1.2 - p.x * 0.6, texToFloat(b.sample(uv)));
c0 = lerp(c0, c0 * 1.12 + 0.03, wr * 0.7);
float st = smoothstep(1.0 - p.y * 0.5, 1.15 - p.y * 0.5, texToFloat(c.sample(uv)));
c0 = lerp(c0, c0 * {col("#b8a88a")}, st * 0.8);
return float4(saturate(c0), 1.0);""", name="BaseColor", a=M, b=wearN, c=stains, z=P["var"], x=P["wear"], y=P["age"])
    lo, hi = rough
    r = m.gray(f"lerp({f(hi)}, {f(lo)}, a.sample(uv).x) + (d.sample(uv) - 0.5) * {f(fuzz * 0.2)}", name="Roughness", a=M, d=fz)
    h = m.gray(f"0.3 + a.sample(uv).x * 0.4 + (d.sample(uv) - 0.5) * {f(fuzz * 0.05)}", name="Height", a=M, d=fz)
    m.finish(base, r, 0.0, h, normal=normal, ao=(0.005, 0.1, 0.8), hue=P["hue"])


@material("Cotton", "Cotton", "Fabric/Cotton", "organic", 4, "Plain-weave cotton: fine even threads, slight slubs and fuzz, soft matte sheen.", ["cotton", "fabric", "cloth", "textile", "plain weave"])
def cotton(m):
    P = std(m, wear=0.2, age=0.15, wear_doc="Washed-out, thin patches.", age_doc="Stains and yellowing.")
    woven(m, P, 40, "#d6d1c6", "#cfc9bd", rough=(0.82, 0.95), slub=0.25, fuzz=0.4, normal=0.6)


@material("Canvas", "Canvas", "Fabric/Canvas", "organic", 8, "Heavy cotton duck canvas: thick irregular threads in a tight plain weave, natural undyed beige.", ["canvas", "fabric", "duck", "textile", "heavy"])
def canvas(m):
    P = std(m, wear=0.3, age=0.3, wear_doc="Scuffed, lighter patches.", age_doc="Grime and stains.")
    woven(m, P, 28, "#b9aa8a", "#a99a7a", rough=(0.85, 0.97), slub=0.55, fuzz=0.3, normal=1.1)


@material("Denim", "Denim", "Fabric/Denim", "organic", 5, "Indigo denim: 3/1 twill with ring-dyed blue warp over white weft, fading where it wears.", ["denim", "jeans", "twill", "indigo", "fabric", "textile"])
def denim(m):
    P = std(m, wear=0.3, age=0.1, wear_doc="Faded, abraded areas.", age_doc="Grime and stains.")
    slub = m.add("anisotropic", name="Slubs", scaleX=3, scaleY=128, octaves=3, persistence=0.6)
    fade = m.add("clouds", name="Fade", scale=4, octaves=6)
    M = m.px("""
float N = 64.0;
float2 g = uv * N;
int2 cell = int2(floor(g));
float2 fr = frac(g);
bool warpTop = ((cell.x - cell.y + 4096) % 4) != 3;
float dw = sin(fr.x * 3.14159);
float df = sin(fr.y * 3.14159);
float hgt = warpTop ? 0.55 + 0.45 * dw * (0.6 + 0.4 * sin(fr.y * 3.14159)) : 0.45 + 0.4 * df;
float sl = texToFloat(a.sample(float2(uv.y, uv.x)));
float ring = warpTop ? saturate(0.6 - dw * 0.8 + (sl - 0.5) * 0.8) : 0.0;
return float4(hgt, warpTop ? 1.0 : 0.0, ring, sl);""", name="Twill", a=slub)
    stains = grime_noise(m, scale=4)
    base = m.px(f"""
float4 t = a.sample(uv);
float fd = smoothstep(1.0 - p.x * 0.8, 1.3 - p.x * 0.8, texToFloat(b.sample(uv)) + t.x * 0.2);
float3 indigo = lerp({col("#24365a")}, {col("#3e5682")}, saturate(t.w * 0.8 + fd * 0.6) * (0.5 + p.z * 0.5));
indigo = lerp(indigo, {col("#8796b0")}, saturate(t.z * 0.2 + fd * 0.3));
float3 weft = {col("#8e95a2")};
float3 c0 = lerp(weft, indigo, t.y) * (0.8 + t.x * 0.25);
c0 = lerp(c0, c0 * 0.75, smoothstep(1.0 - p.y * 0.5, 1.2 - p.y * 0.5, texToFloat(c.sample(uv))));
return float4(saturate(c0), 1.0);""", name="BaseColor", a=M, b=fade, c=stains, z=P["var"], x=P["wear"], y=P["age"])
    r = m.gray("0.95 - a.sample(uv).x * 0.12", name="Roughness", a=M)
    h = m.gray("0.3 + a.sample(uv).x * 0.4", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.6, ao=(0.004, 0.1, 0.8), hue=P["hue"])


@material("Wool", "Knitted Wool", "Fabric/Wool", "organic", 12, "Chunky stockinette knit: rows of V-shaped stitches in soft fuzzy wool.", ["wool", "knit", "knitted", "jumper", "fabric", "yarn"])
def wool(m):
    P = std(m, wear=0.2, age=0.1, wear_doc="Pilling and felting.", age_doc="Grime and fading.")
    fz = m.add("perlin", name="Fuzz", scale=96, octaves=3)
    fibre = m.add("gabor", name="Fibre", scale=24, impulses=8, frequency=3.0, angleRandom=1.0)
    M = m.px("""
float2 N = float2(16.0, 20.0);
float2 g = uv * N;
float2 fr = frac(g);
float2 q1 = fr - float2(0.27, 0.5);
float2 q2 = fr - float2(0.73, 0.5);
float a1 = 0.55;
float2 r1 = float2(q1.x * cos(a1) - q1.y * sin(a1), q1.x * sin(a1) + q1.y * cos(a1));
float2 r2 = float2(q2.x * cos(-a1) - q2.y * sin(-a1), q2.x * sin(-a1) + q2.y * cos(-a1));
float e1 = length(r1 / float2(0.2, 0.52));
float e2 = length(r2 / float2(0.2, 0.52));
float l1 = sqrt(saturate(1.0 - e1 * e1));
float l2 = sqrt(saturate(1.0 - e2 * e2));
float2 g2 = g + float2(0.0, 0.5);
float2 fr2 = frac(g2);
float2 s1 = fr2 - float2(0.27, 0.5);
float2 s2 = fr2 - float2(0.73, 0.5);
float2 t1 = float2(s1.x * cos(a1) - s1.y * sin(a1), s1.x * sin(a1) + s1.y * cos(a1));
float2 t2 = float2(s2.x * cos(-a1) - s2.y * sin(-a1), s2.x * sin(-a1) + s2.y * cos(-a1));
float m1 = sqrt(saturate(1.0 - dot(t1 / float2(0.2, 0.52), t1 / float2(0.2, 0.52))));
float m2 = sqrt(saturate(1.0 - dot(t2 / float2(0.2, 0.52), t2 / float2(0.2, 0.52))));
float hgt = max(max(l1, l2), max(m1, m2) * 0.9);
float fz = c.sample(uv);
float fb = d.sample(uv);
hgt = hgt * (0.85 + (fb - 0.5) * 0.3) + (fz - 0.5) * 0.08 * (1.0 + p.x);
return float4(saturate(hgt), fz, fb, 1.0);""", name="Stitches", c=fz, d=fibre, x=P["wear"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = {col("#cbbfa6")} * (0.62 + s.x * 0.45) * (1.0 + (s.y - 0.5) * 0.15 * (0.5 + p.z));
c0 *= 1.0 - p.y * 0.2;
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"], y=P["age"])
    r = m.gray("0.95 + (a.sample(uv).y - 0.5) * 0.05", name="Roughness", a=M)
    h = m.gray("0.25 + a.sample(uv).x * 0.5", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.9, ao=(0.01, 0.2, 1.0), hue=P["hue"])


@material("Leather", "Leather", "Fabric/Leather", "organic", 30, "Pebble-grain cowhide: rounded grain, fine creases, waxy sheen that burnishes with handling.", ["leather", "hide", "grain", "upholstery", "brown"])
def leather(m):
    P = std(m, wear=0.3, age=0.2, wear_doc="Burnished, lighter handled areas.", age_doc="Dry cracking.")
    grain = m.add("cells", name="Pebble", scale=40, mode="f2MinusF1", jitter=1.0)
    crease = m.add("directional", name="Creases", angle=35.0, scaleAlong=3, scaleAcross=24, octaves=4)
    crease2 = m.add("directional", name="Creases2", angle=125.0, scaleAlong=3, scaleAcross=30, octaves=4)
    tone = m.add("clouds", name="Tone", scale=4, octaves=6)
    M = m.px("""
float e = c.sample(uv);
float dome = sqrt(saturate(e / 0.4));
float k1 = texToFloat(a.sample(uv));
float k2 = texToFloat(b.sample(uv));
float cr = (1.0 - smoothstep(0.0, 0.05, abs(k1 - 0.5))) * 0.6 + (1.0 - smoothstep(0.0, 0.04, abs(k2 - 0.5))) * 0.4;
cr *= 0.6 + p.y;
return float4(dome, saturate(cr), d.sample(uv), 1.0);""", name="Grain", a=crease, b=crease2, c=grain, d=tone, y=P["age"])
    burn = m.add("clouds", name="Burnish", scale=3, octaves=5)
    base = m.px(f"""
float4 s = a.sample(uv);
float br = smoothstep(1.0 - p.x * 0.7, 1.25 - p.x * 0.7, texToFloat(b.sample(uv)));
float3 c0 = lerp({col("#4a2c1a")}, {col("#6e4428")}, saturate(s.z * 0.8 + s.x * 0.35 + (s.z - 0.5) * p.z));
c0 = lerp(c0, {col("#8a5a36")}, br * 0.6 * s.x);
c0 = lerp(c0, c0 * 0.6, s.y);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=burn, z=P["var"], x=P["wear"])
    r = m.px("""
float4 s = a.sample(uv);
float br = smoothstep(1.0 - p.x * 0.7, 1.25 - p.x * 0.7, texToFloat(b.sample(uv)));
float r = lerp(0.62, 0.45, s.x) - br * 0.15 + s.y * 0.15;
return float4(r, r, r, 1.0);""", name="Roughness", a=M, b=burn, x=P["wear"])
    h = m.gray("0.4 + a.sample(uv).x * 0.15 - a.sample(uv).y * 0.12", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.8, ao=(0.005, 0.05, 0.6), hue=P["hue"])


@material("CarbonFibre", "Carbon Fibre", "Fabric/Carbon Fibre", "dielectric", 6, "2x2 twill carbon fibre under a glossy clear coat: tows catching the light in alternating directions.", ["carbon", "carbon fibre", "carbon fiber", "composite", "twill", "racing"])
def carbon_fibre(m):
    P = std(m, wear=0.15, age=0.05, hue=False, wear_doc="Scuffed clear coat.", age_doc="Dust and smears.")
    fib = m.add("anisotropic", name="Fibres", scaleX=4, scaleY=256, octaves=3, persistence=0.6)
    scr = m.add("scratches", name="Scuffs", scale=6, count=4, length=0.4, width=0.0008, angleRandom=1.0)
    M = m.px("""
float N = 16.0;
float2 g = uv * N;
int2 cell = int2(floor(g));
float2 fr = frac(g);
bool along = ((cell.x + cell.y + 4096) % 4) < 2;
float dome = along ? sin(fr.y * 3.14159) : sin(fr.x * 3.14159);
float fb = along ? texToFloat(a.sample(uv)) : texToFloat(a.sample(float2(uv.y, uv.x)));
return float4(dome, along ? 1.0 : 0.0, fb, c.sample(uv) * p.x);""", name="Twill", a=fib, c=scr, x=P["wear"])
    base = m.px(f"""
float4 t = a.sample(uv);
float3 c0 = lerp({col("#232427")}, {col("#3a3c41")}, t.y * (0.5 + t.x * 0.5));
c0 *= 0.85 + t.z * 0.3 * (0.5 + p.z);
c0 = lerp(c0, {col("#6a6a6a")}, t.w * 0.4);
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"])
    r = m.gray("0.08 + a.sample(uv).w * 0.3 + p.y * 0.3", name="Roughness", a=M, y=P["age"])
    h = m.gray("0.5 + a.sample(uv).x * 0.006", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=0.4, ao=(0.005, 0.02, 0.3))
