"""Woods: grain is rings around a virtual pith, warped by noise, so flat-sawn boards show arches and quarter-sawn ones stripes."""

from common import material, std, grime_noise
from matgraph import col, f


def board_grain(m, P, early, late, uv_src, rand_src, width_cm=15.0, length_cm=100.0, ring_cm=0.45, name="Grain"):
    """Packs (ring phase, latewood, fibre, board random) for boards given each board's local uv and a random value.

    Each board is a tangential cut through a slightly tapering log: rings are where the distance from the pith, plus the
    taper along the board, crosses whole ring widths, which draws straight grain at the edges and arches in the middle.
    """
    warp = m.add("perlin", name=name + "Warp", scale=5, octaves=5, persistence=0.5)
    fib = m.add("anisotropic", name=name + "Fibre", scaleX=4, scaleY=200, octaves=4, persistence=0.65)
    return m.px(f"""
float2 q = a.sample(uv).xy;
float r = texToFloat(b.sample(uv));
float w = c.sample(uv);
float fb = d.sample(uv);
float Y = q.y * {f(width_cm)};
float X = q.x * {f(length_cm)};
float y0 = frac(r * 7.13) * {f(width_cm)} * 1.4 - {f(width_cm)} * 0.2;
float dep = 2.0 + frac(r * 3.71) * 14.0;
float taper = (0.04 + frac(r * 5.3) * 0.08) * (frac(r * 9.1) > 0.5 ? 1.0 : -1.0);
float rad = sqrt((Y - y0) * (Y - y0) + dep * dep) + X * taper + (w - 0.5) * 1.6 + sin(X * 0.08 + r * 20.0) * 0.4;
float spacing = {f(ring_cm)} * (0.75 + frac(r * 2.9) * 0.5) * (1.0 + (w - 0.5) * 0.3);
float t = frac(rad / spacing);
float late = smoothstep(0.45, 0.8, t) * (1.0 - smoothstep(0.88, 1.0, t));
return float4(t, late, fb, r);""", name=name, a=uv_src, b=rand_src, c=warp, d=fib)


def wood_colour(m, P, G, early, late, extra="", name="WoodColor", **ins):
    return m.px(f"""
float4 g = a.sample(uv);
float tone = (g.w - 0.5) * 0.35 * (0.3 + p.z);
float3 e = {col(early)} * (1.0 + tone) * (1.0 + (g.z - 0.5) * 0.18);
float3 l = {col(late)} * (1.0 + tone * 0.8);
float3 c0 = lerp(e, l, g.y);
{extra}
return float4(saturate(c0), 1.0);""", name=name, a=G, z=P["var"], **ins)


def planks_wood(m, P, early, late, rows=8, planks=2, ring_cm=0.45, rough=(0.5, 0.7), gap=0.04, normal=0.8, weathered=0.0, size_cm=200.0):
    pl = m.add("planks", name="Boards", rows=rows, planks=planks, gap=gap, bevel=0.08)
    G = board_grain(m, P, early, late, pl["plankUv"], pl["random"], width_cm=size_cm / rows, length_cm=size_cm / planks, ring_cm=ring_cm)
    dents = m.add("scratches", name="Dents", scale=5, count=5, length=0.25, width=0.002, angleRandom=1.0)
    grime = grime_noise(m, scale=6)
    W = m.px(f"""
float mask = texToFloat(a.sample(uv));
float sc = c.sample(uv) * smoothstep(0.0, 0.6, p.x);
float g = texToFloat(b.sample(uv));
float gap = 1.0 - smoothstep(0.0, 0.5, mask);
float grime = saturate(gap * (0.4 + p.y) + smoothstep(1.0 - p.y * 0.6, 1.3 - p.y * 0.6, g) * 0.6);
return float4(mask, sc, grime, gap);""", name="Wear", a=pl["mask"], b=grime, c=dents, x=P["wear"], y=P["age"])
    base = wood_colour(m, P, G, early, late, extra=f"""
float4 w = b.sample(uv);
float grey = {f(weathered)} + p.y * 0.3 * {f(weathered)};
c0 = lerp(c0, dot(c0, float3(0.33, 0.34, 0.33)) * {col("#d6d0c6")} * 1.1, saturate(grey));
c0 = lerp(c0, c0 * 0.55, w.z * 0.7);
c0 = lerp(c0, c0 * 1.12, w.y * 0.4);""", b=W)
    lo, hi = rough
    r = m.px(f"""
float4 g = a.sample(uv);
float4 w = b.sample(uv);
float r = lerp({f(lo)}, {f(hi)}, saturate(0.5 + (g.z - 0.5) * 0.8 - g.y * 0.3));
r = lerp(r, 0.85, w.z * 0.6);
r = lerp(r, r * 0.8, w.y);
return float4(r, r, r, 1.0);""", name="Roughness", a=G, b=W)
    h = m.px(f"""
float4 g = a.sample(uv);
float4 w = b.sample(uv);
float relief = g.y * (0.012 + {f(weathered)} * 0.04) + (g.z - 0.5) * 0.008;
float h = 0.35 + w.x * 0.15 + relief - w.y * 0.03;
return float4(h, h, h, 1.0);""", name="Height", a=G, b=W)
    m.finish(base, r, 0.0, h, normal=normal, ao=(0.02, 0.08, 0.8), hue=P["hue"])


