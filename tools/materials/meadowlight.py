"""Meadowlight: hand-painted pastoral materials.

Every surface is painted the way a background painter would: a value (form lit from the upper left, with painted
occlusion) is mapped through a short palette that runs from cool shadow to warm light, then broken into brush strokes by
Lila's stroke sheets (their per-stroke random picks the stroke's colour, their shade nudges its value). Values are
softly posterised so the result reads as paint, not render.
"""

from common import lin, material, std
from matgraph import col, f

SOFT = "strokes/strokes_soft.png"
BRISTLE = "strokes/strokes_bristle.png"
DABS = "strokes/strokes_dabs.png"

ML = "Meadowlight"


def painterly(m, shadow, light, accent, sheet="strokes_soft", scale=20.0, normal=0.45, contrast=0.4, model="painterly", extra=None):
    """The material node settings this look wants (meta.shading): Painterly palette in linear colour."""
    s = {
        "shadingModel": model,
        "strokeMap": sheet,
        "strokeScale": scale,
        "strokeNormal": normal,
        "paletteShadow": lin(shadow),
        "paletteLight": lin(light),
        "paletteAccent": lin(accent),
        "paletteContrast": contrast,
        "specularTint": 0.6,
    }
    if extra:
        s.update(extra)
    m.meta["shadingModel"] = model
    m.meta["shading"] = s


def stroke_sheet(m, path=SOFT, tiles=4, name="Strokes"):
    return m.add("image", name=name, file=path, tiles=tiles)


def paint(m, P, value, strokes, palette, spread=0.3, bands=5.0, name="Paint"):
    """Maps a value through a 4-colour palette (shadow to light), varied per stroke and softly posterised."""
    c0, c1, c2, c3 = (col(c) for c in palette)
    return m.px(f"""
float t = texToFloat(a.sample(uv));
float4 s = b.sample(uv);
t = saturate(t + (s.a - 0.5) * {f(spread)} * (0.4 + p.z) + (s.b - 0.5) * 0.25);
float k = t * {f(bands)};
t = (floor(k) + smoothstep(0.3, 0.7, frac(k))) / {f(bands)};
float3 c = t < 0.34 ? lerp({c0}, {c1}, t / 0.34) : (t < 0.67 ? lerp({c1}, {c2}, (t - 0.34) / 0.33) : lerp({c2}, {c3}, (t - 0.67) / 0.33));
return float4(c, 1.0);""", name=name, a=value, b=strokes, z=P["var"])


def lit_value(m, height, key=0.5, occlusion=0.6, bias=0.0, name="Value", emboss=1.0):
    """Painted light: the height's slope lit from the upper left, its cavities darkened, its tops lifted."""
    e = m.add("emboss", name=name + "Light", input=height, angle=135.0, intensity=emboss)
    blurred = m.add("blur", name=name + "Occlusion", input=height, radius=0.02)
    return m.px(f"""
float h = texToFloat(c.sample(uv));
float l = texToFloat(a.sample(uv));
float cav = saturate((texToFloat(b.sample(uv)) - h) * 8.0);
float v = 0.5 + (l - 0.5) * {f(key * 2)} + (h - 0.5) * 0.5 - cav * {f(occlusion)} + {f(bias)};
return float4(v, v, v, 1.0);""", name=name, a=e, b=blurred, c=height)


def finish_painted(m, P, base, height, rough=0.85, normal=0.5, metallic=0.0, **extra):
    r = m.gray(f"{f(rough)} + (texToFloat(a.sample(uv)) - 0.5) * 0.05", name="Roughness", a=height)
    m.finish(base, r, metallic, height, normal=normal, ao=(0.03, 0.1, 0.5), hue=P.get("hue"), **extra)


def mat(ident, name, sub, scale, description, tags, **kw):
    return material(ident, name, f"{ML}/{sub}", "stylised", scale, description, ["meadowlight", "stylised", "hand-painted", "painterly"] + tags, **kw)


