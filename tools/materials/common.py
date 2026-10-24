"""Shared pieces for the material sets: the registry, standard variation parameters and recurring sub-graphs."""

from matgraph import Material, col, f

REGISTRY = []


def material(ident, name, category, physical, scale_cm, description, tags, tileable=True, stem=None):
    """Registers a builder: fn(m) fills the Material."""

    def deco(fn):
        REGISTRY.append((stem or ident[0].lower() + ident[1:], lambda: _build(fn, ident, name, category, physical, scale_cm, description, tags, tileable)))
        return fn

    return deco


def _build(fn, ident, name, category, physical, scale_cm, description, tags, tileable):
    m = Material(ident, name, category, physical, scale_cm, description, tags, tileable)
    fn(m)
    return m


def std(m, wear=0.3, age=0.2, var=0.5, hue=True, wear_doc="Scratches and worn spots.", age_doc="Grime and discolouration."):
    """The variation parameters every material exposes."""
    P = {
        "wear": m.param("wear", default=wear, label="Wear", description=wear_doc),
        "age": m.param("age", default=age, label="Age", description=age_doc),
        "var": m.param("colorVariation", default=var, label="Colour Variation", description="How much the colour wanders across the surface."),
    }
    if hue:
        P["hue"] = m.param("hue", default=0.0, lo=-0.5, hi=0.5, label="Hue", description="Shifts the base colour's hue, in turns.")
    m.presets = {
        "Pristine": {"wear": 0.0, "age": 0.0},
        "Default": {"wear": wear, "age": age, "colorVariation": var},
        "Weathered": {"wear": min(1.0, wear + 0.4), "age": min(1.0, age + 0.45)},
    }
    return P


def cavity(m, height, radius=0.01, name="Cavity"):
    """0 on flats and peaks, rising in dips: the height's blurred self minus itself."""
    blurred = m.add("blur", name=name + "Blur", input=height, radius=radius)
    return m.px("float d = texToFloat(a.sample(uv)) - texToFloat(b.sample(uv));\nfloat v = saturate(d * p.x);\nreturn float4(v, v, v, 1.0);",
                name=name, a=blurred, b=height, x=12.0)


def grime_noise(m, scale=5, name="Grime"):
    """Broad blotches with fine speckle, 0 to 1, for dirt that age reveals."""
    return m.add("dirt", name=name, scale=scale, amount=0.5, contrast=1.0, grain=0.4)


def lin(hexstr):
    """'#rrggbb' sRGB as a linear [r, g, b], for material node colour settings."""
    h = hexstr.lstrip("#")
    out = []
    for i in (0, 2, 4):
        c = int(h[i : i + 2], 16) / 255.0
        out.append(round(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4, 4))
    return out


# Slang snippets ---------------------------------------------------------------------------------------------------------

def reveal(mask, amount, soft=0.15):
    """Slang: a mask grown by amount 0..1 (0 none, 1 everywhere) with a soft edge."""
    return f"smoothstep(1.0 - ({amount}) * (1.0 + {f(soft)}), 1.0 - ({amount}) * (1.0 + {f(soft)}) + {f(soft)}, {mask})"
