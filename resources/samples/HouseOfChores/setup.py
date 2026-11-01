"""Turns the downloaded Blender 4.3 splash file into houseofchores.lila. Lila runs it after the download with SOURCE (the downloaded folder) and OUT
(the .lila to write) set; to run it by hand, set both first."""

import os

from lila import ops

src = next((os.path.join(root, f) for root, _, files in os.walk(SOURCE) for f in files if f == "blender-4.3-splash.blend"), None)
if not src:
    raise RuntimeError("blender-4.3-splash.blend is not in the download")

ops.newScene()
# Modifiers and geometry nodes Lila can't rebuild are baked by an installed Blender, when there is one.
ops.importFile(src, options={"evaluate": "unsupported"})
ops.select(clear=True)
ops.saveScene(OUT)