@mat("MeadowGrass", "Meadow Grass", "Grass", 150, "Soft painted meadow grass: tufts in cool teal shadow to sunny yellow-green, stroked in short soft dabs.", ["grass", "meadow", "ground", "green"])
def meadow_grass(m):
    P = std(m, wear=0.15, age=0.2, wear_doc="Trodden, flattened patches.", age_doc="Late-summer gold in the tips.")
    painterly(m, "#2f5a5a", "#f2e6a0", "#c8d86a")
    tuft = m.px("""
float2 q = uv - 0.5;
float r = length(q * float2(1.0, 1.4));
float v = sqrt(saturate(1.0 - r / 0.45)) * (0.6 + 0.4 * saturate(0.5 - q.y));
return float4(v, v, v, 1.0);""", name="Tuft")
    tufts = m.add("splatter", name="Tufts", pattern=tuft, countX=10, countY=10, perCell=6, size=0.8, sizeRandom=0.5, rotationRandom=0.15, valueRandom=0.3, blend="max")
    roll = m.add("clouds", name="Rolling", scale=3, octaves=4)
    h = m.gray("0.25 + c.sample(uv) * 0.5 + (texToFloat(d.sample(uv)) - 0.5) * 0.15 - p.x * 0.15 * c.sample(uv)", name="Height", c=tufts["out"], d=roll, x=P["wear"])
    v = lit_value(m, h, key=0.6, occlusion=0.8)
    st = stroke_sheet(m, SOFT, 6)
    base = paint(m, P, v, st, ["#2a4c4a", "#3f7a4a", "#79a84a", "#cfd67a"])
    gold = m.px(f"""
float3 c0 = a.sample(uv).rgb;
float tip = smoothstep(0.55, 0.9, texToFloat(c.sample(uv)));
c0 = lerp(c0, c0 * {col("#f0d080")} * 1.25, tip * p.y);
return float4(c0, 1.0);""", name="Gold", a=base, c=h, y=P["age"])
    finish_painted(m, P, gold, h, rough=0.85, normal=0.6)


@mat("MeadowMoss", "Painted Moss", "Moss", 60, "Plump painted moss cushions: deep blue-green hollows, lime highlights, soft round dabs.", ["moss", "green", "growth"])
def meadow_moss(m):
    P = std(m, wear=0.1, age=0.3, wear_doc="Flattened cushions.", age_doc="Browning at the edges.")
    painterly(m, "#24494a", "#e8f0a0", "#a6c84a", sheet="strokes_dabs")
    cush = m.add("worley", name="Cushions", scale=7, octaves=3, persistence=0.45, invert=True)
    h = m.gray("0.2 + texToFloat(a.sample(uv)) * 0.6", name="Height", a=cush)
    v = lit_value(m, h, key=0.7, occlusion=1.0, bias=0.05)
    st = stroke_sheet(m, DABS, 6)
    base = paint(m, P, v, st, ["#1f3f3e", "#2f6a3e", "#6d9e38", "#c9dc6a"], spread=0.35)
    brown = m.px(f"""
float3 c0 = a.sample(uv).rgb;
float edge = 1.0 - smoothstep(0.3, 0.5, texToFloat(c.sample(uv)));
c0 = lerp(c0, {col("#8a7a42")}, edge * p.y * 0.7);
return float4(c0, 1.0);""", name="Browning", a=base, c=h, y=P["age"])
    finish_painted(m, P, brown, h, rough=0.9, normal=0.7)


@mat("FoliageClusters", "Dappled Foliage", "Foliage", 100, "Leaf clusters for foliage cards: round clumps lit from above, sun-dappled tops and cool cores, with opacity for cut-outs.", ["foliage", "leaves", "tree", "canopy", "cards"])
def foliage_clusters(m):
    P = std(m, wear=0.1, age=0.2, wear_doc="Gaps between the clumps.", age_doc="Autumn turn.")
    painterly(m, "#1f4048", "#f6eaa8", "#b0d050", model="foliage", extra={
        "foliageBottomColor": lin("#1c3a36"), "foliageTopColor": lin("#9cc450"), "foliageGradient": 0.6,
        "foliageBottomShadow": 0.5, "foliageTopHighlight": 0.35, "foliageTranslucency": 0.5, "foliageWrap": 0.6, "twoSided": True,
        "blendMode": "masked"})
    m.with_channel("opacity")
    clump = m.px("""
float2 q = uv - 0.5;
float r = length(q);
float lobes = 0.4 + 0.05 * sin(atan2(q.y, q.x) * 7.0);
float v = sqrt(saturate(1.0 - r / lobes));
return float4(v, v, v, 1.0);""", name="Clump")
    clumps = m.add("splatter", name="Clumps", pattern=clump, countX=5, countY=5, perCell=4, size=0.9, sizeRandom=0.4, rotationRandom=1.0, valueRandom=0.15, blend="max")
    leafy = m.add("worley", name="Leaves", scale=40, octaves=2, invert=True)
    h = m.gray("c.sample(uv) * (0.85 + texToFloat(d.sample(uv)) * 0.15)", name="Height", c=clumps["out"], d=leafy)
    v = lit_value(m, h, key=0.8, occlusion=0.6, emboss=1.4)
    st = stroke_sheet(m, DABS, 5)
    base = paint(m, P, v, st, ["#1c3a3a", "#2e6a40", "#6ea23c", "#d8e07a"], spread=0.35)
    autumn = m.px(f"""
float3 c0 = a.sample(uv).rgb;
float k = dot(c0, float3(0.3, 0.5, 0.2));
c0 = lerp(c0, lerp({col("#7a3a24")}, {col("#e0a040")}, k * 1.6), p.y * 0.8);
return float4(c0, 1.0);""", name="Autumn", a=base, y=P["age"])
    op = m.gray("smoothstep(0.05 + p.x * 0.3, 0.12 + p.x * 0.3, c.sample(uv))", name="Opacity", c=h, x=P["wear"])
    finish_painted(m, P, autumn, h, rough=0.8, normal=0.5, opacity=op)


