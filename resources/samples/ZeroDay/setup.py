"""Turns the unpacked Zero-Day archive's Measure One into zero_day_measure_one.lila. Lila runs it after the download with SOURCE (the unpacked
folder) and OUT (the .lila to write) set; to run it by hand, set both first."""

import os

from lila import ops

fbx = next((os.path.join(root, f) for root, _, files in os.walk(SOURCE) for f in files if f == "MEASURE_ONE.fbx"), None)
if not fbx:
    raise RuntimeError("MEASURE_ONE.fbx is not in the download")

ops.newScene()
ops.importFile(fbx)
# Its white surfaces are made for emissive lighting alone; under the viewport's environment they need a stop less.
post = ops.postProcess(name="post")
ops.setAttr(post + ".exposure", -1.0)
ops.select(clear=True)
ops.saveScene(OUT)
