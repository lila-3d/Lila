"""Builds the Curves example: one labelled setup per curve tool group in a 4 by 3 grid, each with a camera framing it,
and Cam_All framing the whole grid at 16:9.

Run with `lilabatch --batch build_curves.py [out_dir]`, or from the script editor. Writes curves.lila next to this file,
or into out_dir. Units are cm, Y up.
"""

import os
import sys

from lila import ops

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
out_dir = sys.argv[1] if len(sys.argv) > 1 else here

COLUMNS = 4
COLUMN_SPACING = 850.0
ROW_SPACING = 900.0
OPEN = None


def first(r):
    return r[0] if isinstance(r, list) else r


def cable(name, points, **flags):
    """The cable's curve and its pipe mesh."""
    made = first(ops.cable(points=points, pipe=True, name=name, **flags))
    return made[0], made[-1]


def at(i, x=0.0, y=0.0, z=0.0):
    """A point relative to setup i's cell, read left to right, back to front."""
    col, row = i % COLUMNS, i // COLUMNS
    return ((col - (COLUMNS - 1) / 2) * COLUMN_SPACING + x, y, (row - 1) * ROW_SPACING + z)


def label(i, text):
    """The setup's name in bevelled text, standing at the front of its cell."""
    curve = first(ops.textCurves(text=text, size=45, alignment="center", name=f"Label{i}"))
    ops.move(curve, by=at(i, 0, 0, 330))
    solid = first(ops.bevelPlus(curve, extrudeDepth=4, frontProfile="convex", frontSegments=2, frontWidth=1, frontDepth=1,
                                name=f"Label{i}Solid"))
    ops.assignMaterial("M_Label", solid)
    return [curve, solid]


def camera(name, eye, look):
    cam = ops.createNode("camera", name=name, skipSelect=True)
    ops.viewPlace(cam, eye=eye, lookAt=look, up=(0, 1, 0))
    return cam


def material(name, color, roughness=0.5, metallic=0.0):
    ops.createMaterial(name, baseColor=color, roughness=roughness, metallic=metallic)
    return name


def border_edges(mesh, x):
    return ops.findComponents(mesh, kind="edge", box=[[x - 0.5, OPEN, OPEN], [x + 0.5, OPEN, OPEN]])


ops.newScene()
metal = material("M_Metal", (0.62, 0.64, 0.68), roughness=0.3, metallic=1.0)
paint = material("M_Paint", (0.85, 0.32, 0.18), roughness=0.4)
wood = material("M_Wood", (0.55, 0.38, 0.22), roughness=0.7)
dark = material("M_Dark", (0.08, 0.08, 0.09), roughness=0.6)
clay = material("M_Clay", (0.78, 0.76, 0.72), roughness=0.8)
hair = material("M_Hair", (0.32, 0.2, 0.12), roughness=0.5)
material("M_Label", (0.95, 0.95, 0.95), roughness=0.6)
groups = {}

# 0. Overview: a desk lamp made only from curves (revolved base and shade, piped arm, cable), three times life size.
i = 0


def lamp(x, y, z):
    return at(i, 3 * x, 3 * y, 3 * z)


base_profile = ops.curve(type="poly", points=[lamp(0, 0, 0), lamp(22, 0, 0), lamp(22, 2, 0), lamp(18, 4, 0), lamp(4, 5, 0), lamp(3, 8, 0),
                                              lamp(0, 8, 0)], name="LampBaseProfile")
lamp_base = first(ops.revolve(base_profile, axisOrigin=lamp(0, 0, 0), axis=(0, 1, 0), sections=48, name="LampBase"))
arm_path = ops.curveEP(points=[lamp(0, 7, 0), lamp(4, 50, 0), lamp(10, 70, 0), lamp(40, 85, 0), lamp(60, 72, 0)], name="LampArmPath")
lamp_arm = first(ops.pipe(arm_path, radius=4.8, sides=16, name="LampArm"))
shade_profile = ops.curveEP(points=[lamp(62, 72, 0), lamp(68, 70, 0), lamp(76, 62, 0), lamp(80, 54, 0)], name="LampShadeProfile")
lamp_shade = first(ops.revolve(shade_profile, axisOrigin=lamp(60, 72, 0), axis=(0, 1, 0), sections=48, name="LampShade"))
lamp_cable_curve, lamp_cable = cable("LampCable", [lamp(-4, 3, 0), lamp(-60, 0, 30)], slack=0.15, pipeRadius=1.5)
ops.assignMaterial(metal, lamp_base, lamp_arm)
ops.assignMaterial(paint, lamp_shade)
ops.assignMaterial(dark, lamp_cable)
ops.hide(base_profile, arm_path, shade_profile, lamp_cable_curve)
groups["Overview"] = [base_profile, lamp_base, arm_path, lamp_arm, shade_profile, lamp_shade, lamp_cable_curve, lamp_cable]
camera("Cam_Overview", lamp(110, 110, 190), lamp(10, 40, 0))