@mat("SunlitStone", "Sunlit Stone", "Stone", 150, "Chunky painted stone blocks: warm cream tops catching the sun, lavender shadows in the joints.", ["stone", "wall", "blocks", "masonry", "warm"])
def sunlit_stone(m):
    P = std(m, wear=0.2, age=0.25, wear_doc="Rounded, worn block edges.", age_doc="Moss in the joints.")
    painterly(m, "#5a4a78", "#fff0c8", "#e0a878")
    b = m.add("bricks", name="Blocks", rows=5, columns=3, offset=0.45, mortar=0.06, bevel=0.25)
    lump = m.add("clouds", name="Lumps", scale=6, octaves=4)
    h = m.gray("0.15 + texToFloat(a.sample(uv)) * (0.55 + c.sample(uv) * 0.15) + (texToFloat(b.sample(uv)) - 0.5) * 0.12", name="Height", a=b["mask"], b=lump, c=b["random"])
    v = lit_value(m, h, key=0.7, occlusion=1.2, bias=0.05)
    st = stroke_sheet(m, SOFT, 5)
    base = paint(m, P, v, st, ["#5c4f72", "#9a8a8a", "#d8c09a", "#f6e8c4"])
    moss = m.px(f"""
float3 c0 = a.sample(uv).rgb;
float j = 1.0 - smoothstep(0.1, 0.4, texToFloat(c.sample(uv)));
c0 = lerp(c0, {col("#5e8a44")}, j * smoothstep(0.0, 1.0, p.y) * 0.8);
return float4(c0, 1.0);""", name="JointMoss", a=base, c=b["mask"], y=P["age"])
    finish_painted(m, P, moss, h, rough=0.85, normal=0.8)


@mat("RiverRock", "River Rock", "Stone", 80, "Smooth painted river stones: rounded cool blue-grey pebbles with soft sunlit crowns, damp darker bottoms.", ["stone", "river", "pebbles", "rocks", "cool"])
def river_rock(m):
    P = std(m, wear=0.1, age=0.3, wear_doc="Fewer, sunk stones.", age_doc="Wet, darker stones.")
    painterly(m, "#3a4a6a", "#f2ead0", "#8aa8b0")
    peb = m.px("""
float2 q = uv - 0.5;
float r = length(q * float2(1.0, 1.3));
float v = sqrt(saturate(1.0 - (r / 0.45) * (r / 0.45)));
return float4(v, v, v, 1.0);""", name="Pebble")
    stones = m.add("splatter", name="Stones", pattern=peb, countX=5, countY=5, perCell=3, size=0.85, sizeRandom=0.4, rotationRandom=1.0, valueRandom=0.15, blend="max")
    h = m.gray("0.1 + c.sample(uv) * 0.7", name="Height", c=stones["out"])
    v = lit_value(m, h, key=0.8, occlusion=1.0, emboss=1.2)
    st = stroke_sheet(m, SOFT, 5)
    base = paint(m, P, v, st, ["#30384e", "#5e6e80", "#a8b4b4", "#eee6cc"])
    tint = m.px(f"""
float3 c0 = a.sample(uv).rgb;
float r = b.sample(uv).r;
c0 = lerp(c0, c0 * lerp({col("#e8d0c0")}, {col("#c8d8e0")}, r), 0.5);
c0 *= 1.0 - p.y * 0.35;
return float4(c0, 1.0);""", name="StoneTint", a=base, b=stones["color"], y=P["age"])
    finish_painted(m, P, tint, h, rough=0.6, normal=0.7)


