"""Builds the Spike Burst demo: a geometry nodes VFX meant to be sent to Unreal as a Niagara mesh flipbook.

Run with `lilabatch --batch build_spike_burst.py [out_dir]`, or from the script editor. Writes SpikeBurst.lila and
SpikeBurstFX.lilanodes (the node group on its own, to share) next to this file, or into out_dir.
"""

import os
import sys

from lila import ops

FPS = 30.0
FRAMES = 27  # one burst, 0.9 s: the length of the flipbook

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
out_dir = sys.argv[1] if len(sys.argv) > 1 else here

spec = {
    "name": "SpikeBurstFX",
    "description": "Jagged spikes bursting out of a dome, for Niagara mesh flipbooks. Key Growth and Evolution.",
    "inputs": [
        {"name": "growth", "type": "float", "default": 1.0, "min": 0, "max": 2, "softMax": 1, "subtype": "factor",
         "label": "Growth", "description": "How far the spikes are out. Key 0 to 1 for the burst.", "panel": "Animation"},
        {"name": "evolution", "type": "float", "default": 0.0, "label": "Evolution",
         "description": "Moves through the cell pattern so the spikes change shape over time. Key it.", "panel": "Animation"},
        {"name": "spikeLength", "type": "float", "default": 120.0, "min": 0, "softMax": 300, "subtype": "distance",
         "label": "Spike Length", "description": "How far the longest spikes reach.", "panel": "Spikes"},
        {"name": "density", "type": "float", "default": 0.04, "min": 0.001, "softMax": 0.1, "step": 0.005,
         "label": "Density", "description": "Cells per cm: higher gives more, thinner spikes.", "panel": "Spikes"},
        {"name": "sharpness", "type": "float", "default": 3.0, "min": 0.25, "softMax": 6,
         "label": "Sharpness", "description": "Higher keeps only the tallest peaks, for fewer, thinner needles.", "panel": "Spikes"},
        {"name": "randomness", "type": "float", "default": 0.7, "min": 0, "max": 1, "subtype": "factor",
         "label": "Randomness", "description": "How irregular the cell layout is.", "panel": "Spikes"},
        {"name": "baseFalloff", "type": "float", "default": 0.15, "min": 0, "max": 1, "subtype": "factor",
         "label": "Base Spikes", "description": "Spike length at the base relative to the tip.", "panel": "Spikes"},
        {"name": "gradientEnd", "type": "float", "default": 160.0, "min": 1, "softMax": 500, "subtype": "distance",
         "label": "Gradient Length", "description": "Distance from the origin where the red vertex colour reaches 1 (Niagara reads it as the fade mask).",
         "panel": "Colour"},
    ],
    "nodes": [
        # Minkowski cells with an exponent under 1 are star-shaped, which is what makes the spikes.
        {"name": "Cells", "type": "VoronoiTexture", "props": {"dimensions": "4D", "distance": "minkowski"},
         "values": {"detail": 0, "exponent": 0.3}},
        {"name": "Norm", "type": "MapRange", "props": {"clamp": "true"}, "values": {"fromMin": 0, "fromMax": 28, "toMin": 0, "toMax": 1}},
        {"name": "Peaks", "type": "Math", "props": {"operation": "power"}},
        {"name": "Reach", "type": "Math", "props": {"operation": "multiply"}},
        {"name": "Grow", "type": "Math", "props": {"operation": "multiply"}},
        {"name": "Pos", "type": "Position"},
        {"name": "Split", "type": "SeparateXYZ"},
        {"name": "Below", "type": "Compare", "props": {"operation": "lessThan"}, "values": {"b": -1.0}},
        {"name": "Dome", "type": "DeleteGeometry", "props": {"domain": "point"}},
        {"name": "Height", "type": "MapRange", "values": {"fromMin": 0, "fromMax": 40, "toMax": 1}},
        {"name": "Taper", "type": "Mix", "props": {"dataType": "float"}, "values": {"b": 1.0}},
        {"name": "Shaped", "type": "Math", "props": {"operation": "multiply"}},
        {"name": "Normal", "type": "Normal"},
        {"name": "Push", "type": "VectorMath", "props": {"operation": "scale"}},
        {"name": "Spikes", "type": "SetPosition"},
        {"name": "Dist", "type": "VectorMath", "props": {"operation": "length"}},
        {"name": "Fade", "type": "MapRange", "values": {"fromMin": 0, "toMin": 0, "toMax": 1}},
        {"name": "Colour", "type": "CombineColor", "values": {"green": 0, "blue": 0, "alpha": 1}},
        {"name": "Store", "type": "StoreNamedAttribute", "props": {"dataType": "color", "domain": "point"}, "values": {"name": "Col"}},
        {"name": "Pos2", "type": "Position"},
        {"name": "Round", "type": "VectorMath", "props": {"operation": "normalize"}},
        {"name": "Soft", "type": "SetMeshNormal", "props": {"mode": "free", "domain": "point"}},
    ],
    "links": [
        "GroupInput.evolution -> Cells.w", "GroupInput.density -> Cells.scale",
        "GroupInput.randomness -> Cells.randomness",
        "Cells.distance -> Norm.value", "Norm.result -> Peaks.a", "GroupInput.sharpness -> Peaks.b",
        "Peaks.value -> Reach.a", "GroupInput.spikeLength -> Reach.b",
        "Reach.value -> Grow.a", "GroupInput.growth -> Grow.b",
        "Pos.value -> Split.vector", "Split.y -> Height.value",
        "GroupInput.baseFalloff -> Taper.a", "Height.result -> Taper.factor",
        "Grow.value -> Shaped.a", "Taper.result -> Shaped.b",
        "Normal.value -> Push.a", "Shaped.value -> Push.scale",
        "Split.y -> Below.a", "GroupInput.geometry -> Dome.geometry", "Below.result -> Dome.selection",
        "Dome.geometry -> Spikes.geometry", "Push.vector -> Spikes.offset",
        "Spikes.geometry -> Store.geometry",
        "Pos.value -> Dist.a", "Dist.value -> Fade.value", "GroupInput.gradientEnd -> Fade.fromMax",
        "Fade.result -> Colour.red", "Colour.color -> Store.value",
        "Store.geometry -> Soft.mesh", "Pos2.value -> Round.a", "Round.vector -> Soft.customNormal",
        "Soft.mesh -> GroupOutput.geometry",
    ],
}