# 1. Surfaces: sweep, revolve and loft side by side.
i = 1
sweep_path = ops.curveEP(points=[at(i, -260, 0, 0), at(i, -230, 60, -30), at(i, -180, 80, 20), at(i, -140, 40, 0)], name="SweepPath")
sweep = first(ops.extrude(sweep_path, profileShape="circle", radius=8, sides=16, name="Sweep"))
vase_profile = ops.curveEP(points=[at(i, 15, 0, 0), at(i, 40, 30, 0), at(i, 25, 80, 0), at(i, 18, 100, 0), at(i, 24, 120, 0)],
                           name="VaseProfile")
vase = first(ops.revolve(vase_profile, axisOrigin=at(i), axis=(0, 1, 0), sections=40, caps=True, name="Revolve"))
loft_curves = [ops.curveEP(closed=True, name=f"LoftSection{n}",
                           points=[at(i, 180 + rx, h, 0), at(i, 180, h, rz), at(i, 180 - rx, h, 0), at(i, 180, h, -rz)])
               for n, (h, rx, rz) in enumerate([(0, 50, 30), (40, 25, 40), (80, 45, 20), (120, 20, 20)])]
loft = first(ops.loft(*loft_curves, name="Loft"))
ops.assignMaterial(clay, sweep, vase, loft)
groups["Surfaces"] = [sweep_path, sweep, vase_profile, vase, *loft_curves, loft]
camera("Cam_Surfaces", at(i, 0, 220, 520), at(i, 0, 50, 0))

# 2. Curve network and border blend.
i = 2
net_curves = []
for n, z in enumerate((-80, 0, 80)):
    net_curves.append(ops.curveEP(points=[at(i, -280, 20, z), at(i, -200, 60 + abs(z) * 0.2, z), at(i, -120, 20, z)], name=f"NetU{n}"))
for n, x in enumerate((-270, -200, -130)):
    sag = 60 if x == -200 else 25
    net_curves.append(ops.curveEP(points=[at(i, x, 20, -90), at(i, x, sag + 20, 0), at(i, x, 20, 90)], name=f"NetV{n}"))
network = first(ops.curveNetwork(*net_curves, spansU=6, spansV=6, tolerance=15, name="Network"))
blend_a = first(ops.polyPlane(name="BlendA", width=100, depth=100, subdivisionsX=4, subdivisionsZ=4, position=at(i, 60, 0, 0)))
blend_b = first(ops.polyPlane(name="BlendB", width=100, depth=100, subdivisionsX=4, subdivisionsZ=4, position=at(i, 250, 60, 0)))
ops.rotate(blend_b, by=(0, 0, 30))
border = first(ops.borderBlend(meshA=blend_a, edgesA=border_edges(blend_a, at(i, 110)[0]), meshB=blend_b,
                               edgesB=ops.findComponents(blend_b, kind="edge", border=True, extreme="-x"), segments=8, name="BorderBlend"))
ops.assignMaterial(clay, network, blend_a, blend_b)
ops.assignMaterial(paint, border)
groups["NetworkAndBlend"] = [*net_curves, network, blend_a, blend_b, border]
camera("Cam_NetworkAndBlend", at(i, 0, 300, 450), at(i, 0, 30, 0))

# 3. Along a curve: a rail bent along a path, posts arrayed along the same path.
i = 3
fence_path = ops.curveEP(points=[at(i, -300, 0, 50), at(i, -150, 0, -80), at(i, 0, 0, 60), at(i, 150, 0, -60), at(i, 300, 0, 40)],
                         name="FencePath")
rails, rail_paths = [], []
for n, h in enumerate((40, 80)):
    rail_path = ops.curveEP(points=[(x, y + h, z) for x, y, z in ops.curveData(fence_path)[0]["points"]], name=f"RailPath{n}")
    rail = first(ops.polyCube(name=f"Rail{n}", width=100, height=8, depth=6, subdivisionsX=20, position=at(i)))
    ops.deformAlongCurve(rail, curve=rail_path, forwardAxis="+x", stretch=True)
    rails.append(rail)
    rail_paths.append(rail_path)
