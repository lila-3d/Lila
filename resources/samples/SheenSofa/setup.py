"""Turns the downloaded Sheen Wood Leather Sofa into sheen_sofa.lila. Lila runs it after the download with SOURCE (the downloaded folder) and OUT
(the .lila to write) set; to run it by hand, set both first."""

import os

from lila import ops

src = next((os.path.join(root, f) for root, _, files in os.walk(SOURCE) for f in files if f == "SheenWoodLeatherSofa.glb"), None)
if not src:
    raise RuntimeError("SheenWoodLeatherSofa.glb is not in the download")

ops.newScene()
ops.importFile(src)
ops.select(clear=True)
ops.saveScene(OUT)