@mat("WarmStucco", "Warm Stucco", "Plaster", 200, "Sun-warmed painted stucco: peach and cream wash, soft blotches, a few bricks peeking through where it has fallen away.", ["stucco", "plaster", "wall", "render", "warm", "village"])
def warm_stucco(m):
    P = std(m, wear=0.4, age=0.3, wear_doc="Fallen-away patches showing brick.", age_doc="Rain streaks and dirt at the base.")
    painterly(m, "#7a5a78", "#fff2d2", "#f0b088", contrast=0.3)
    wash = m.add("clouds", name="Wash", scale=3, octaves=5)
    patch = m.add("perlin", name="Patches", scale=4, octaves=5, persistence=0.6)
    brick = m.add("bricks", name="UnderBrick", rows=16, columns=5, mortar=0.12, bevel=0.15)
    M = m.px("""
float pa = smoothstep(0.78 - p.x * 0.3, 0.8 - p.x * 0.3, texToFloat(a.sample(uv)));
float br = texToFloat(b.sample(uv));
return float4(pa, br, c.sample(uv), 1.0);""", name="Patches2", a=patch, b=brick["mask"], c=wash, x=P["wear"])
    h = m.gray("0.6 + (a.sample(uv).z - 0.5) * 0.06 - a.sample(uv).x * (0.35 - a.sample(uv).y * 0.12)", name="Height", a=M)
    v0 = lit_value(m, h, key=0.5, occlusion=0.6, bias=0.1)
    v = m.gray("texToFloat(a.sample(uv)) + (b.sample(uv).z - 0.5) * 0.9", name="WashValue", a=v0, b=M)
    st = stroke_sheet(m, DABS, 4)
    plaster = paint(m, P, v, st, ["#9a6e6a", "#d89c7c", "#eebf94", "#fae4c0"], spread=0.45, bands=4.0)
    bricks = paint(m, P, v, st, ["#5a3438", "#9a5444", "#c47a58", "#e0a07a"], spread=0.3, name="BrickPaint")
    base = m.px(f"""
float k = texToFloat(c.sample(uv));
float3 c0 = lerp(a.sample(uv).rgb, b.sample(uv).rgb, k);
float streak = smoothstep(0.6, 0.9, frac(uv.x * 7.0 + sin(uv.x * 40.0) * 0.05)) * smoothstep(0.4, 0.0, uv.y);
c0 = lerp(c0, c0 * {col("#b8a8a0")}, saturate(streak + smoothstep(0.25, 0.0, uv.y) * 0.6) * p.y * 0.6);
return float4(c0, 1.0);""", name="BaseColor", a=plaster, b=bricks, c=m.gray("a.sample(uv).x", name="PatchMask", a=M), y=P["age"])
    finish_painted(m, P, base, h, rough=0.9, normal=0.5)


@mat("PaintedPlaster", "Whitewashed Plaster", "Plaster", 150, "Cool whitewashed lime plaster: chalky white with lilac shadows, soft trowel swirls and a few painted cracks.", ["plaster", "whitewash", "wall", "lime", "white", "cottage"])
def painted_plaster(m):
    P = std(m, wear=0.2, age=0.3, wear_doc="Chips in the wash.", age_doc="Painted cracks.")
    painterly(m, "#6a6a98", "#fffaf0", "#d8c8f0", contrast=0.3)
    swirl = m.add("gabor", name="Trowel", scale=3, impulses=6, frequency=0.8, angleRandom=1.0)
    soft = m.add("clouds", name="Soft", scale=3, octaves=5)
    cracks = m.add("cells", name="Cracks", scale=4, mode="crackle", width=0.01, jitter=1.0)
    M = m.px("""
float ck = texToFloat(c.sample(uv)) * smoothstep(0.7 - p.y * 0.4, 0.85 - p.y * 0.4, texToFloat(b.sample(uv)));
return float4(texToFloat(a.sample(uv)), texToFloat(b.sample(uv)), ck, 1.0);""", name="Surface", a=swirl, b=soft, c=cracks, y=P["age"])
    h = m.gray("0.5 + (a.sample(uv).x - 0.5) * 0.05 + (a.sample(uv).y - 0.5) * 0.06 - a.sample(uv).z * 0.05", name="Height", a=M)
    v0 = lit_value(m, h, key=0.6, occlusion=0.8, bias=0.1)
    v = m.gray("texToFloat(a.sample(uv)) + (b.sample(uv).y - 0.5) * 0.5", name="WashValue", a=v0, b=M)
    st = stroke_sheet(m, SOFT, 4)
    base = paint(m, P, v, st, ["#7a76a0", "#bab4d0", "#e8e2e4", "#fbf6ec"], spread=0.25)
    ck = m.px(f"""
float3 c0 = lerp(a.sample(uv).rgb, {col("#5a5070")}, b.sample(uv).z * 0.8);
return float4(c0, 1.0);""", name="CrackPaint", a=base, b=M)
    finish_painted(m, P, ck, h, rough=0.9, normal=0.4)


