"""Very low-poly cute dog: ONE connected, symmetric, closed mesh grown out of one cube by extrusion, about 230 quads.

Runs through the MCP execute_python tool or headless: lilabatch --batch low_poly_dog.py
Y is up, the dog faces +Z (Lila's front view), units are cm. Every step selects faces by where they are (normal, side,
box, extreme), never by remembered ids, so it survives the edits before it.
"""

import os
import tempfile

from lila import cmds

OUT = os.environ.get("LILA_CAPTURE_DIR", tempfile.gettempdir())
OPEN = None  # an open side of a box= query


def faces(**query):
    return cmds.selectComponentsWhere(dog, kind="face", **query)


def look(name):
    """Front, side, top and perspective in one small image: judge the silhouette after each stage."""
    path = os.path.join(OUT, f"dog_{name}.png")
    cmds.captureTurntable(path=path, views=["front", "right", "top", "persp"], display="shaded")
    return path


cmds.newScene()

# Stage 1, blockout. The body is split on x=0 (symmetry plane) and in three rows: front legs, middle, back legs.
dog, _ = cmds.polyCube(name="Dog", width=14, height=11, depth=20, subdivisionsX=2, subdivisionsZ=3, position=(0, 15, 0))
cmds.setSymmetry(dog, axis="x")
cmds.createMaterial("DogFur", baseColor=(0.78, 0.55, 0.32), roughness=0.85)
cmds.assignMaterial("DogFur", dog)

# Short legs out of the bottom corner faces; symmetry makes the -x pair, and each leg scales about its own centre.
for end in ("+z", "-z"):
    faces(normal=(0, -1, 0), side="+x", extreme=end)
    cmds.polyInset(thickness=0.8)
    cmds.polyExtrude(thickness=4)
    cmds.scaleComponents(by=(0.85, 1, 0.85))
    cmds.polyExtrude(thickness=5.5)

# Neck and a big head out of the body's front faces (the box leaves out the legs' front faces, which face +z too).
faces(normal=(0, 0, 1), box=[[OPEN, 10, OPEN], [OPEN, OPEN, OPEN]])
cmds.polyExtrude(thickness=2)
cmds.scaleComponents(by=(0.8, 0.8, 1))
cmds.moveComponents(by=(0, 5, 0))
cmds.polyExtrude(thickness=2)
cmds.scaleComponents(by=(1.6, 1.5, 1))
cmds.moveComponents(by=(0, 3, 0))
cmds.polyExtrude(thickness=13)
cmds.scaleComponents(by=(0.92, 0.92, 1))
print("blockout", cmds.meshCheck(dog)["issues"])
look("blockout")

# Stage 2, features. A horizontal loop around the head gives it upper faces (eyes, ears) and lower ones (snout).
head_z = cmds.meshCheck(dog)["box"][1][2]
front = [[OPEN, OPEN, head_z - 0.5], [OPEN, OPEN, OPEN]]
vertical = cmds.findComponents(dog, kind="edge", direction=(0, 1, 0), side="+x", box=front)
cmds.selectComponents(dog, edges=vertical[:1])
cmds.polyInsertLoop(fraction=0.5)

# Snout: both lower front faces as one region across the seam, so it stays centred.
faces(normal=(0, 0, 1), box=front, extreme="-y", count=2)
cmds.polyInset(thickness=1.0)
cmds.polyExtrude(thickness=4)
cmds.scaleComponents(by=(0.75, 0.8, 1))
cmds.moveComponents(by=(0, -0.5, 0))

# Nose: the snout tip inset, narrowed and bumped up.
tip = [[OPEN, OPEN, cmds.meshCheck(dog)["box"][1][2] - 0.5], [OPEN, OPEN, OPEN]]
faces(normal=(0, 0, 1), box=tip)
cmds.polyInset(thickness=0.6)
cmds.scaleComponents(by=(0.45, 1.3, 1))
cmds.polyExtrude(thickness=1.5)
cmds.moveComponents(by=(0, 0.8, 0))
cmds.createMaterial("DogNose", baseColor=(0.03, 0.02, 0.02), roughness=0.35)
cmds.assignMaterial("DogNose")  # no objects: the selected faces

# Big round eyes: the upper front face per side. They touch the seam, so individual=True keeps the two apart.
faces(normal=(0, 0, 1), side="+x", box=front, extreme="+y")
cmds.polyInset(thickness=0.6, individual=True)
cmds.scaleComponents(by=(0.85, 1.1, 1))
cmds.polyExtrude(thickness=0.8, individual=True)
cmds.scaleComponents(by=(1.05, 1.05, 1))
cmds.polyExtrude(thickness=0.6, individual=True)
cmds.scaleComponents(by=(0.75, 0.75, 1))
cmds.createMaterial("DogEye", baseColor=(0.02, 0.02, 0.03), roughness=0.1)
cmds.assignMaterial("DogEye")

# Large floppy ears: the head's upper side face, a narrow root, then two segments hanging down and out.
faces(normal=(1, 0, 0), side="+x", box=[[OPEN, 22, head_z - 13.5], [OPEN, OPEN, OPEN]], extreme="+y")
cmds.polyInset(thickness=1.0)
cmds.scaleComponents(by=(1, 0.7, 0.6))
cmds.polyExtrude(thickness=1.5)
cmds.polyExtrude(thickness=1.0)
cmds.moveComponents(by=(1, -6, 0))
cmds.scaleComponents(by=(0.7, 1, 1.5))
cmds.polyExtrude(thickness=1.0)
cmds.moveComponents(by=(0.5, -6, 0))
cmds.scaleComponents(by=(1, 1, 0.7))

# Tail: the upper back faces inset as one region, then two extrusions curling up.
faces(normal=(0, 0, -1), box=[[OPEN, 13, OPEN], [OPEN, OPEN, -9.5]])
cmds.polyInset(thickness=2.5)
cmds.polyExtrude(thickness=3)
cmds.moveComponents(by=(0, 3, 0))
cmds.scaleComponents(by=(0.7, 0.7, 1))
cmds.polyExtrude(thickness=3)
cmds.moveComponents(by=(0, 4, 0))
cmds.scaleComponents(by=(0.6, 0.6, 1))

# Finish: one shell, closed, manifold, symmetric. A softer look would be one subdivision level:
# cmds.addModifier(dog, kind="subdivision", params={"levels": 1})
cmds.selectMode("object")
cmds.select(clear=True)
report = cmds.meshCheck(dog)
assert report["ok"] and report["shells"] == 1, report["issues"]
print(f"{dog}: {report['faces']} faces, {report['triangles']} triangles, {report['shells']} shell", look("final"))