@material("WoodPlanks", "Oak Floorboards", "Wood/Planks", "organic", 200, "Oiled oak floorboards: arched flat-sawn grain, per-board tone, dents and grime in the seams.", ["wood", "oak", "planks", "floor", "boards"])
def wood_planks(m):
    P = std(m, wear=0.3, age=0.2, wear_doc="Dents and scuffs.", age_doc="Grime in the seams and darkening.")
    planks_wood(m, P, "#b38a5c", "#83592f", rows=8, planks=2, ring_cm=0.4)


@material("WoodPine", "Pine Boards", "Wood/Planks", "organic", 200, "Pale knotty-free pine boards with strong summer/winter ring contrast.", ["wood", "pine", "planks", "boards", "pale"])
def wood_pine(m):
    P = std(m, wear=0.2, age=0.15, wear_doc="Dents and scuffs.", age_doc="Grime in the seams and yellowing.")
    planks_wood(m, P, "#d9b88a", "#b5824e", rows=6, planks=2, ring_cm=0.6, rough=(0.55, 0.72))


@material("WoodWorn", "Weathered Wood", "Wood/Planks", "organic", 200, "Sun-greyed weathered boards: silvered surface, soft earlywood eroded away so the grain stands proud.", ["wood", "weathered", "grey", "old", "driftwood", "planks"])
def wood_worn(m):
    P = std(m, wear=0.5, age=0.5, wear_doc="Scuffs and splits.", age_doc="Greying and grime.")
    planks_wood(m, P, "#9a8a76", "#6e5f4e", rows=5, planks=1, ring_cm=0.7, rough=(0.7, 0.9), gap=0.05, normal=1.4, weathered=0.6)


@material("WoodBeams", "Timber Beam", "Wood/Beams", "organic", 60, "Rough-sawn structural timber: open grain, drying checks along the length and dark aged tones.", ["wood", "beam", "timber", "rough", "structural"])
def wood_beams(m):
    P = std(m, wear=0.4, age=0.4, wear_doc="Checks and splits along the grain.", age_doc="Darkening and grime.")
    grid = m.add("uv", name="BeamUv")
    one = m.add("constant", name="BeamRandom", value=0.37)
    G = board_grain(m, P, "#8a6440", "#5a3c22", grid, one, width_cm=60.0, length_cm=60.0, ring_cm=1.1)
    checks = m.add("directional", name="Checks", angle=0.0, scaleAlong=2, scaleAcross=40, octaves=3, persistence=0.4)
    saw = m.add("anisotropic", name="SawMarks", scaleX=60, scaleY=3, octaves=2)
    grime = grime_noise(m, scale=4)
    W = m.px("""
float k = texToFloat(a.sample(uv));
float crack = smoothstep(0.8 - p.x * 0.15, 0.86 - p.x * 0.15, k);
float saw = c.sample(uv);
float g = texToFloat(b.sample(uv));
float grime = smoothstep(1.0 - p.y * 0.7, 1.25 - p.y * 0.7, g);
return float4(crack, saw, grime, 1.0);""", name="Checks2", a=checks, b=grime, c=saw, x=P["wear"], y=P["age"])
    base = wood_colour(m, P, G, "#8a6440", "#5a3c22", extra="""
float4 w = b.sample(uv);
c0 *= 0.92 + w.y * 0.16;
c0 = lerp(c0, c0 * 0.4, w.x);
c0 = lerp(c0, c0 * 0.6, w.z * 0.6);""", b=W)
    r = m.gray("lerp(0.7, 0.88, a.sample(uv).z) + b.sample(uv).x * 0.08", name="Roughness", a=G, b=W)
    h = m.gray("0.5 + a.sample(uv).y * 0.02 + (b.sample(uv).y - 0.5) * 0.03 - b.sample(uv).x * 0.2 + (a.sample(uv).z - 0.5) * 0.02", name="Height", a=G, b=W)
    m.finish(base, r, 0.0, h, normal=1.0, ao=(0.02, 0.1, 1.0), hue=P["hue"])


