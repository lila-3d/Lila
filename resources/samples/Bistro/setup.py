"""Turns the unpacked Bistro archive into bistro.lila. Lila runs it after the download with SOURCE (the unpacked folder)
and OUT (the .lila to write) set; to run it by hand, set both first."""

import os

from lila import ops

fbx = next((os.path.join(root, f) for root, _, files in os.walk(SOURCE) for f in files if f == "BistroExterior.fbx"), None)
if not fbx:
    raise RuntimeError("BistroExterior.fbx is not in the download")

ops.newScene()
ops.importFile(fbx)
# The archive's sun is warm and 150 times the environment, tuned for another renderer; this one keeps the clay neutral.
sun = (ops.ls(type="light") or [None])[0]
if sun:
    ops.setAttr(sun + ".color", [1.0, 1.0, 1.0])
    ops.setAttr(sun + ".intensity", 8.0)
# Shown only where a map can't load, such as the archive's DDS textures: mid gray rather than white.
for m in ops.ls(type="material"):
    if ops.getAttr(m + ".baseColorMap") and ops.getAttr(m + ".baseColor") == [1.0, 1.0, 1.0]:
        ops.setAttr(m + ".baseColor", [0.5, 0.5, 0.5])
post = ops.postProcess(name="post")
ops.setAttr(post + ".exposure", -1.3)
ops.select(clear=True)
ops.saveScene(OUT)
