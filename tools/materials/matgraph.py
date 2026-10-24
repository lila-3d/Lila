"""A small builder for .lilamat texture graphs, used by make_materials.py.

A material is authored as a node group ("Material") with one output per channel, wrapped in a top-level graph that links
those outputs to the Material Output. Exposed parameters live on the group (so the node editor and direct evaluation see
them) and are mirrored as top-level param inputs targeting it (so the material works as a procedural layer too).
"""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

CHANNEL_TYPES = {
    "baseColor": "color",
    "roughness": "gray",
    "metallic": "gray",
    "normal": "color",
    "height": "gray",
    "ao": "gray",
    "emissive": "color",
    "opacity": "gray",
}

DEFAULT_SETTINGS = {
    "channels": [
        {"name": "baseColor", "type": "color", "bits": 8},
        {"name": "roughness", "type": "gray", "bits": 8},
        {"name": "metallic", "type": "gray", "bits": 8},
        {"name": "normal", "type": "color", "bits": 8},
        {"name": "height", "type": "gray", "bits": 16},
        {"name": "ao", "type": "gray", "bits": 8},
        {"name": "emissive", "type": "color", "bits": 8},
    ],
    "width": 2048,
    "height": 2048,
    "tiles": [{"udim": 1001}],
    "normalConvention": "opengl",
    "bakeProject": "",
    "exportPreset": "unreal",
    "material": "",
}


def _load_defs():
    defs = {}
    for folder in ("resources/texnodes", "resources/texgenerators"):
        for f in sorted((ROOT / folder).rglob("*.lilatexnode")):
            d = json.loads(f.read_text(encoding="utf-8"))
            for n in d.get("nodes", [d]):
                defs[n["type"]] = {
                    "inputs": {i["id"]: i["type"] for i in n.get("inputs", [])},
                    "outputs": [o["id"] for o in n.get("outputs", [])],
                    "params": {p["id"]: p["type"] for p in n.get("params", [])},
                }
    return defs


DEFS = _load_defs()


TO_LINEAR = """
float4 c = a.sample(uv);
float3 s = saturate(c.rgb);
return float4(lerp(pow((s + 0.055) / 1.055, 2.4), s / 12.92, step(s, 0.04045)), c.a);"""

TO_SRGB = """
float4 c = a.sample(uv);
float3 l = saturate(c.rgb);
return float4(lerp(1.055 * pow(l, 1.0 / 2.4) - 0.055, l * 12.92, step(l, 0.0031308)), c.a);"""


def srgb_to_linear(hexstr):
    h = hexstr.lstrip("#")
    out = []
    for i in (0, 2, 4):
        c = int(h[i : i + 2], 16) / 255.0
        out.append(round(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4, 5))
    return out


def col(hexstr):
    """'#rrggbb' as a Slang float3 literal, sRGB-encoded: materials do their colour math in sRGB and finish() converts to linear."""
    h = hexstr.lstrip("#")
    r, g, b = (int(h[i : i + 2], 16) / 255.0 for i in (0, 2, 4))
    return f"float3({r:.4f}, {g:.4f}, {b:.4f})"


def f(v):
    return f"{float(v):.4f}"


class Param:
    def __init__(self, name, type, default, lo=None, hi=None, label=None, description="", options=None):
        self.name = name
        self.type = type
        self.default = default
        self.lo = lo
        self.hi = hi
        self.label = label or re.sub(r"(?<!^)([A-Z])", r" \1", name).capitalize()
        self.description = description
        self.options = options
        self.targets = []

    def decl(self, targets):
        d = {"name": self.name, "kind": "param", "type": self.type, "default": self.default, "label": self.label}
        if self.description:
            d["description"] = self.description
        if self.lo is not None:
            d["min"] = self.lo
        if self.hi is not None:
            d["max"] = self.hi
        if self.options:
            d["options"] = self.options
        d["targets"] = targets
        return d


class Sock:
    def __init__(self, node, socket):
        self.node = node
        self.socket = socket


class Node:
    def __init__(self, mat, name, type):
        self.mat = mat
        self.name = name
        self.type = type

    def __getitem__(self, socket):
        if socket not in DEFS[self.type]["outputs"]:
            raise KeyError(f"{self.type} has no output {socket}")
        return Sock(self.name, socket)

    def first(self):
        return Sock(self.name, DEFS[self.type]["outputs"][0])


