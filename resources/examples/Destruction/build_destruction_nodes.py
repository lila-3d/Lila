"""Builds the geometry-nodes destruction demo: the same shot as Destruction_Tools.lila, written as one node group.

The slab is the mesh the group runs on; the four walls and the pit's rim are made inside the group from Cube nodes, the rim
as a fixed collider. Each is cut by its own
Fracture node, the sets are put together with Join Pieces, seven Explosion nodes feed the Rigid Body Solver's blasts,
and a Collapse node driven by Scene Time makes the slab give way from one side to the other.

Run with `lilabatch --batch build_destruction_nodes.py`, or from the script editor. Writes Destruction_Nodes.lila and
CourtyardCollapse.lilanodes (the group on its own, to apply to any slab) next to this file, or into the folder in the
LILA_EXAMPLE_OUT environment variable.
"""

import os

from lila import ops

FPS = 30.0
FIRST, LAST = 1, 240
GROUND_TOP, GROUND_THICKNESS, PIT_DEPTH = 0.0, 60.0, 2500.0

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
out_dir = os.environ.get("LILA_EXAMPLE_OUT") or here


def seconds(frame):
    return frame / FPS


# name, centre of its base, size (x, y, z): the same walls as the tools demo.
WALLS = [
    ("WallA", [-1300, 0, -700], [700, 300, 30]),
    ("WallB", [-250, 0, 500], [600, 300, 30]),
    ("WallC", [650, 0, -450], [30, 300, 700]),
    ("WallD", [1350, 0, 600], [800, 300, 30]),
]
# position, frame, speed (cm/s), radius (cm), and the wall it hits first (its pieces are finer there).
EXPLOSIONS = [
    ([-1500, 110, -640], 20, 1500, 300, "WallA"),
    ([-1080, 190, -760], 34, 1400, 280, None),
    ([-300, 120, 560], 48, 1600, 320, "WallB"),
    ([710, 150, -300], 62, 1500, 300, "WallC"),
    ([600, 90, -650], 70, 1300, 260, None),
    ([1150, 130, 660], 84, 1600, 320, "WallD"),
    ([1580, 200, 540], 96, 1500, 280, None),
]

inputs = [
    {"name": "groundPieces", "type": "int", "default": 220, "min": 1, "softMax": 600, "label": "Ground Pieces",
     "description": "Roughly how many pieces the slab breaks into.", "panel": "Pieces"},
    {"name": "wallPieces", "type": "int", "default": 70, "min": 1, "softMax": 300, "label": "Wall Pieces",
     "description": "Roughly how many pieces each wall breaks into.", "panel": "Pieces"},
    {"name": "seed", "type": "int", "default": 1, "label": "Seed", "description": "Another seed gives another crack pattern.",
     "panel": "Pieces"},
    {"name": "strength", "type": "float", "default": 20.0, "min": 0, "softMax": 200, "label": "Strength",
     "description": "How hard the concrete is to break, N per cm2 two pieces share.", "panel": "Pieces"},
    {"name": "blast", "type": "float", "default": 1.0, "min": 0, "softMax": 3, "label": "Blast",
     "description": "Scales every explosion's speed.", "panel": "Explosions"},
    {"name": "collapseStart", "type": "float", "default": 125.0, "label": "Collapse Start",
     "description": "Frame the first ground pieces fall.", "panel": "Collapse"},
    {"name": "collapseDuration", "type": "float", "default": 75.0, "min": 1, "label": "Collapse Duration",
     "description": "Frames for the failure to cross the slab.", "panel": "Collapse"},
    {"name": "rumble", "type": "float", "default": 4.0, "min": 0, "softMax": 20, "label": "Rumble",
     "description": "How far the ground shakes before it falls, cm.", "panel": "Collapse"},
]

nodes = [
    {"name": "GroundFracture", "type": "Fracture", "position": [250, -400], "props": {"material": "concrete"}},
    {"name": "GroundAnchor", "type": "AnchorPieces", "position": [500, -320],
     "values": {"height": GROUND_TOP - GROUND_THICKNESS, "tolerance": 1.0}},
    {"name": "Time", "type": "SceneTime", "position": [500, -560], "values": {"fps": FPS}},
    {"name": "GroundCollapse", "type": "CollapsePieces", "position": [750, -400],
     "values": {"direction": [1, 0, 0.25], "lead": 25.0, "raggedness": 0.3}},
    {"name": "Solver", "type": "RigidBodySolver", "position": [1600, 0],
     "values": {"startFrame": FIRST, "groundHeight": GROUND_TOP - GROUND_THICKNESS - PIT_DEPTH, "density": 2400.0,
                "friction": 0.7, "bounce": 0.05}},
    {"name": "Blasts", "type": "JoinGeometry", "position": [1350, 500]},
    {"name": "Realize", "type": "RealizeInstances", "position": [1850, 0]},
]
links = [
    "GroupInput.geometry -> GroundFracture.mesh", "GroupInput.groundPieces -> GroundFracture.pieces",
    "GroupInput.seed -> GroundFracture.seed", "GroupInput.strength -> GroundFracture.linkStrength",
    "GroundFracture.pieces -> GroundAnchor.pieces", "GroundFracture.links -> GroundAnchor.links",
    "GroupInput.strength -> GroundAnchor.strength",
    "GroundFracture.pieces -> GroundCollapse.pieces", "GroundAnchor.links -> GroundCollapse.links",
    "Time.frame -> GroundCollapse.frame", "GroupInput.collapseStart -> GroundCollapse.start",
    "GroupInput.collapseDuration -> GroundCollapse.duration", "GroupInput.rumble -> GroundCollapse.rumble",
    "Blasts.geometry -> Solver.blasts", "Solver.pieces -> Realize.geometry", "Realize.geometry -> Shot.geometry",
    "Rim.geometry -> Solver.colliders", "Rim.geometry -> Shot.geometry", "Shot.geometry -> GroupOutput.geometry",
]
nodes += [
    {"name": "Rim", "type": "JoinGeometry", "position": [1350, -700]},
    {"name": "Shot", "type": "JoinGeometry", "position": [2050, 0]},
]

