"""Builds the Level example: an eroded valley with a walled hill town on the best site the settlement planner finds, a
ruined keep on the next best, rubble piled at its breaches and a market corner in the town square, all settled with
physics.

Run with `lilabatch --batch build_level.py [out_dir]`, or from the script editor. Writes level.lila next to this file,
or into out_dir. Units are cm, Y up.
"""

import math
import os
import sys
import time

from lila import ops

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
out_dir = sys.argv[1] if len(sys.argv) > 1 else here

TERRAIN_SIZE = 120000.0  # 1.2 km square
TERRAIN_SEED = 7
TOWN_RADIUS = 16000.0
KEEP_SIZE = 1400.0

started = time.time()


def log(message):
    print(f"[{time.time() - started:6.1f}s] {message}")


def first(result):
    return result[0] if isinstance(result, list) else result


def ground_at(x, z):
    return ops.terrainSample("Terrain", points=[[x, z]])[0]["height"]


def circle(cx, cz, radius, count):
    return [[cx + radius * math.cos(i / count * math.tau), cz + radius * math.sin(i / count * math.tau)] for i in range(count)]


ops.newScene()

terrain = ops.terrainCreate(size=[TERRAIN_SIZE, TERRAIN_SIZE], resolution=513, amplitude=7000, featureSize=45000, roughness=0.5,
                            ridged=0.3, seed=TERRAIN_SEED, name="Terrain")
ops.terrainErode("Terrain", iterations=90)
log("terrain")

# The planner scores flat ground away from the edge; the best site gets the town and the next the keep.
inner = TERRAIN_SIZE * 0.35
sites = ops.settlementSuggest(ground="Terrain", outline=[[-inner, -inner], [inner, -inner], [inner, inner], [-inner, inner]], count=2,
                              spacing=TOWN_RADIUS * 2.5)
town_at, keep_at = sites[0]["position"], sites[1]["position"]
log(f"sites: town {town_at}, keep {keep_at}")

town = ops.settlementCreate(ground="Terrain", outline=circle(town_at[0], town_at[2], TOWN_RADIUS, 24), attractors=[[town_at[0], town_at[2]]],
                            profile="medievalTown", seed=3, name="HillTown")
log(f"town: {town['lots']} lots, {town['buildings']} buildings")

# The keep: four walls broken to different heights, each standing on the lowest ground under it and sunk a little so
# no edge floats on the slope. Gaps in the east and west walls are the breaches the rubble spills from.
kx, kz = keep_at[0], keep_at[2]
half = KEEP_SIZE / 2
corners = {"sw": (kx - half, kz - half), "se": (kx + half, kz - half), "ne": (kx + half, kz + half), "nw": (kx - half, kz + half)}
KEEP_WALLS = [
    ("KeepNorth", [corners["nw"], corners["ne"]], 950),
    ("KeepEastA", [corners["ne"], (kx + half, kz + 150)], 620),
    ("KeepEastB", [(kx + half, kz - 250), corners["se"]], 380),
    ("KeepSouth", [corners["se"], corners["sw"]], 300),
    ("KeepWestA", [corners["sw"], (kx - half, kz - 100)], 720),
    ("KeepWestB", [(kx - half, kz + 300), corners["nw"]], 860),
]
walls = []
for name, ends, height in KEEP_WALLS:
    base = min(ground_at(x, z) for x, z in ends) - 60
    result = ops.masonryWall(points=[[x, base, z] for x, z in ends], height=height + 60, thickness=140, bond="english", stoneLength=70,
                             stoneHeight=32, stoneDepth=40, joint=1.5, wear=0.6, jitter=0.5, seed=len(walls) + 1, name=name)
    walls.append(result["mesh"])
log(f"keep: {len(walls)} walls")

# Masonry walls come out with these two slot names, so materials of the same names colour them.
stone = ops.createMaterial("MasonryStone", baseColor=[0.42, 0.4, 0.36], roughness=0.9)
ops.createMaterial("MasonryMortar", baseColor=[0.55, 0.53, 0.48], roughness=1.0)
timber = ops.createMaterial("Timber", baseColor=[0.36, 0.24, 0.14], roughness=0.8)

