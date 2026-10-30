"""Builds the Achievements example: every Steam achievement icon's model in a grid, labelled, as Achievements.lila.

Run with `lilabatch --batch build_achievements.py [out_dir]`, or from the script editor. Writes next to this file, or
into out_dir.
"""

import os
import sys

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
sys.path.insert(0, here)

import achievements as A
from lila import ops
from table import ACHIEVEMENTS

out_dir = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].endswith(".py") else here
COLUMNS = 9
SPACING = 40.0

ops.newScene()
mats = A.materials()
for i, (api, name, *_rest) in enumerate(ACHIEVEMENTS):
    build, _view = A.BUILDERS[api]
    A.S.meshes = []
    build()
    for mesh, look in A.S.meshes:
        ops.assignMaterial(mats[look], mesh)
    roots = []
    for mesh, _look in A.S.meshes:
        parent = ops.listRelatives(mesh, parent=True)
        root = parent[0] if parent else mesh
        if root not in roots:
            roots.append(root)
    label = A.text(name, size=3, depth=0.4, pos=(0, -15, 0))
    ops.assignMaterial(mats[A.FROST], label)
    group = ops.group(*roots, label, name="".join(w.capitalize() for w in api.split("_")) + "Achievement")
    ops.move(group, to=[(i % COLUMNS) * SPACING, -(i // COLUMNS) * SPACING, 0])

ops.select(ops.ls(dag=True))
ops.captureView(view="front", width=480, height=270, display="shaded", grid=False, showSelection=False,
                frameSelection=True, path=os.path.join(out_dir, "Achievements.png"))
ops.select([])
path = os.path.join(out_dir, "Achievements.lila")
ops.saveScene(path)
print("saved", path)