# The ground around the slab, down to the pit floor, which stays: the pit's rim. Pieces hit it as a fixed collider.
for name, centre, size in [("RimNorth", [0, -1280, -2250], [6000, 2560, 1500]), ("RimSouth", [0, -1280, 2250], [6000, 2560, 1500]),
                           ("RimWest", [-2500, -1280, 0], [1000, 2560, 3000]), ("RimEast", [2500, -1280, 0], [1000, 2560, 3000])]:
    nodes += [
        {"name": f"{name}Box", "type": "MeshCube", "position": [900, -900], "values": {"size": size}},
        {"name": f"{name}Place", "type": "TransformGeometry", "position": [1100, -900], "values": {"translation": centre}},
    ]
    links += [f"{name}Box.mesh -> {name}Place.geometry", f"{name}Place.geometry -> Rim.geometry"]

# Each wall: a Cube moved into place, fractured with its pieces finer around the first explosion to hit it.
first_hit = {e[4]: e[0] for e in EXPLOSIONS if e[4]}
joined = ("GroundCollapse.pieces", "GroundCollapse.links")
for k, (name, base, size) in enumerate(WALLS):
    y = 200 + k * 260
    centre = [base[0], base[1] + size[1] / 2, base[2]]
    nodes += [
        {"name": f"{name}Box", "type": "MeshCube", "position": [0, y], "values": {"size": size}},
        {"name": f"{name}Place", "type": "TransformGeometry", "position": [200, y], "values": {"translation": centre}},
        {"name": f"{name}Fracture", "type": "Fracture", "position": [450, y], "props": {"material": "concrete"},
         "values": {"impact": first_hit[name], "impactFocus": 0.5, "impactRadius": 200.0}},
        {"name": f"Join{name}", "type": "JoinPieces", "position": [1000 + k * 120, y / 2]},
    ]
    links += [
        f"{name}Box.mesh -> {name}Place.geometry", f"{name}Place.geometry -> {name}Fracture.mesh",
        f"GroupInput.wallPieces -> {name}Fracture.pieces", f"GroupInput.seed -> {name}Fracture.seed",
        f"GroupInput.strength -> {name}Fracture.linkStrength",
        f"{joined[0]} -> Join{name}.pieces", f"{joined[1]} -> Join{name}.links",
        f"{name}Fracture.pieces -> Join{name}.pieces2", f"{name}Fracture.links -> Join{name}.links2",
    ]
    joined = (f"Join{name}.pieces", f"Join{name}.links")
links += [f"{joined[0]} -> Solver.pieces", f"{joined[1]} -> Solver.links"]

# The explosions, each speed scaled by Blast.
for k, (position, frame, speed, radius, _) in enumerate(EXPLOSIONS, 1):
    y = 300 + k * 150
    nodes += [
        {"name": f"Speed{k}", "type": "Math", "position": [900, y], "props": {"operation": "multiply"}, "values": {"a": speed}},
        {"name": f"Explosion{k}", "type": "Explosion", "position": [1100, y],
         "values": {"position": position, "frame": frame, "radius": radius}},
    ]
    links += [f"GroupInput.blast -> Speed{k}.b", f"Speed{k}.value -> Explosion{k}.speed", f"Explosion{k}.blast -> Blasts.geometry"]

spec = {
    "name": "CourtyardCollapse",
    "description": "Four concrete walls blown apart by seven explosions, then the slab they stand on rumbles and gives way from "
                   "one side to the other.",
    "inputs": inputs,
    "nodes": nodes,
    "links": links,
}

ops.newScene()
ops.playbackOptions(fps=FPS, minTime=seconds(FIRST), maxTime=seconds(LAST), animationStartTime=seconds(FIRST),
                    animationEndTime=seconds(LAST))

ground = ops.polyCube(name="Ground", position=[0, GROUND_TOP - GROUND_THICKNESS, 0], width=4000, height=GROUND_THICKNESS,
                      depth=3000, baseAtOrigin=True, generator=False)[0]
ops.freezeTransforms(ground)
ops.polyPlane(name="PitFloor", position=[0, GROUND_TOP - GROUND_THICKNESS - PIT_DEPTH, 0], width=4000, depth=3000, generator=False)
ops.gnBuildGroup(spec)
mod = ops.gnApply(ground, "CourtyardCollapse")

cam = ops.createNode("camera", name="ShotCam")
ops.viewPlace(cam, eye=[-4300, 2700, 4300], lookAt=[0, -500, 0])

ops.select(ground)
ops.currentTime(seconds(FIRST))
ops.gnExportGroup("CourtyardCollapse", os.path.join(out_dir, "CourtyardCollapse.lilanodes"))
ops.saveScene(os.path.join(out_dir, "Destruction_Nodes.lila"))
print("DESTRUCTION", mod)