# Prop sources: hidden meshes the arrange commands pick from.
rubble = [first(ops.polyCube(name=f"Rubble{i}", width=w, height=h, depth=d, position=[0, -10000, 0]))
          for i, (w, h, d) in enumerate([(42, 24, 30), (30, 20, 26), (55, 28, 34), (24, 16, 20)])]
crate = first(ops.polyCube(name="Crate", width=80, height=70, depth=80, position=[0, -10000, 0]))
barrel = first(ops.polyCylinder(name="Barrel", radius=30, height=90, subdivisionsAxis=16, position=[0, -10000, 0]))
ops.assignMaterial(stone, *rubble)
ops.assignMaterial(timber, crate, barrel)

piles = []
for name, (x, z) in [("RubbleEast", (kx + half + 220, kz - 50)), ("RubbleWest", (kx - half - 220, kz + 100))]:
    result = ops.pileProps(items=rubble, at=[x, ground_at(x, z), z], count=60, radius=260, onto=["Terrain"], realize=True, seed=len(piles) + 1,
                           name=name)
    piles.append(result.get("group", name) if isinstance(result, dict) else name)
log("rubble")

# A market corner on the town's centre: crates piled, barrels stacked beside them.
cx, cz = town_at[0], town_at[2]
market = []
result = ops.pileProps(items=[crate], at=[cx, ground_at(cx, cz), cz], count=16, radius=120, onto=["Terrain"], realize=True, seed=2, name="Crates")
market.append(result.get("group", "Crates") if isinstance(result, dict) else "Crates")
for i, (dx, dz) in enumerate([(260, -60), (260, 30), (330, -15)]):
    bx, bz = cx + dx, cz + dz
    result = ops.stackProps(items=[barrel], at=[bx, ground_at(bx, bz), bz], count=2 - i // 2, onto=["Terrain"], realize=True, seed=i + 1,
                            name=f"Barrels{i}")
    market.append(result.get("group", f"Barrels{i}") if isinstance(result, dict) else f"Barrels{i}")
log("market")

# Maples round the keep, each a different seed, stood on the ground.
trees = []
leaf = first(ops.treeLeafPlane(preset="Kaede Mature"))
ops.move(leaf, to=[0, -10000, 0])
for i, angle in enumerate([20, 95, 160, 240, 310]):
    a = math.radians(angle)
    tx, tz = kx + (half + 1500) * math.cos(a), kz + (half + 1500) * math.sin(a)
    tree = ops.treeBuild(preset="Kaede Mature", settings={"trunk.seed": i + 1, "leaf.seed": i + 1}, leaf=leaf, name=f"Maple{i}")
    ops.move(tree, to=[tx, ground_at(tx, tz) - 20, tz])
    trees.append(tree)
log("trees")

ops.hide(*rubble, crate, barrel, leaf)
ops.group(*rubble, crate, barrel, leaf, name="PropSources")
ops.group(*walls, *piles, *trees, name="Keep")
ops.group(*market, name="Market")

ops.select(clear=True)
for name, eye, look in [
    ("Cam_Overview", [kx - 55000, keep_at[1] + 32000, kz + 45000], [(kx + town_at[0]) / 2, town_at[1], (kz + town_at[2]) / 2]),
    ("Cam_Town", [town_at[0] + 22000, town_at[1] + 9000, town_at[2] + 22000], town_at),
    ("Cam_Keep", [kx + 3200, keep_at[1] + 900, kz + 2600], [kx, keep_at[1] + 300, kz]),
    ("Cam_Market", [cx + 700, ground_at(cx, cz) + 280, cz + 650], [cx + 140, ground_at(cx, cz) + 50, cz]),
]:
    cam = ops.createNode("camera", name=name, skipSelect=True)
    ops.viewPlace(cam, eye=eye, lookAt=look, up=(0, 1, 0))
    # The default far plane is 100 m; the wide shots look across the whole valley.
    ops.setAttr(f"{cam}.farClipPlane", 1000000.0)
    ops.setAttr(f"{cam}.nearClipPlane", 5.0)

ops.saveScene(os.path.join(out_dir, "level.lila"))
log("saved")