@mat("PaintedTimber", "Painted Timber", "Wood", 100, "Hand-painted timber beams: long warm grain strokes, honey highlights, plum-brown shadows in the cracks.", ["wood", "timber", "beams", "grain", "warm"])
def painted_timber(m):
    P = std(m, wear=0.2, age=0.3, wear_doc="Splits along the grain.", age_doc="Greying.")
    painterly(m, "#4a3048", "#ffe0a0", "#d88a48", sheet="strokes_bristle")
    pl = m.add("planks", name="Beams", rows=3, planks=1, gap=0.05, bevel=0.2)
    grain = m.add("directional", name="Grain", angle=0.0, scaleAlong=2, scaleAcross=24, octaves=5, persistence=0.6)
    crack = m.add("directional", name="Splits", angle=0.0, scaleAlong=3, scaleAcross=60, octaves=2)
    M = m.px("""
float g = texToFloat(a.sample(uv));
float sp = smoothstep(0.86 - p.x * 0.1, 0.9 - p.x * 0.1, texToFloat(b.sample(uv)));
return float4(g, sp, texToFloat(c.sample(uv)), 1.0);""", name="Wood", a=grain, b=crack, c=pl["mask"], x=P["wear"])
    h = m.gray("0.2 + a.sample(uv).z * 0.5 + (a.sample(uv).x - 0.5) * 0.08 - a.sample(uv).y * 0.15", name="Height", a=M)
    v = lit_value(m, h, key=0.6, occlusion=1.0)
    st = stroke_sheet(m, BRISTLE, 4)
    base = paint(m, P, v, st, ["#3e2838", "#7a4a34", "#b8784a", "#efc480"], spread=0.3)
    grey = m.px("""
float3 c0 = a.sample(uv).rgb;
c0 = lerp(c0, dot(c0, float3(0.3, 0.5, 0.2)).xxx * float3(0.95, 0.95, 1.05), p.y * 0.5);
return float4(c0, 1.0);""", name="Greying", a=base, y=P["age"])
    finish_painted(m, P, grey, h, rough=0.85, normal=0.6)


@mat("WoodShingles", "Wood Shingles", "Roof", 100, "Painted cedar shingles in overlapping courses: each shingle its own warm tone, deep shadow under each course.", ["roof", "shingles", "wood", "cedar", "cottage"])
def wood_shingles(m):
    P = std(m, wear=0.2, age=0.35, wear_doc="Curled, split shingles.", age_doc="Moss and silvering.")
    painterly(m, "#3e3050", "#ffe2a8", "#c88050", sheet="strokes_bristle")
    t = m.add("bricks", name="Courses", rows=6, columns=10, offset=0.5, mortar=0.025, bevel=0.04)
    grain = m.add("directional", name="Grain", angle=90.0, scaleAlong=2, scaleAcross=30, octaves=4)
    M = m.px("""
float3 bu = b.sample(uv).xyz;
float fall = 1.0 - bu.y;
float hgt = texToFloat(a.sample(uv)) * (0.45 + fall * 0.5);
return float4(hgt, c.sample(uv), texToFloat(d.sample(uv)), 1.0);""", name="Shingle", a=t["mask"], b=t["brickUv"], c=t["random"], d=grain)
    h = m.gray("0.1 + a.sample(uv).x * 0.7 + (a.sample(uv).z - 0.5) * 0.05", name="Height", a=M)
    v = lit_value(m, h, key=0.6, occlusion=1.2)
    st = stroke_sheet(m, BRISTLE, 5)
    base = paint(m, P, v, st, ["#30243e", "#6e4232", "#b06e44", "#e8b478"], spread=0.3)
    tone = m.px(f"""
float3 c0 = a.sample(uv).rgb * (0.85 + b.sample(uv).y * 0.3);
float mo = smoothstep(1.0 - p.y * 0.5, 1.2 - p.y * 0.5, b.sample(uv).y * 0.5 + (1.0 - b.sample(uv).x) * 0.6);
c0 = lerp(c0, {col("#6a8a48")}, mo * 0.7);
c0 = lerp(c0, dot(c0, float3(0.3, 0.5, 0.2)).xxx, p.y * 0.25);
return float4(c0, 1.0);""", name="Tone", a=base, b=M, y=P["age"])
    finish_painted(m, P, tone, h, rough=0.85, normal=0.7)


