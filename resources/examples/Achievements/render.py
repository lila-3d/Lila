"""Render each achievement's beauty and matte passes for compose.py. Run with `lilabatch --batch render.py`, or from
the script editor.

ACH_OUT sets the output folder (default: lila-achievements/passes in the temp folder), ACH_ONLY a comma-separated
list of API names to render.
"""

import os
import sys
import tempfile
import traceback

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
sys.path.insert(0, here)

import achievements as A
from table import ACHIEVEMENTS
from lila import ops

out = os.environ.get("ACH_OUT", os.path.join(tempfile.gettempdir(), "lila-achievements", "passes"))
only = {n for n in os.environ.get("ACH_ONLY", "").split(",") if n}
os.makedirs(out, exist_ok=True)

failed = []
for api, *_ in ACHIEVEMENTS:
    if only and api not in only:
        continue
    build, (yaw, pitch) = A.BUILDERS[api]
    try:
        ops.newScene()
        A.S.meshes = []
        build()
        mats = A.materials()
        mask = ops.createMaterial("Icon_mask", baseColor=[0, 0, 0], metallic=0.0, roughness=1.0, specular=0.0)
        meshes = [m for m, _ in A.S.meshes]
        for m, look in A.S.meshes:
            ops.assignMaterial(mats[look], m)
        ops.select(meshes)
        view = dict(yaw=yaw, pitch=pitch, width=768, height=768, display="shaded", grid=False, showSelection=False,
                    frameSelection=True)
        ops.captureView(path=os.path.join(out, f"{api}_beauty.png"), **view)
        for m in meshes:
            ops.assignMaterial(mask, m)
        ops.captureView(path=os.path.join(out, f"{api}_matte.png"), **view)
        print("rendered", api)
    except Exception:
        failed.append(api)
        print("FAILED", api)
        traceback.print_exc()

print("failed:", ",".join(failed) if failed else "none")
