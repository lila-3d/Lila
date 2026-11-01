"""Turns the downloaded Flight Helmet glTF into flight_helmet.lila. Lila runs it after the download with SOURCE (the downloaded folder) and OUT
(the .lila to write) set; to run it by hand, set both first."""

import os

from lila import ops

src = next((os.path.join(root, f) for root, _, files in os.walk(SOURCE) for f in files if f == "FlightHelmet.gltf"), None)
if not src:
    raise RuntimeError("FlightHelmet.gltf is not in the download")

ops.newScene()
ops.importFile(src)
ops.select(clear=True)
ops.saveScene(OUT)