@mat("Thatch", "Thatch", "Roof", 120, "Painted straw thatch: combed bundles in golden courses, deep amber shadow between them.", ["thatch", "straw", "roof", "cottage", "golden"])
def thatch(m):
    P = std(m, wear=0.2, age=0.4, wear_doc="Ragged, thinned straw.", age_doc="Weathering to grey-brown.")
    painterly(m, "#5a3a48", "#fff0b0", "#e0b050", sheet="strokes_bristle")
    straw = m.add("directional", name="Straw", angle=90.0, scaleAlong=2, scaleAcross=80, octaves=4, persistence=0.6)
    bundles = m.add("directional", name="Bundles", angle=90.0, scaleAlong=1, scaleAcross=10, octaves=2)
    M = m.px("""
float course = frac(uv.y * 5.0 + texToFloat(b.sample(uv)) * 0.15);
float lip = smoothstep(0.0, 0.85, course);
float s = texToFloat(a.sample(uv));
float hgt = lip * (0.7 + s * 0.3) + (texToFloat(b.sample(uv)) - 0.5) * 0.15;
return float4(hgt, s, course, 1.0);""", name="Courses", a=straw, b=bundles)
    h = m.gray("0.1 + a.sample(uv).x * 0.7", name="Height", a=M)
    v = lit_value(m, h, key=0.5, occlusion=1.0, bias=0.05)
    st = stroke_sheet(m, BRISTLE, 6)
    base = paint(m, P, v, st, ["#4a2e30", "#9a6a34", "#d8a84a", "#f6e08a"], spread=0.3)
    age = m.px("""
float3 c0 = a.sample(uv).rgb;
c0 = lerp(c0, dot(c0, float3(0.3, 0.5, 0.2)).xxx * float3(1.0, 0.95, 0.88), p.y * 0.45);
return float4(c0, 1.0);""", name="Weathering", a=base, y=P["age"])
    finish_painted(m, P, age, h, rough=0.9, normal=0.8)