post = first(ops.polyCube(name="Post", width=10, height=90, depth=10, baseAtOrigin=True, position=at(i)))
ops.arrayAlongCurve(post, curve=fence_path, fill="spacing", spacing=60)
ops.assignMaterial(wood, *rails, post)
groups["AlongCurve"] = [fence_path, *rail_paths, *rails, post]
camera("Cam_AlongCurve", at(i, 0, 350, 520), at(i, 0, 40, 0))

# 4. Planar: text curves, a curve boolean, and Bevel Plus solids of both (front view, XY plane).
i = 4
text = ops.textCurves(text="Lila", size=80, name="TextLila")
text_curve = first(text)
ops.move(text_curve, by=at(i, -300, 120, 0))
text_bevel = first(ops.bevelPlus(text_curve, extrudeDepth=10, frontProfile="convex", frontSegments=3, frontWidth=2, frontDepth=2,
                                 name="TextBevel"))
ops.move(text_bevel, by=(0, -130, 0))
gear = ops.circle(center=at(i, 140, 140, 0), normal=(0, 0, 1), radius=50, name="BoolTarget")
cutter = ops.curve(type="poly", closed=True, points=[at(i, 120, 120, 0), at(i, 220, 120, 0), at(i, 220, 220, 0), at(i, 120, 220, 0)],
                   name="BoolTool")
boolean = first(ops.curveBoolean(gear, cutter, operation="subtract", name="BoolResult"))
ops.move(boolean, by=(160, 0, 0))
bool_bevel = first(ops.bevelPlus(boolean, extrudeDepth=12, frontProfile="straight", frontWidth=3, frontDepth=3, name="BoolBevel"))
ops.move(bool_bevel, by=(0, -130, 0))
ops.assignMaterial(paint, text_bevel, bool_bevel)
groups["Planar"] = [text_curve, text_bevel, gear, cutter, boolean, bool_bevel]
camera("Cam_Planar", at(i, 0, 80, 650), at(i, 0, 80, 0))

# 5. Roof: an L-shaped footprint with a courtyard hole.
i = 5
footprint = ops.curve(type="poly", closed=True, name="RoofFootprint",
                      points=[at(i, -200, 0, -150), at(i, 200, 0, -150), at(i, 200, 0, 0), at(i, 0, 0, 0), at(i, 0, 0, 150), at(i, -200, 0, 150)])
courtyard = ops.curve(type="poly", closed=True, name="RoofCourtyard",
                      points=[at(i, -140, 0, -90), at(i, -60, 0, -90), at(i, -60, 0, -10), at(i, -140, 0, -10)])
roof = first(ops.roof(footprint, courtyard, pitch=35, overhang=15, thickness=4, name="Roof"))
ops.assignMaterial(paint, roof)
groups["Roof"] = [footprint, courtyard, roof]
camera("Cam_Roof", at(i, 250, 380, 420), at(i, 0, 0, 0))

# 6. Blend: two curves joined by a G2 blend, curvature combs on.
i = 6
blend_left = ops.curveEP(points=[at(i, -250, 0, 0), at(i, -180, 40, 0), at(i, -100, 20, 0)], name="BlendLeft")
blend_right = ops.curveEP(points=[at(i, 100, 80, 0), at(i, 180, 120, 0), at(i, 250, 90, 0)], name="BlendRight")
blend = first(ops.blendCurve(blend_left, blend_right, continuityA="g2", continuityB="g2", name="BlendCurve"))
for c in (blend_left, blend_right, blend):
    ops.setAttr(f"{c}.displayComb", True)
groups["Blend"] = [blend_left, blend_right, blend]
camera("Cam_Blend", at(i, 0, 60, 550), at(i, 0, 60, 0))

# 7. Cables: a bundle sagging between three poles, draped over a crate.
i = 7
poles = []
for n, x in enumerate((-250, 0, 250)):
    pole = first(ops.polyCylinder(name=f"CablePole{n}", radius=6, height=220, baseAtOrigin=True, position=at(i, x, 0, 0)))
    poles.append(pole)
crate = first(ops.polyCube(name="CableCrate", width=80, height=200, depth=80, baseAtOrigin=True, position=at(i, 125, 0, 0)))
cable_curve, cables = cable("PowerCables", [at(i, -250, 210, 0), at(i, 0, 210, 0), at(i, 250, 210, 0)], slack=0.08, count=4, spread=6,
                            drape=True, colliders=[crate], pipeRadius=1.2)
ops.assignMaterial(wood, *poles)
ops.assignMaterial(clay, crate)
ops.assignMaterial(dark, cables)
groups["Cables"] = [*poles, crate, cable_curve, cables]
camera("Cam_Cables", at(i, 0, 220, 520), at(i, 0, 140, 0))

