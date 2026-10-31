"""Builds the Grey Card lighting reference: an 18% gray ball, a chrome ball, a white ball and a ColorChecker chart.

Run with `lilabatch --batch build_grey_card.py [out_dir]`, or from the script editor. Writes grey_card.lila next to
this file, or into out_dir.
"""

import os
import sys

from lila import ops

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
out_dir = sys.argv[1] if len(sys.argv) > 1 else here

BALL_RADIUS = 15.0
SUN_INTENSITY = 3.0
EXPOSURE = -1.0  # stops, on the scene's post-process
STAND_HEIGHT = 28.0

# X-Rite ColorChecker Classic, sRGB 8-bit, rows top to bottom.
CHART = [
    [("DarkSkin", (115, 82, 68)), ("LightSkin", (194, 150, 130)), ("BlueSky", (98, 122, 157)),
     ("Foliage", (87, 108, 67)), ("BlueFlower", (133, 128, 177)), ("BluishGreen", (103, 189, 170))],
    [("Orange", (214, 126, 44)), ("PurplishBlue", (80, 91, 166)), ("ModerateRed", (193, 90, 99)),
     ("Purple", (94, 60, 108)), ("YellowGreen", (157, 188, 64)), ("OrangeYellow", (224, 163, 46))],
    [("Blue", (56, 61, 150)), ("Green", (70, 148, 73)), ("Red", (175, 54, 60)),
     ("Yellow", (231, 199, 31)), ("Magenta", (187, 86, 149)), ("Cyan", (8, 133, 161))],
    [("White", (243, 243, 242)), ("Neutral8", (200, 200, 200)), ("Neutral65", (160, 160, 160)),
     ("Neutral5", (122, 122, 121)), ("Neutral35", (85, 85, 85)), ("Black", (52, 52, 52))],
]
PATCH = 9.0
GAP = 1.5


def first(r):
    return r[0] if isinstance(r, list) else r


def linear(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def material(name, color, roughness=0.6, metallic=0.0):
    m = ops.createNode("material", name=name, skipSelect=True)
    ops.setAttr(m + ".baseColor", list(color))
    ops.setAttr(m + ".roughness", roughness)
    ops.setAttr(m + ".metallic", metallic)
    return m


def assign(mat, *objects):
    ops.select(*objects)
    ops.assignMaterial(mat)


ops.newScene()

ground = first(ops.polyPlane(name="Ground", width=2000.0, depth=2000.0))
assign(material("M_Ground18", (0.18, 0.18, 0.18), roughness=0.85), ground)

balls = []
for name, x, mat in (("GrayBall", -40.0, material("M_Gray18", (0.18, 0.18, 0.18), roughness=0.7)),
                     ("ChromeBall", 0.0, material("M_Chrome", (0.95, 0.95, 0.95), roughness=0.03, metallic=1.0)),
                     ("WhiteBall", 40.0, material("M_White90", (0.9, 0.9, 0.9), roughness=0.7))):
    # Ico spheres: even triangles everywhere, so no pole pinches the chrome ball's reflection.
    b = first(ops.polyIcoSphere(name=name, radius=BALL_RADIUS, subdivisions=5, position=[x, BALL_RADIUS, 0.0]))
    assign(mat, b)
    balls.append(b)
ops.group(*balls, name="Balls")

cols, rows = len(CHART[0]), len(CHART)
width = cols * PATCH + (cols + 1) * GAP
height = rows * PATCH + (rows + 1) * GAP
board = first(ops.polyCube(name="ChartBoard", width=width, height=height, depth=1.0, position=[0.0, height / 2.0, 0.0]))
assign(material("M_ChartBoard", (0.02, 0.02, 0.02), roughness=0.8), board)
patches = [board]
for r, row in enumerate(CHART):
    for c, (name, rgb) in enumerate(row):
        x = -width / 2.0 + GAP + PATCH / 2.0 + c * (PATCH + GAP)
        y = height - GAP - PATCH / 2.0 - r * (PATCH + GAP)
        p = first(ops.polyCube(name="Patch_" + name, width=PATCH, height=PATCH, depth=0.4, position=[x, y, 0.7]))
        # Matte, like the printed chart: a glossy patch would show the sky instead of its colour.
        assign(material("M_" + name, tuple(linear(v) for v in rgb), roughness=0.9), p)
        patches.append(p)
chart = ops.group(*patches, name="ColorChecker")
stand = first(ops.polyCube(name="ChartStand", width=width + 6.0, height=STAND_HEIGHT, depth=14.0,
                           position=[0.0, STAND_HEIGHT / 2.0, -45.0]))
assign(material("M_Stand18", (0.18, 0.18, 0.18), roughness=0.85), stand)
ops.setAttr(chart + ".translate", [0.0, STAND_HEIGHT, -45.0])
ops.setAttr(chart + ".rotate", [-12.0, 0.0, 0.0])

sun = ops.light(type="directional", name="Sun", intensity=SUN_INTENSITY, castShadows=True)
ops.setAttr(sun + ".rotate", [-45.0, -35.0, 0.0])

post = ops.postProcess(name="Post")
ops.setAttr(post + ".exposure", EXPOSURE)

ops.select(clear=True)
cam = ops.createNode("camera", name="Cam_GreyCard", skipSelect=True)
ops.viewPlace(cam, eye=(0, 58, 148), lookAt=(0, 27, -16), up=(0, 1, 0))
ops.saveScene(os.path.join(out_dir, "grey_card.lila"))