@material("WoodPainted", "Painted Wood", "Wood/Painted", "organic", 150, "Painted clapboard: a matte coat that follows the grain and wears through to bare wood on the edges and high spots.", ["wood", "paint", "painted", "chipped", "boards", "clapboard"])
def wood_painted(m):
    P = std(m, wear=0.35, age=0.25, wear_doc="Paint worn through to wood.", age_doc="Grime and fading.")
    paint = m.param("paintColor", "color", "#9fb3a6", None, None, "Paint Colour", "The paint's colour (sRGB).")
    pl = m.add("planks", name="Boards", rows=6, planks=1, gap=0.03, bevel=0.12)
    G = board_grain(m, P, "#a8835a", "#7a5634", pl["plankUv"], pl["random"], width_cm=25.0, length_cm=150.0, ring_cm=0.5)
    chips = m.add("perlin", name="Chips", scale=7, octaves=7, persistence=0.62)
    grime = grime_noise(m, scale=5)
    W = m.px("""
float mask = texToFloat(a.sample(uv));
float n = texToFloat(b.sample(uv)) + (1.0 - mask) * 0.25;
float t = 0.82 - p.x * 0.45;
float chip = smoothstep(t, t + 0.02, n);
float g = texToFloat(d.sample(uv));
float grime = saturate((1.0 - smoothstep(0.0, 0.6, mask)) * (0.3 + p.y) + smoothstep(1.0 - p.y * 0.6, 1.3 - p.y * 0.6, g) * 0.5);
return float4(mask, chip, grime, 1.0);""", name="Paint", a=pl["mask"], b=chips, d=grime, x=P["wear"], y=P["age"])
    tint = m.color(paint, name="PaintColor")
    wood = wood_colour(m, P, G, "#a8835a", "#7a5634")
    chip = m.gray("b.sample(uv).y", name="ChipMask", b=W)
    dirt = m.gray("b.sample(uv).z", name="GrimeMask", b=W)
    base = m.px("""
float3 wd = a.sample(uv).rgb;
float3 pc = b.sample(uv).rgb;
pc = lerp(pc, dot(pc, float3(0.3, 0.4, 0.3)).xxx * 1.05, p.w * 0.25);
float3 c0 = lerp(pc * (0.97 + dot(wd, float3(0.1, 0.1, 0.1))), wd, c.sample(uv));
c0 = lerp(c0, c0 * 0.6, d.sample(uv) * 0.6);
return float4(c0, 1.0);""", name="BaseColor", a=wood, b=tint, c=chip, d=dirt, w=P["age"])
    r = m.gray("lerp(0.62, lerp(0.6, 0.75, a.sample(uv).z), b.sample(uv).y) + b.sample(uv).z * 0.15", name="Roughness", a=G, b=W)
    h = m.gray("0.35 + b.sample(uv).x * 0.15 + a.sample(uv).y * 0.012 + (1.0 - b.sample(uv).y) * 0.012", name="Height", a=G, b=W)
    m.finish(base, r, 0.0, h, normal=0.8, ao=(0.02, 0.08, 0.8), hue=P["hue"])


@material("WoodEndGrain", "End Grain Blocks", "Wood/End Grain", "organic", 40, "End-grain wood blocks, as on a butcher block: concentric rings around off-centre piths, radial drying cracks.", ["wood", "end grain", "rings", "blocks", "butcher block"])
def wood_end_grain(m):
    P = std(m, wear=0.3, age=0.2, wear_doc="Radial drying cracks and knife marks.", age_doc="Darkening and grime.")
    t = m.add("tiles", name="Blocks", countX=4, countY=4, gap=0.015, bevel=0.03)
    warp = m.add("perlin", name="RingWarp", scale=8, octaves=5)
    pores = m.add("worley", name="Pores", scale=48, octaves=1, invert=True)
    G = m.px("""
float2 q = a.sample(uv).xy - 0.5;
float r = texToFloat(b.sample(uv));
float2 pith = float2(cos(r * 40.0), sin(r * 40.0)) * (0.3 + r * 0.6);
float2 dq = q - pith;
float dist = length(dq) + (c.sample(uv) - 0.5) * 0.05;
float ring = dist * (7.0 + r * 5.0);
float t = frac(ring);
float late = smoothstep(0.55, 0.85, t) * (1.0 - smoothstep(0.9, 1.0, t));
float ang = atan2(dq.y, dq.x);
float crack = pow(saturate(abs(sin(ang * (1.0 + floor(r * 2.0)) + r * 9.0))), 400.0) * smoothstep(0.1, 0.45, dist) * step(0.5, frac(r * 13.7)) * smoothstep(0.0, 0.7, p.x);
return float4(t, late, crack, r);""", name="Rings", a=t["tileUv"], b=t["random"], c=warp, x=P["wear"])
    base = m.px(f"""
float4 g = a.sample(uv);
float tone = (g.w - 0.5) * 0.4 * (0.3 + p.z);
float3 c0 = lerp({col("#b88a5a")}, {col("#7e5432")}, g.y) * (1.0 + tone);
c0 *= 0.9 + c.sample(uv) * 0.12;
c0 = lerp(c0, {col("#3e2a1c")}, g.z);
c0 *= 1.0 - p.y * 0.25;
return float4(saturate(c0), 1.0);""", name="BaseColor", a=G, c=pores, z=P["var"], y=P["age"])
    r = m.gray("0.6 + a.sample(uv).y * 0.08 + a.sample(uv).z * 0.2", name="Roughness", a=G)
    h = m.gray("0.35 + texToFloat(b.sample(uv)) * 0.15 + a.sample(uv).y * 0.01 - a.sample(uv).z * 0.06 - (1.0 - c.sample(uv)) * 0.004", name="Height", a=G, b=t["mask"], c=pores)
    m.finish(base, r, 0.0, h, normal=0.8, ao=(0.015, 0.06, 0.7), hue=P["hue"])