@mat("CloudSky", "Painted Sky", "Sky", 1000, "A painted summer sky for domes and backdrops: towering soft cumulus with warm sunlit tops and lilac bellies over a deepening blue. Tiles across, not up.", ["sky", "clouds", "backdrop", "dome", "cumulus"], tileable=False)
def cloud_sky(m):
    P = std(m, wear=0.4, age=0.2, wear_doc="Cloud cover.", age_doc="Evening warmth.")
    painterly(m, "#7a78b0", "#fff4dc", "#f0b8a0", contrast=0.25, extra={"painterlyStrength": 0.6})
    cl = m.add("clouds", name="Cumulus", scale=2, octaves=6, persistence=0.5, mode="billow")
    cl2 = m.add("clouds", name="Wisps", scale=6, octaves=5, persistence=0.55)
    M = m.px("""
float v = uv.y;
float band = smoothstep(0.15, 0.45, v) * (1.0 - smoothstep(0.75, 0.95, v));
float c0 = texToFloat(a.sample(uv)) * 0.8 + texToFloat(b.sample(uv)) * 0.3;
float cov = smoothstep(0.52 - p.x * 0.3, 0.66 - p.x * 0.3, c0 * (0.6 + band * 0.55));
float lit = saturate(0.5 + (texToFloat(a.sample(uv + float2(-0.01, 0.015))) - texToFloat(a.sample(uv))) * -6.0 + (v - 0.4) * 0.6);
return float4(cov, lit, v, 1.0);""", name="Clouds", a=cl, b=cl2, x=P["wear"])
    st = stroke_sheet(m, SOFT, 3)
    sky = m.px(f"""
float4 s = a.sample(uv);
float4 k = b.sample(uv);
float3 skyc = lerp({col("#cfe2e8")}, {col("#4f86c0")}, smoothstep(0.1, 0.95, s.z + (k.a - 0.5) * 0.06));
skyc = lerp(skyc, {col("#f6d8b0")}, (1.0 - smoothstep(0.0, 0.35, s.z)) * p.y);
float t = saturate(s.y + (k.a - 0.5) * 0.3 * (0.4 + p.z) + (k.b - 0.5) * 0.2);
t = (floor(t * 4.0) + smoothstep(0.3, 0.7, frac(t * 4.0))) / 4.0;
float3 cloud = t < 0.5 ? lerp({col("#9a94b8")}, {col("#e0d4dc")}, t / 0.5) : lerp({col("#e0d4dc")}, {col("#fff6e2")}, (t - 0.5) / 0.5);
cloud = lerp(cloud, cloud * {col("#ffd8b8")}, p.y * 0.5);
float3 c0 = lerp(skyc, cloud, s.x);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=st, z=P["var"], y=P["age"])
    em = m.px("return float4(a.sample(uv).rgb * 0.6, 1.0);", name="Emissive", a=sky)
    h = m.gray("0.5 + a.sample(uv).x * 0.02", name="Height", a=M)
    r = m.gray("0.95", name="Roughness")
    m.finish(sky, r, 0.0, h, normal=0.1, ao=(0.01, 0.01, 0.0), emissive=em)


@mat("PaintedWater", "Painted Water", "Water", 200, "Stylised pond and river water: deep teal to sky-blue, with crisp painted highlight squiggles and soft ripple bands.", ["water", "river", "pond", "lake", "ripples"])
def painted_water(m):
    P = std(m, wear=0.3, age=0.2, wear_doc="Choppiness: more highlight squiggles.", age_doc="Murk: greener, darker water.")
    painterly(m, "#1f4a6a", "#f8fbff", "#7ad0d0", contrast=0.5, extra={"specularTint": 0.2})
    wn = m.add("perlin", name="Swell", scale=3, octaves=4)
    rip = m.add("directional", name="Ripples", angle=0.0, scaleAlong=3, scaleAcross=30, octaves=3)
    M = m.px("""
float sw = texToFloat(a.sample(uv));
float r0 = texToFloat(b.sample(uv + float2(0.0, (sw - 0.5) * 0.06)));
float band = sin((uv.y * 22.0 + sw * 4.0) * 3.14159);
float glint = smoothstep(0.82 - p.x * 0.15, 0.86 - p.x * 0.15, r0) * smoothstep(0.3, 0.9, band);
return float4(sw, r0, glint, band * 0.5 + 0.5);""", name="Waves", a=wn, b=rip, x=P["wear"])
    st = stroke_sheet(m, SOFT, 4)
    base = m.px(f"""
float4 s = a.sample(uv);
float4 k = b.sample(uv);
float t = saturate(s.x * 0.6 + s.w * 0.25 + (k.a - 0.5) * 0.25 * (0.4 + p.z));
t = (floor(t * 4.0) + smoothstep(0.3, 0.7, frac(t * 4.0))) / 4.0;
float3 c0 = t < 0.5 ? lerp({col("#1d4660")}, {col("#2f7a8a")}, t / 0.5) : lerp({col("#2f7a8a")}, {col("#7ab8c8")}, (t - 0.5) / 0.5);
c0 = lerp(c0, c0 * {col("#a8b870")}, p.y * 0.5);
c0 = lerp(c0, {col("#f4f8f4")}, s.z);
return float4(c0, 1.0);""", name="BaseColor", a=M, b=st, z=P["var"], y=P["age"])
    h = m.gray("0.5 + (a.sample(uv).x - 0.5) * 0.04 + (a.sample(uv).w - 0.5) * 0.01", name="Height", a=M)
    r = m.gray("0.08 + (1.0 - a.sample(uv).z) * 0.04", name="Roughness", a=M)
    m.finish(base, r, 0.0, h, normal=0.5, ao=(0.01, 0.01, 0.0))


@mat("DirtPath", "Dirt Path", "Ground", 150, "A worn painted footpath: warm ochre earth, scattered pebbles, soft dappled shade, grass creeping in from the edges.", ["path", "dirt", "ground", "trail", "earth", "warm"])
def dirt_path(m):
    P = std(m, wear=0.3, age=0.3, wear_doc="Ruts and footworn hollows.", age_doc="Grass creeping in.")
    painterly(m, "#5a4060", "#fff0c0", "#e0a060")
    peb = m.px("""
float2 q = uv - 0.5;
float v = sqrt(saturate(1.0 - dot(q, q) / 0.16));
return float4(v, v, v, 1.0);""", name="Pebble")
    stones = m.add("splatter", name="Pebbles", pattern=peb, countX=8, countY=8, perCell=2, size=0.35, sizeRandom=0.6, valueRandom=0.2, blend="max")
    ground = m.add("clouds", name="Ground", scale=4, octaves=5)
    shade = m.add("cells", name="Dapple", scale=5, mode="f1", jitter=1.0)
    grassy = m.add("clouds", name="Grassy", scale=3, octaves=5)
    h = m.gray("0.3 + (texToFloat(a.sample(uv)) - 0.5) * 0.2 * (0.5 + p.x) + c.sample(uv) * 0.3", name="Height", a=ground, c=stones["out"], x=P["wear"])
    v = lit_value(m, h, key=0.6, occlusion=1.0, bias=0.08)
    dap = m.px("""
float v = texToFloat(a.sample(uv)) - smoothstep(0.35, 0.2, c.sample(uv)) * 0.18;
return float4(v, v, v, 1.0);""", name="Dappled", a=v, c=shade)
    st = stroke_sheet(m, SOFT, 5)
    earth = paint(m, P, dap, st, ["#5a4058", "#9a6a4a", "#d0a060", "#f2d8a0"], spread=0.3)
    grass = paint(m, P, dap, st, ["#2a4c4a", "#3f7a4a", "#79a84a", "#cfd67a"], spread=0.3, name="GrassPaint")
    base = m.px("""
float g = smoothstep(0.75 - p.y * 0.4, 0.85 - p.y * 0.4, texToFloat(c.sample(uv)));
return float4(lerp(a.sample(uv).rgb, b.sample(uv).rgb, g), 1.0);""", name="BaseColor", a=earth, b=grass, c=grassy, y=P["age"])
    finish_painted(m, P, base, h, rough=0.9, normal=0.6)


@mat("FlowerMeadow", "Flower Meadow", "Grass", 150, "Painted meadow grass sprinkled with flowers: daisies, buttercups, pink clover and blue speedwell, each a few bright dabs.", ["grass", "flowers", "meadow", "wildflowers", "spring"])
def flower_meadow(m):
    P = std(m, wear=0.15, age=0.2, wear_doc="Fewer flowers.", age_doc="Late-summer gold in the grass.")
    painterly(m, "#2f5a5a", "#f8eeb0", "#f0a0c0")
    tuft = m.px("""
float2 q = uv - 0.5;
float r = length(q * float2(1.0, 1.4));
float v = sqrt(saturate(1.0 - r / 0.45)) * (0.6 + 0.4 * saturate(0.5 - q.y));
return float4(v, v, v, 1.0);""", name="Tuft")
    tufts = m.add("splatter", name="Tufts", pattern=tuft, countX=10, countY=10, perCell=6, size=0.8, sizeRandom=0.5, rotationRandom=0.15, valueRandom=0.3, blend="max")
    h0 = m.gray("0.25 + c.sample(uv) * 0.5", name="GrassHeight", c=tufts["out"])
    v = lit_value(m, h0, key=0.6, occlusion=0.8)
    st = stroke_sheet(m, SOFT, 6)
    grass = paint(m, P, v, st, ["#2a4c4a", "#3f7a4a", "#79a84a", "#cfd67a"])
    flower = m.px("""
float2 q = uv - 0.5;
float ang = atan2(q.y, q.x);
float r = length(q);
float petal = 0.32 + 0.12 * cos(ang * 5.0);
float v = 1.0 - smoothstep(petal - 0.04, petal, r);
float centre = 1.0 - smoothstep(0.08, 0.11, r);
return float4(v, centre, v, 1.0);""", name="Flower")
    flowers = m.add("splatter", name="Flowers", pattern=m.gray("a.sample(uv).r", name="FlowerShape", a=flower), countX=9, countY=9, perCell=2, size=0.4, sizeRandom=0.4, rotationRandom=1.0, valueRandom=0.0, blend="top")
    base = m.px(f"""
float4 rc = b.sample(uv);
float fl = c.sample(uv) * step(p.x * 0.8, rc.g);
float k = rc.r;
float3 fc = k < 0.4 ? {col("#f6f2e8")} : (k < 0.65 ? {col("#f2d040")} : (k < 0.85 ? {col("#e888b0")} : {col("#7090e0")}));
float3 c0 = lerp(a.sample(uv).rgb, fc * (0.85 + fl * 0.2), step(0.3, fl));
return float4(c0, 1.0);""", name="Blooms", a=grass, b=flowers["color"], c=flowers["out"], x=P["wear"])
    h = m.gray("0.25 + c.sample(uv) * 0.5 + step(0.3, d.sample(uv)) * 0.2", name="Height", c=tufts["out"], d=flowers["out"])
    finish_painted(m, P, base, h, rough=0.85, normal=0.6)
