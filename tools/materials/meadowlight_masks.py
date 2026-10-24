"""Meadowlight smart masks (.lilasmart, kind mask): the painted-light masks a hand-painted look layers colour through.

Each binds to the set's baked mesh maps by role, so it adapts to any mesh. The description says what to fill it with.
"""

import json

from matgraph import ROOT

OUT = ROOT / "extensions/Meadowlight/resources/smartmaterials"


def gen(mid, type, values=None, bindings=None, mode="normal", opacity=1.0, invert=False):
    m = {"id": mid, "kind": "generator", "type": type, "mode": mode, "opacity": opacity, "invert": invert, "values": values or {}}
    if bindings:
        m["bindings"] = bindings
    return m


def levels(mid, lo, hi, gamma=1.0):
    return {"id": mid, "kind": "levels", "mode": "normal", "opacity": 1.0, "invert": False, "values": {"inLow": lo, "inHigh": hi, "gamma": gamma}}


MASKS = {
    "meadowSunSideWarmth": ("Sun-Side Warmth", "Faces turned to an afternoon sun from the upper left, less where occluded. Fill with a warm gold or peach (screen or soft light, low opacity) for sunlit warmth.",
                            [gen("M1", "genSunBleach", {"azimuth": 135.0, "elevation": 50.0, "aoInfluence": 0.6, "level": 0.45, "softness": 0.8})]),
    "meadowCoolShadowTint": ("Cool Shadow Tint", "Everything turned away from the sun, deepening in occlusion. Fill with a cool lilac or blue-violet (multiply or overlay) so shadows read cool against warm light.",
                             [gen("M1", "genSunBleach", {"azimuth": 135.0, "elevation": 50.0, "aoInfluence": 0.8, "level": 0.55, "softness": 0.9}, invert=True)]),
    "meadowTopHighlights": ("Top Highlights", "Upward-facing surfaces, tightened to the tops. Fill with a pale sky-lit cream or lime (screen) for the painted top light on foliage, rocks and roofs.",
                            [gen("M1", "maskFacing", {"elevation": 90.0}, {"normal": "worldNormal"}), levels("M2", 0.55, 0.95)]),
    "meadowGroundBounce": ("Ground Bounce", "Downward-facing surfaces, catching light bounced up from the ground. Fill with a warm earthy orange or grass green (screen, low opacity).",
                           [gen("M1", "maskFacing", {"elevation": -90.0}, {"normal": "worldNormal"}), levels("M2", 0.2, 0.9, 0.8)]),
    "meadowPaintedAo": ("Painted AO", "Soft painted occlusion in creases and contact areas. Fill with a desaturated plum or deep teal (multiply) rather than black, the way a painter darkens.",
                        [gen("M1", "genAoDarken", {"level": 0.35, "softness": 0.9}), levels("M2", 0.0, 0.85, 1.2)]),
}


def write_all():
    OUT.mkdir(parents=True, exist_ok=True)
    written = []
    for stem, (name, description, masks) in MASKS.items():
        doc = {"format": "lilasmart", "version": 1, "name": name, "description": description, "kind": "mask", "masks": masks}
        path = OUT / f"{stem}.lilasmart"
        path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n")
        written.append(path)
    return written