ops.newScene()
ops.playbackOptions(fps=FPS, minTime=0.0, maxTime=(FRAMES - 1) / FPS, animationStartTime=0.0, animationEndTime=(FRAMES - 1) / FPS)

cone = ops.polySphere(name="SpikeBurst", radius=40, subdivisionsAxis=96, subdivisionsHeight=64)[0]
info = ops.gnBuildGroup(spec)
mod = ops.gnApply(cone, "SpikeBurstFX")

# The burst: spikes shoot out over 6 frames and ease to a stop, while the cells keep evolving.
f = lambda frame: frame / FPS
ops.setKeyframe(f"{mod}.growth", time=f(0), value=0.0)
ops.setKeyframe(f"{mod}.growth", time=f(6), value=1.0)
ops.setKeyframe(f"{mod}.growth", time=f(FRAMES - 1), value=1.15)
ops.setKeyframe(f"{mod}.evolution", time=f(0), value=0.0)
ops.setKeyframe(f"{mod}.evolution", time=f(FRAMES - 1), value=1.2)
ops.keyTangent(f"{mod}.growth", time=f(6), inTangentType="flat", outTangentType="flat")

ops.currentTime(f(10))
ops.gnExportGroup("SpikeBurstFX", os.path.join(out_dir, "SpikeBurstFX.lilanodes"))
ops.saveScene(os.path.join(out_dir, "SpikeBurst.lila"))
print("SPIKE", mod, info.get("name", info) if isinstance(info, dict) else info)