@material("Bark", "Pine Bark", "Wood/Bark", "organic", 60, "Deeply furrowed bark: long irregular plates split by dark fissures, flaky layered surfaces, lichen-grey highlights.", ["bark", "tree", "pine", "trunk", "organic"])
def bark(m):
    P = std(m, wear=0.3, age=0.3, wear_doc="Flaking of the plates.", age_doc="Grey lichen and moss in the fissures.")
    plates = m.add("cells", name="Plates", scale=3, mode="f2MinusF1", jitter=1.0)
    plateId = m.add("cells", name="PlateId", scale=3, mode="cells", jitter=1.0)
    ridge = m.add("directional", name="Ridges", angle=90.0, scaleAlong=2, scaleAcross=14, octaves=5, persistence=0.55)
    flake = m.add("perlin", name="Flake", scale=12, octaves=6, persistence=0.6)
    wob = m.px("""
float n = texToFloat(a.sample(uv));
float n2 = texToFloat(a.sample(uv + float2(0.37, 0.71)));
return float4(n, n2, 0.0, 1.0);""", name="Wobble", a=flake)
    M = m.px("""
float2 wv = b.sample(uv).xy - 0.5;
float2 s = float2(uv.x * 4.0 + wv.x * 0.45, uv.y + wv.y * 0.12);
float edge = c.sample(s);
float id = d.sample(s);
float rd = texToFloat(a.sample(uv));
float fl = b.sample(uv).x;
float plate = smoothstep(0.03, 0.35, edge + (rd - 0.5) * 0.12);
float fissure = 1.0 - smoothstep(0.02, 0.14, edge + (rd - 0.5) * 0.1);
float layers = floor(saturate(fl * 1.6 - 0.2 + id * 0.3) * 3.0) / 3.0;
float flaked = smoothstep(0.65 - p.x * 0.2, 0.7 - p.x * 0.2, fl) * plate;
return float4(plate * (0.75 + layers * 0.25 + rd * 0.2), fissure, flaked, id);""", name="Bark", a=ridge, b=wob, c=plates, d=plateId, x=P["wear"])
    base = m.px(f"""
float4 s = a.sample(uv);
float3 c0 = lerp({col("#4e3726")}, {col("#80604a")}, saturate(s.x * 0.8 + (s.w - 0.5) * 0.4 * (0.3 + p.z)));
c0 = lerp(c0, {col("#9a5f3e")}, s.z * 0.65);
c0 = lerp(c0, {col("#2e2117")}, s.y);
float lichen = smoothstep(0.8 - p.y * 0.4, 1.0 - p.y * 0.4, s.x * (0.6 + s.w * 0.6));
c0 = lerp(c0, {col("#8e8f7e")}, lichen * 0.55);
c0 = lerp(c0, {col("#45542a")}, s.y * smoothstep(0.2, 1.0, p.y) * 0.6);
return float4(c0, 1.0);""", name="BaseColor", a=M, z=P["var"], y=P["age"])
    r = m.gray("lerp(0.75, 0.92, a.sample(uv).y) - a.sample(uv).z * 0.05", name="Roughness", a=M)
    h = m.gray("0.15 + a.sample(uv).x * 0.6 - a.sample(uv).z * 0.06", name="Height", a=M)
    m.finish(base, r, 0.0, h, normal=1.6, ao=(0.04, 0.3, 1.0), hue=P["hue"])
