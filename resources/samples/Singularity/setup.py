"""Turns the downloaded Blender 5.1 splash file into singularity.lila. Lila runs it after the download with SOURCE (the downloaded folder) and OUT
(the .lila to write) set; to run it by hand, set both first."""

import os

from lila import ops

src = next((os.path.join(root, f) for root, _, files in os.walk(SOURCE) for f in files if f == "blender-5.1-splash.blend"), None)
if not src:
    raise RuntimeError("blender-5.1-splash.blend is not in the download")

ops.newScene()
ops.importFile(src)
ops.select(clear=True)
ops.saveScene(OUT)