# 8. Sketch: a constrained, dimensioned bracket on the ground plane.
i = 8
sketch = ops.curve(type="poly", closed=True, name="SketchBracket",
                   points=[at(i, -100, 0, -60), at(i, 100, 0, -58), at(i, 102, 0, -10), at(i, -40, 0, -12), at(i, -42, 0, 60), at(i, -100, 0, 62)])
sketch_steps = [("constrain", "fixed", ["p0.0"]), ("constrain", "horizontal", ["l0.0"]), ("constrain", "vertical", ["l0.1"]),
                ("constrain", "horizontal", ["l0.2"]), ("constrain", "vertical", ["l0.3"]), ("constrain", "horizontal", ["l0.4"]),
                ("constrain", "vertical", ["l0.5"]), ("dimension", "length", ["l0.0"], 200.0), ("dimension", "length", ["l0.5"], 120.0),
                ("dimension", "length", ["l0.1"], 50.0)]
for step in sketch_steps:
    try:
        if step[0] == "constrain":
            ops.sketchConstrain(sketch, type=step[1], refs=step[2])
        else:
            ops.sketchDimension(sketch, type=step[1], refs=step[2], value=step[3])
    except Exception as e:
        print("sketch:", step, e)
groups["Sketch"] = [sketch]
camera("Cam_Sketch", at(i, 0, 420, 1), at(i, 0, 0, 0))

# 9. Grooming: guides rooted on a head, and hair cards built from them beside it.
i = 9
head = first(ops.polySphere(name="GroomHead", radius=55, subdivisionsAxis=32, subdivisionsHeight=24, position=at(i, -110, 55, 0)))
scalp = first(ops.polySphere(name="GroomScalp", radius=55.5, subdivisionsAxis=32, subdivisionsHeight=24, position=at(i, -110, 55, 0)))
ops.selectComponentsWhere(scalp, kind="face", box=[[OPEN, OPEN, OPEN], [OPEN, 80, OPEN]])
ops.deleteComponents()
ops.selectMode("object")
guides = first(ops.groomGuides(scalp, count=500, length=40, lengthJitter=0.3, points=8, seed=3, name="HairGuides"))
# Comb the guides down and back from the crown so they read as hair rather than spikes.
ops.curveSculptStroke(guides, brush="comb", strength=0.6, dabs=[{"center": at(i, -110, 120, 0), "normal": (0, 1, 0), "radius": 60,
                                                                "strength": 0.6, "delta": (0, -6, -10)}])
cards = first(ops.hairCards(guides, width=3, segments=4, name="HairCards"))
ops.move(cards, by=(220, 0, 0))
cards_head = first(ops.polySphere(name="CardsHead", radius=55, subdivisionsAxis=32, subdivisionsHeight=24, position=at(i, 110, 55, 0)))
ops.assignMaterial(clay, head, cards_head)
ops.assignMaterial(hair, scalp, cards)
groups["Groom"] = [head, scalp, guides, cards, cards_head]
camera("Cam_Groom", at(i, 0, 160, 520), at(i, 0, 70, 0))

# 10. Unreal spline: a road curve with a road segment mesh to run along it in Unreal.
i = 10
road = ops.curveEP(points=[at(i, -300, 0, 0), at(i, -150, 10, -120), at(i, 50, 20, 80), at(i, 300, 0, -40)], name="RoadSpline")
road_piece = first(ops.polyCube(name="RoadSegment", width=200, height=5, depth=80, subdivisionsX=4, position=at(i, 0, 0, -220)))
ops.assignMaterial(dark, road_piece)
try:
    ops.unrealMap(road, kind="spline", splineMesh=road_piece, splineMeshAxis="x")
except Exception as e:
    print("unrealMap:", e)
groups["UnrealSpline"] = [road, road_piece]
camera("Cam_UnrealSpline", at(i, 0, 300, 500), at(i, 0, 0, 0))

titles = {"Overview": "Built from curves", "Surfaces": "Sweep, revolve, loft", "NetworkAndBlend": "Network, border blend",
          "AlongCurve": "Along a curve", "Planar": "Text, boolean, Bevel Plus", "Roof": "Roof", "Blend": "Blend curve",
          "Cables": "Cables", "Sketch": "Sketch constraints", "Groom": "Groom, hair cards", "UnrealSpline": "Unreal spline"}
for n, (name, members) in enumerate(groups.items()):
    ops.group(*members, *label(n, titles[name]), name=name + "Example")
camera("Cam_All", (0, 2490, 3735), (0, 50, 250))
ops.select(clear=True)
ops.saveScene(os.path.join(out_dir, "curves.lila"))
