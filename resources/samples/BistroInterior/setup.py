"""Turns the unpacked Bistro archive's interior into bistro_interior.lila. Lila runs it after the download with SOURCE
(the unpacked folder) and OUT (the .lila to write) set; to run it by hand, set both first."""

import os

from lila import ops

fbx = next((os.path.join(root, f) for root, _, files in os.walk(SOURCE) for f in files if f == "BistroInterior_Wine.fbx"), None)
if not fbx:
    raise RuntimeError("BistroInterior_Wine.fbx is not in the download")

ops.newScene()
ops.importFile(fbx)

# The archive leaves its glass and drinks to a path tracer's transmission; here they blend: (base colour, opacity).
SEE_THROUGH = {
    "TransparentGlass": ((0.9, 0.95, 0.95), 0.12),
    "TransparentGlassWine": ((0.08, 0.18, 0.08), 0.55),
    "Water": ((0.85, 0.92, 1.0), 0.15),
    "White_Wine": ((0.85, 0.7, 0.3), 0.45),
    "Red_Wine": ((0.1, 0.004, 0.012), 0.93),
    "Beer": ((0.8, 0.5, 0.1), 0.75),
}
for name, (color, opacity) in SEE_THROUGH.items():
    if not ops.objExists(name):
        continue
    ops.setAttr(name + ".blendMode", "translucent")
    ops.setAttr(name + ".baseColor", list(color))
    ops.setAttr(name + ".opacity", opacity)
    ops.setAttr(name + ".roughness", 0.05)
    ops.setAttr(name + ".twoSided", True)

ops.select(clear=True)
ops.saveScene(OUT)