class Material:
    def __init__(self, ident, name, category, physical, scale_cm, description, tags, tileable=True):
        self.ident = ident
        self.meta = {
            "name": name,
            "category": category,
            "tags": tags,
            "physical": physical,
            "scaleCm": scale_cm,
            "tileable": tileable,
            "seedParam": "seed",
            "variationParams": [],
            "author": "Lila",
            "licence": "CC0-1.0",
            "version": "1.0",
            "description": description,
        }
        self.nodes = []
        self.links = []
        self.params = {}
        self.outputs = {}
        self.settings = None
        self.counts = {}
        self.presets = {}
        self.seed = self.param("seed", "seed", 0, label="Seed", description="Changes the pattern without changing the look.")

    # Parameters ---------------------------------------------------------------------------------------------------

    def param(self, name, type="float", default=0.5, lo=0.0, hi=1.0, label=None, description="", variation=True):
        if type == "seed":
            lo = hi = None
        if type == "color" and isinstance(default, str):
            # Graph colours are linear; authored as sRGB hex.
            default = srgb_to_linear(default) + [1.0]
        p = Param(name, type, default, lo, hi, label, description)
        self.params[name] = p
        if variation and type != "seed":
            self.meta["variationParams"].append(name)
        return p

    # Nodes --------------------------------------------------------------------------------------------------------

    def add(self, type, name=None, **values):
        if type not in DEFS:
            raise KeyError(f"unknown node type {type}")
        d = DEFS[type]
        base = name or type[0].upper() + type[1:]
        self.counts[base] = self.counts.get(base, 0) + 1
        nm = base if self.counts[base] == 1 else f"{base}{self.counts[base]}"
        node = Node(self, nm, type)
        vals = {}
        for k, v in values.items():
            if k not in d["inputs"] and k not in d["params"]:
                raise KeyError(f"{type} has no input or parameter {k}")
            if isinstance(v, Node):
                v = v.first()
            if isinstance(v, Sock):
                if k not in d["inputs"]:
                    raise KeyError(f"{type}.{k} is a parameter, it can't take a link")
                self.links.append([v.node, v.socket, nm, k])
            elif isinstance(v, Param):
                v.targets.append(f"{nm}.{k}")
                vals[k] = v.default
            else:
                vals[k] = v
        if "seed" in d["params"] and "seed" not in values:
            self.seed.targets.append(f"{nm}.seed")
        self.nodes.append({"name": nm, "type": type, "values": vals})
        return node

    def px(self, code, name="Pixel", **values):
        """A Pixel Processor: a, b colour inputs, c, d gray inputs, x, y, z, w values."""
        return self.add("pixelProcessor", name=name, expression=code.strip(), **values)

    def gray(self, expr, name="Gray", **values):
        return self.px(f"float v = {expr};\nreturn float4(v, v, v, 1.0);", name=name, **values)

    def color(self, param, name="Color"):
        """A colour parameter's node, converted to sRGB so the authoring math (all in sRGB) can mix it."""
        node = self.add("color", name=name, color=param)
        return self.px(TO_SRGB, name=name + "Srgb", a=node)

    def out(self, **channels):
        for ch, src in channels.items():
            if ch not in CHANNEL_TYPES:
                raise KeyError(ch)
            self.outputs[ch] = src.first() if isinstance(src, Node) else src

    # Common tails ---------------------------------------------------------------------------------------------------

    def finish(self, base, roughness, metallic, height, normal=1.0, ao=(0.03, 0.08, 1.0), hue=None, detail=None, **extra):
        """Normal from height (RNM with a detail normal when given), AO from height, an optional hue shift on base colour."""
        height, base = self._unused_variation(height, base)
        n = self.add("heightToNormal", name="Normal", height=height, intensity=normal)
        if detail is not None:
            n = self.add("normalCombine", name="DetailNormal", base=n["normal"], detail=detail, mode="rnm")
        occ = self.add("ambientOcclusion", name="Occlusion", height=height, radius=ao[0], depth=ao[1], strength=ao[2])
        if hue is not None:
            base = self.add("hsv", name="Tint", input=base, hue=hue)
        base = self.px(TO_LINEAR, name="BaseColorLinear", a=base)
        roughness = self.gray("saturate(texToFloat(a.sample(uv)))", name="RoughnessClamp", a=roughness)
        if "emissive" in extra:
            extra["emissive"] = self.px(TO_LINEAR, name="EmissiveLinear", a=extra["emissive"])
        if isinstance(metallic, (int, float)):
            metallic = self.add("constant", name="Metallic", value=float(metallic))
        self.out(baseColor=base, roughness=roughness, metallic=metallic, height=height, normal=n, ao=occ, **extra)

    def _unused_variation(self, height, base):
        """Gives a standard variation parameter the material's own recipe left unused a generic meaning."""
        wear, age, var = (self.params.get(k) for k in ("wear", "age", "colorVariation"))
        if wear is not None and not wear.targets:
            wear.description = "Softens the relief, as if worn down."
            height = self.gray("0.5 + (texToFloat(a.sample(uv)) - 0.5) * (1.0 - p.x * 0.6)", name="WornRelief", a=height, x=wear)
        if age is not None and not age.targets:
            age.description = "Darkens and dulls the colour."
            base = self.px("float4 c = a.sample(uv);\nreturn float4(lerp(c.rgb, c.rgb * 0.85, p.y), c.a);", name="Aged", a=base, y=age)
        if var is not None and not var.targets:
            var.description = "Broad tonal variation across the surface."
            blot = self.add("clouds", name="VariationNoise", scale=3, octaves=5)
            base = self.px("float4 c = a.sample(uv);\nreturn float4(c.rgb * (1.0 + (texToFloat(b.sample(uv)) - 0.5) * 0.25 * p.z), c.a);", name="Variation", a=base, b=blot, z=var)
        return height, base

    # Output -------------------------------------------------------------------------------------------------------

    def _layout(self, nodes, links, sink):
        depth = {n["name"]: 0 for n in nodes}
        changed = True
        while changed:
            changed = False
            for a, _, b, _ in links:
                if depth[b] < depth[a] + 1:
                    depth[b] = depth[a] + 1
                    changed = True
        depth[sink] = max(depth.values()) + 1
        rows = {}
        for n in nodes:
            d = depth[n["name"]]
            r = rows.get(d, 0)
            rows[d] = r + 1
            n["position"] = [d * 240.0, r * 160.0]

    def to_json(self):
        group_nodes = [dict(n) for n in self.nodes]
        group_links = [list(l) for l in self.links]
        outputs = []
        for ch, sock in self.outputs.items():
            group_links.append([sock.node, sock.socket, "GroupOutput", ch])
            outputs.append({"name": ch, "type": CHANNEL_TYPES[ch]})
        group_nodes.append({"name": "GroupOutput", "type": "GroupOutput", "values": {}})
        self._layout(group_nodes, group_links, "GroupOutput")
        for n in group_nodes:
            n["position"], pos = None, n["position"]
            n.pop("position")
            n["position"] = pos
        params = [p for p in self.params.values() if p.targets or p.name == "seed"]
        group = {
            "name": self.ident,
            "typeVersion": 1,
            "description": self.meta["description"],
            "inputs": [p.decl(p.targets) for p in params],
            "outputs": outputs,
            "nodes": [{"name": n["name"], "type": n["type"], "position": n["position"], "values": n["values"]} for n in group_nodes],
            "links": group_links,
        }
        mat_values = {"group": self.ident}
        mat_values.update({p.name: p.default for p in params})
        top = {
            "name": self.ident,
            "typeVersion": 1,
            "description": self.meta["description"],
            "inputs": [p.decl([f"Material.{p.name}"]) for p in params],
            "nodes": [
                {"name": "Material", "type": "Group", "position": [0.0, 0.0], "values": mat_values},
                {"name": "Output", "type": "materialOutput", "position": [320.0, 0.0], "values": {}},
            ],
            "links": [["Material", ch, "Output", ch] for ch in self.outputs],
            "subgroups": {self.ident: group},
        }
        self.meta["variationParams"] = [v for v in self.meta["variationParams"] if v in {p.name for p in params}]
        doc = {"format": "lilamat", "version": 1, "name": self.ident, "meta": self.meta, "graph": top}
        if self.presets:
            doc["presets"] = self.presets
        if self.settings:
            doc["settings"] = self.settings
        return doc

    def with_channel(self, name, type="gray", bits=8):
        s = json.loads(json.dumps(self.settings or DEFAULT_SETTINGS))
        if not any(c["name"] == name for c in s["channels"]):
            s["channels"].append({"name": name, "type": type, "bits": bits})
        self.settings = s


def write(mat, folder, stem):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"{stem}.lilamat"
    path.write_text(json.dumps(mat.to_json(), indent=1) + "\n", encoding="utf-8", newline="\n")
    return path
