"""Builds the tools-only destruction demo: four concrete walls on a slab, blown apart by seven explosions, then the slab
rumbles and gives way from one side to the other, dropping everything into the pit below.

Only commands are used (the same ones the Effects > Destruction menu runs); no node editing. Run with
`lilabatch --batch build_destruction_tools.py`, or from the script editor. Writes Destruction_Tools.lila next to this
file, or into the folder in the LILA_EXAMPLE_OUT environment variable. LILA_EXAMPLE_CHECK=1 also simulates every frame
and prints the bounds as it goes.
"""

import os
import time

from lila import ops

FPS = 30.0
FIRST, LAST = 1, 240

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
out_dir = os.environ.get("LILA_EXAMPLE_OUT") or here


def seconds(frame):
    return frame / FPS


ops.newScene()
ops.playbackOptions(fps=FPS, minTime=seconds(FIRST), maxTime=seconds(LAST), animationStartTime=seconds(FIRST),
                    animationEndTime=seconds(LAST))

# A 40 x 30 m slab, 60 cm thick, its top at Y = 0.
ops.polyCube(name="Ground", position=[0, -60, 0], width=4000, height=60, depth=3000, baseAtOrigin=True, generator=False)

# The ground around it, down to the pit floor, which stays: the pit's rim, a collider that pieces hit but never moves.
RIM = [
    ("RimNorth", [0, -2560, -2250], 6000, 1500),
    ("RimSouth", [0, -2560, 2250], 6000, 1500),
    ("RimWest", [-2500, -2560, 0], 1000, 3000),
    ("RimEast", [2500, -2560, 0], 1000, 3000),
]
for name, base, width, depth in RIM:
    ops.polyCube(name=name, position=base, width=width, height=2560, depth=depth, baseAtOrigin=True, generator=False)

# The pit floor, where the solver's floor ends up (collapse lowers it by depth below the slab).
ops.polyPlane(name="PitFloor", position=[0, -2560, 0], width=4000, depth=3000, generator=False)

# Four walls standing on it, 3 m tall and 30 cm thick.
walls = [
    ("WallA", [-1300, 0, -700], 700, 30),
    ("WallB", [-250, 0, 500], 600, 30),
    ("WallC", [650, 0, -450], 30, 700),
    ("WallD", [1350, 0, 600], 800, 30),
]
for name, base, width, depth in walls:
    ops.polyCube(name=name, position=base, width=width, height=300, depth=depth, baseAtOrigin=True, generator=False)

# One simulation for everything, so the walls' rubble rides the slab down when it goes.
mod = ops.breakApart("Ground", *[w[0] for w in walls], material="concrete", startFrame=FIRST, colliders=[r[0] for r in RIM])[0]
ops.setAttr(f"{mod}.Ground_pieces", 220)
for name, *_ in walls:
    ops.setAttr(f"{mod}.{name}_pieces", 70)

# Seven explosions, each just off a wall's face, staggered so the walls go one after another.
explosions = [
    ([-1500, 110, -640], 20, 1500, 300),
    ([-1080, 190, -760], 34, 1400, 280),
    ([-300, 120, 560], 48, 1600, 320),
    ([710, 150, -300], 62, 1500, 300),
    ([600, 90, -650], 70, 1300, 260),
    ([1150, 130, 660], 84, 1600, 320),
    ([1580, 200, 540], 96, 1500, 280),
]
for position, frame, blast, radius in explosions:
    ops.addExplosion("Destruction", position=position, frame=frame, blast=blast, radius=radius)

# Then the slab: it starts shaking at frame 100 on the -X side and fails across to +X over two and a half seconds.
ops.collapse("Destruction", part="Ground", start=125, duration=75, direction=[1, 0, 0.25], rumble=4, lead=25,
             raggedness=0.3, depth=2500)

cam = ops.createNode("camera", name="ShotCam")
ops.viewPlace(cam, eye=[-4300, 2700, 4300], lookAt=[0, -500, 0])

ops.select("Destruction")
if os.environ.get("LILA_EXAMPLE_CHECK"):
    t0 = time.perf_counter()
    for frame in range(FIRST, LAST + 1, 10):
        ops.currentTime(seconds(frame))
        info = ops.meshInfo("Destruction")
        b = info.get("worldBounds") or info.get("bounds")
        print(f"CHECK frame {frame:3d} bounds {b}")
    print(f"CHECK simulated {LAST - FIRST + 1} frames in {time.perf_counter() - t0:.1f} s")

ops.currentTime(seconds(FIRST))
ops.saveScene(os.path.join(out_dir, "Destruction_Tools.lila"))
print("DESTRUCTION", mod)
