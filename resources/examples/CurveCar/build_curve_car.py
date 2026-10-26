"""Builds the Curve Car example: a 1960s Cobra 427 roadster modelled from curves. The body is lofted through cross-section
curves built from a table of profile rows (crown, hips, sill); the wheel wells, cockpit, grille mouth, lamp buckets, vents
and panel gaps are cut with one live boolean; the wheels, windscreen, pipes, trim and seats are revolved, lofted, piped and
bevelled from curves.

Run with `lilabatch --batch build_curve_car.py [out_dir]`, or from the script editor. Writes curve_car.lila next to this
file, or into out_dir. Units are cm, Y up, the car faces +X with the wheelbase centred on the origin.
"""

import math
import os
import sys

from lila import ops

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
out_dir = sys.argv[1] if len(sys.argv) > 1 else here

WHEELBASE = 228.6
FRONT_AXLE, REAR_AXLE = WHEELBASE / 2, -WHEELBASE / 2
FRONT_TRACK, REAR_TRACK = 142.0, 147.0
FRONT_TYRE = (32.5, 21.0)  # radius, width
REAR_TYRE = (33.5, 29.0)

# Body section rows, nose to tail (a loft takes at most 12):
# (x, bottom, centre top, crown height, crown offset, hip half width, hip height, sill half width).
# The crown is the top of each fender; the centre top is the bonnet or deck between them, lower at the wheels.
BODY = [
    (196, 28, 48, 50, 14, 24, 39, 18),
    (188, 22, 55, 63, 38, 52, 43, 46),
    (168, 18, 62, 73, 52, 70, 50, 64),
    (FRONT_AXLE, 17, 70, 81, 63, 83, 57, 72),
    (68, 17, 76, 80, 61, 78, 59, 74),
    (30, 17, 82, 82, 59, 74.5, 61, 74),
    (-30, 17, 84, 85, 61, 75.5, 62, 74.5),
    (-75, 17, 87, 90, 66, 83, 63, 77),
    (REAR_AXLE, 18, 88, 94, 68, 89, 64, 79),
    (-160, 23, 87, 90, 62, 82, 61, 74),
    (-190, 31, 78, 79, 52, 70, 57, 60),
    (-205, 44, 63, 63, 8, 14, 54, 8),
]

guides = []  # construction curves, hidden at the end


def first(r):
    return r[0] if isinstance(r, list) else r


def ep(points, name, closed=False):
    guides.append(ops.curveEP(points=[list(p) for p in points], closed=closed, name=name))
    return guides[-1]


def poly(points, name, closed=False):
    guides.append(ops.curve(type="poly", points=[list(p) for p in points], closed=closed, name=name))
    return guides[-1]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def add(a, b, k=1.0):
    return tuple(a[i] + b[i] * k for i in range(3))


def unit(v):
    n = math.sqrt(dot(v, v))
    return tuple(c / n for c in v)


def side_tag(side):
    return "L" if side < 0 else "R"


def half_section(bottom, top, crown_y, crown_z, hip_z, hip_y, sill_z):
    """One side of a body section from the top centre round the fender and down to the bottom centre."""
    rise = crown_y - top
    side = crown_y - hip_y
    return [
        (0.0, top),
        (crown_z * 0.45, top + rise * 0.55),
        (crown_z * 0.8, crown_y - rise * 0.05),
        (crown_z, crown_y),
        (crown_z + (hip_z - crown_z) * 0.62, crown_y - side * 0.18),
        (hip_z - (hip_z - crown_z) * 0.1, crown_y - side * 0.55),
        (hip_z, hip_y),
        (hip_z - (hip_z - sill_z) * 0.35, hip_y - (hip_y - bottom) * 0.45),
        (sill_z, bottom + 6),
        (sill_z - 6, bottom),
        (sill_z * 0.5, bottom),
        (0.0, bottom),
    ]


def body_section(name, row):
    x, *shape = row
    half = half_section(*shape)
    ring = [(x, y, z) for z, y in half] + [(x, y, -z) for z, y in reversed(half[1:-1])]
    return ep(ring, name, closed=True)


class Surface:
    """Ray casts against the body as lofted, before any cut: places trim, lamps and panel gaps on its skin."""

    def __init__(self, mesh):
        data = ops.meshData(mesh, worldSpace=True)
        self.tris = []
        for f in data["faces"]:
            pts = [data["points"][i] for i in f]
            self.tris += [(pts[0], pts[k], pts[k + 1]) for k in range(1, len(pts) - 1)]

    def hit(self, origin, direction):
        best = None
        for a, b, c in self.tris:
            e1 = [b[i] - a[i] for i in range(3)]
            e2 = [c[i] - a[i] for i in range(3)]
            p = cross(direction, e2)
            det = dot(e1, p)
            if abs(det) < 1e-9:
                continue
            s = [origin[i] - a[i] for i in range(3)]
            u = dot(s, p) / det
            if u < 0 or u > 1:
                continue
            q = cross(s, e1)
            v = dot(direction, q) / det
            if v < 0 or u + v > 1:
                continue
            t = dot(e2, q) / det
            if t > 0 and (best is None or t < best):
                best = t
        if best is None:
            raise RuntimeError(f"no body surface along {origin} {direction}")
        return tuple(origin[i] + direction[i] * best for i in range(3))


def material(name, color, roughness=0.5, metallic=0.0):
    ops.createMaterial(name, baseColor=color, roughness=roughness, metallic=metallic)
    return name


ops.newScene()
paint = material("M_GuardsmanBlue", (0.05, 0.12, 0.42), roughness=0.2)
glass = material("M_Glass", (0.05, 0.06, 0.08), roughness=0.05)
rubber = material("M_Tyre", (0.03, 0.03, 0.03), roughness=0.9)
chrome = material("M_Chrome", (0.92, 0.92, 0.94), roughness=0.08, metallic=1.0)
alloy = material("M_Magnesium", (0.55, 0.55, 0.52), roughness=0.35, metallic=1.0)
leather = material("M_Leather", (0.08, 0.05, 0.04), roughness=0.6)
dark = material("M_Grille", (0.02, 0.02, 0.02), roughness=0.7)
lamp = material("M_Lamp", (1.0, 0.97, 0.88), roughness=0.05)
tail = material("M_TailLamp", (0.75, 0.03, 0.02), roughness=0.1)

body_sections = [body_section(f"BodySection{n}", row) for n, row in enumerate(BODY)]
body = first(ops.loft(*body_sections, caps=True, smooth=True, spansBetween=4, name="Body"))
ops.assignMaterial(paint, body)
skin = Surface(body)


def side_point(x, y, side, inset=0.0):
    """Where the flank is at (x, y) on one side, moved inset cm into the body."""
    p = skin.hit((x, y, side * 200.0), (0, 0, -side))
    return (p[0], p[1], p[2] - side * inset)


def top_point(x, z, inset=0.0):
    p = skin.hit((x, 300.0, z), (0, -1, 0))
    return (p[0], p[1] - inset, p[2])


def slab(name, outline, normal, depth):
    return first(ops.bevelPlus(poly(outline, name + "Outline", closed=True), extrudeDepth=depth, frontBevel=False, backBevel=False,
                               normal=normal, name=name))


cutters = []

# Wheel wells: a cylinder per wheel from inside the tyre out past the flank.
wheels_at = [(FRONT_AXLE, FRONT_TRACK / 2, FRONT_TYRE), (REAR_AXLE, REAR_TRACK / 2, REAR_TYRE)]
for x, half_track, (radius, width) in wheels_at:
    for side in (1, -1):
        inner = half_track - width / 2 - 8
        cutters.append(first(ops.polyCylinder(name=f"WellCutter{'F' if x > 0 else 'R'}{side_tag(side)}", radius=radius + 4.5,
                                              height=120 - inner, subdivisionsAxis=48, rotation=(90, 0, 0),
                                              position=(x, radius, side * (inner + 120) / 2))))


def cockpit_outline(y, n=28):
    pts = []
    for k in range(n):
        c, s = math.cos(2 * math.pi * k / n), math.sin(2 * math.pi * k / n)
        pts.append((-16 + 50 * math.copysign(abs(c) ** 0.35, c), y, 56 * math.copysign(abs(s) ** 0.3, s) * (1 - 0.06 * c)))
    return pts


# Cockpit: a rounded opening lofted down into the body, which leaves the tub floor and the dash wall.
cutters.append(first(ops.loft(*[ep(cockpit_outline(y), f"CockpitOutline{n}", closed=True) for n, y in enumerate((44, 140))],
                              caps=True, name="CockpitCutter")))


def mouth_outline(x, scale):
    return [(x, 37 + 14 * scale * math.sin(2 * math.pi * k / 24) - 1.5 * scale * math.cos(4 * math.pi * k / 24),
             38 * scale * math.cos(2 * math.pi * k / 24)) for k in range(24)]


# Grille mouth: an oval pushed back into the nose.
cutters.append(first(ops.loft(*[ep(mouth_outline(x, s), f"MouthOutline{n}", closed=True) for n, (x, s) in enumerate(((176, 0.92), (215, 1.0)))],
                              caps=True, name="MouthCutter")))

# Headlamp buckets: recesses along each lamp's axis into the fender fronts.
lamp_axis = unit((1.0, 0.18, 0.0))
lamps_at = []
for side in (1, -1):
    face = skin.hit((260, 62, side * 51.0), (-1, 0, 0))
    lamps_at.append((side, face))
    cutters.append(first(ops.polyCylinder(name=f"LampCutter{side_tag(side)}", radius=9.8, height=40, subdivisionsAxis=40, normal=lamp_axis,
                                          position=add(face, lamp_axis, 16))))

# Side vents behind the front wheels.
for side in (1, -1):
    depth = 100 - side_point(70, 51, side)[2] * side + 3
    outline = [(77, 42, side * 100), (68, 43, side * 100), (64, 51, side * 100), (66, 59, side * 100), (73, 60, side * 100), (79, 53, side * 100)]
    cutters.append(first(ops.bevelPlus(ep(outline, f"VentOutline{side_tag(side)}", closed=True), extrudeDepth=depth, frontBevel=False,
                                       backBevel=False, normal=(0, 0, -side), name=f"VentCutter{side_tag(side)}")))

# Panel gaps: thin slabs whose inner edge runs 1 cm under the skin. Door shut lines down each flank, bonnet and boot lids on top.
for side in (1, -1):
    for n, x in enumerate((30.0, -62.0)):
        edge = [side_point(x, y, side, 1.0) for y in range(24, 73, 6)]
        outline = edge + [(x, 110, side * 30), (x, 110, side * 140), (x, 24, side * 140)]
        cutters.append(slab(f"DoorGap{n}{side_tag(side)}", outline, (1, 0, 0), 0.6))
    edge = [side_point(x, 25, side, 1.0) for x in range(-62, 31, 8)]
    cutters.append(slab(f"DoorSillGap{side_tag(side)}", edge + [(30, 25, side * 140), (-62, 25, side * 140)], (0, 1, 0), 0.6))

for name, lo, hi, half in (("Bonnet", 40.0, 156.0, 42.0), ("Boot", -176.0, -98.0, 40.0)):
    for side in (1, -1):
        edge = [top_point(lo + (hi - lo) * k / 12, side * half, 1.0) for k in range(13)]
        cutters.append(slab(f"{name}Gap{side_tag(side)}", edge + [(hi, 160, side * half), (lo, 160, side * half)], (0, 0, 1), 0.6))
    for x in (lo, hi) if name == "Boot" else (hi,):
        edge = [top_point(x, -half + 2 * half * k / 12, 1.0) for k in range(13)]
        cutters.append(slab(f"{name}Gap{'F' if x == hi else 'B'}", edge + [(x, 160, half), (x, 160, -half)], (1, 0, 0), 0.6))

ops.polyBoolean(body, *cutters, operation="difference", live=True)
ops.hide(*cutters)

# Wheels: tyres, cast centres and two-eared knock-off spinners.
wheels = []
for x, half_track, (radius, width) in wheels_at:
    for side in (1, -1):
        tag = f"{'F' if x > 0 else 'R'}{side_tag(side)}"
        c = (x, radius, side * half_track)
        hw = width / 2

        def p(r, dz):
            return (c[0], c[1] + r, c[2] + side * dz)

        tyre = first(ops.revolve(ep([p(19.5, -hw + 1.5), p(radius - 8, -hw), p(radius - 2.5, -hw + 0.4), p(radius - 0.6, -hw + 2), p(radius, -hw + 4),
                                     p(radius, 0), p(radius, hw - 4), p(radius - 0.6, hw - 2), p(radius - 2.5, hw - 0.4), p(radius - 8, hw),
                                     p(19.5, hw - 1.5)], f"TyreProfile{tag}"),
                                 axisOrigin=c, axis=(0, 0, 1), sections=64, name=f"Tyre{tag}"))
        rim = first(ops.revolve(poly([p(19.5, -hw + 2), p(20.5, hw - 2.5), p(20.5, hw - 0.8), p(18.6, hw - 1.4), p(15.5, hw - 4.5), p(9.5, hw - 3.5),
                                      p(7.5, hw + 0.5), p(5.5, hw + 1.5), p(0, hw + 1.5)], f"RimProfile{tag}"),
                                axisOrigin=c, axis=(0, 0, 1), sections=48, name=f"Rim{tag}"))
        ears = [(0, 10), (2.2, 8.5), (3.6, 3), (5, 0), (3.6, -3), (2.2, -8.5), (0, -10), (-2.2, -8.5), (-3.6, -3), (-5, 0), (-3.6, 3), (-2.2, 8.5)]
        spinner = first(ops.bevelPlus(ep([(c[0] + dx, c[1] + dy, c[2] + side * (hw + 1.5)) for dx, dy in ears], f"SpinnerOutline{tag}", closed=True),
                                      extrudeDepth=2.5, frontProfile="convex", frontWidth=0.8, frontDepth=0.8, normal=(0, 0, side), name=f"Spinner{tag}"))
        windows = []
        for k in range(8):
            a = 2 * math.pi * k / 8
            ca, sa = math.cos(a), math.sin(a)
            outline = [(c[0] + ca * r - sa * w, c[1] + sa * r + ca * w, c[2] + side * (hw - 2.9)) for r, w in
                       ((10.5, 0), (11.2, 1.6), (13.5, 2.2), (15, 1.4), (15.3, 0), (15, -1.4), (13.5, -2.2), (11.2, -1.6))]
            windows.append(first(ops.bevelPlus(ep(outline, f"RimWindowOutline{tag}{k}", closed=True), extrudeDepth=0.4, frontBevel=False,
                                               backBevel=False, normal=(0, 0, side), name=f"RimWindow{tag}{k}")))
        ops.assignMaterial(rubber, tyre, *windows)
        ops.assignMaterial(alloy, rim)
        ops.assignMaterial(chrome, spinner)
        wheels += [tyre, rim, spinner, *windows]


def screen_rail(x, y_of, half, bow, n=9):
    return [(x + bow * (1 - (z / half) ** 2), y_of(z), z) for z in [-half + 2 * half * k / (n - 1) for k in range(n)]]


# Windscreen: glass lofted between two bowed rails, framed by a chrome pipe.
cowl = top_point(38, 0)[1]
base = screen_rail(37, lambda z: top_point(37, z)[1] - 1.0, 60, 4)
top = screen_rail(21, lambda z: cowl + 31, 57, 3)
screen = first(ops.loft(ep(base, "ScreenBase"), ep(top, "ScreenTop"), name="Windscreen"))
frame = first(ops.pipe(poly([base[0], *top, base[-1]], "ScreenFramePath"), radius=1.3, sides=12, autoFillet=True, bendRadius=4, name="ScreenFrame"))
ops.assignMaterial(glass, screen)
ops.assignMaterial(chrome, frame)

# Headlamps: a lens and chrome bezel in each bucket.
details = []
for side, face in lamps_at:
    lens = first(ops.polySphere(name=f"Headlamp{side_tag(side)}", radius=8.6, subdivisionsAxis=32, subdivisionsHeight=16, normal=lamp_axis,
                                position=add(face, lamp_axis, -5.0)))
    ops.scale(lens, by=(1, 0.45, 1))
    bezel = first(ops.polyTorus(name=f"HeadlampBezel{side_tag(side)}", radius=9.4, sectionRadius=1.0, subdivisionsAxis=40, subdivisionsHeight=10,
                                normal=lamp_axis, position=add(face, lamp_axis, -0.8)))
    ops.assignMaterial(lamp, lens)
    ops.assignMaterial(chrome, bezel)
    details += [lens, bezel]

# Tail lamps: twin round lamps each side.
for side in (1, -1):
    for n, z in enumerate((38.0, 51.0)):
        tag = f"{side_tag(side)}{n}"
        face = skin.hit((-300, 62, side * z), (1, 0, 0))
        lens = first(ops.polySphere(name=f"TailLamp{tag}", radius=5, subdivisionsAxis=24, subdivisionsHeight=12, normal=(-1, 0, 0),
                                    position=add(face, (1, 0, 0), 1.0)))
        ops.scale(lens, by=(1, 0.5, 1))
        bezel = first(ops.polyTorus(name=f"TailLampBezel{tag}", radius=5.2, sectionRadius=0.6, subdivisionsAxis=32, subdivisionsHeight=8,
                                    normal=(-1, 0, 0), position=add(face, (-1, 0, 0), 0.3)))
        ops.assignMaterial(tail, lens)
        ops.assignMaterial(chrome, bezel)
        details += [lens, bezel]

# Grille: a dark mesh at the back of the mouth with a chrome bar across it.
grille = first(ops.bevelPlus(ep(mouth_outline(177, 0.9), "GrilleOutline", closed=True), extrudeDepth=1, frontBevel=False, backBevel=False,
                             normal=(1, 0, 0), name="Grille"))
mouth_lip = skin.hit((300, 37, 0), (-1, 0, 0))[0]
bar = first(ops.pipe(ep([(mouth_lip - 9 + 6 * (1 - (z / 33) ** 2), 37, z) for z in (-33, -16, 0, 16, 33)], "GrilleBarPath"), radius=1.1, sides=10,
                     name="GrilleBar"))
ops.assignMaterial(dark, grille)
ops.assignMaterial(chrome, bar)
details += [grille, bar]

# Bumperettes: chrome hoops following the nose and tail, their ends tucked into the body.
for end, z_at, ys in (("Front", 41.0, (22, 28, 36, 44, 50)), ("Rear", 36.0, (34, 42, 50, 57))):
    direction = (-1, 0, 0) if end == "Front" else (1, 0, 0)
    for side in (1, -1):
        hits = [skin.hit((-300 * direction[0], y, side * z_at), direction) for y in ys]
        points = [add(hits[0], direction, 2), *[add(h, direction, -3.5) for h in hits], add(hits[-1], direction, 2)]
        hoop = first(ops.pipe(ep(points, f"{end}BumperettePath{side_tag(side)}"), radius=1.5, sides=12, name=f"{end}Bumperette{side_tag(side)}"))
        ops.assignMaterial(chrome, hoop)
        details.append(hoop)

# Side exhausts: a pipe each side along the sill, out of the body behind the front wheel.
for side in (1, -1):
    sill = side_point(20, 22, side)[2] * side
    path = ep([(74, 30, side * (sill - 6)), (64, 23, side * (sill + 4)), (40, 21, side * (sill + 7)), (-40, 21, side * (sill + 7)),
               (-74, 21, side * (sill + 7))], f"SidePipePath{side_tag(side)}")
    pipe = first(ops.pipe(path, radius=5, sides=20, endCap="round", name=f"SidePipe{side_tag(side)}"))
    ops.assignMaterial(chrome, pipe)
    details.append(pipe)


def scoop_section(x, half, rise, n=14):
    y = top_point(x, 0)[1]
    pts = [(x, y + rise * abs(math.sin(math.pi * k / (n - 1))) ** 0.6, half * math.cos(math.pi * k / (n - 1))) for k in range(n)]
    return pts + [(x, y - 3, -half * 0.9), (x, y - 3, half * 0.9)]


# Bonnet scoop: a low lofted intake down the bonnet's centre line.
scoop_rows = ((128, 17, 5.0), (110, 19, 4.5), (80, 16, 2.8), (58, 9, 0.8))
scoop = first(ops.loft(*[ep(scoop_section(*row), f"ScoopSection{n}", closed=True) for n, row in enumerate(scoop_rows)], caps=True, smooth=True,
                       name="BonnetScoop"))
scoop_y = top_point(128, 0)[1]
scoop_mouth = first(ops.bevelPlus(ep([(128.6, scoop_y - 0.2 + 4.0 * abs(math.sin(math.pi * k / 11)) ** 0.6, 15.5 * math.cos(math.pi * k / 11)) for k in range(12)],
                                     "ScoopMouthOutline", closed=True), extrudeDepth=0.5, frontBevel=False, backBevel=False, normal=(1, 0, 0),
                                  name="ScoopMouth"))
ops.assignMaterial(paint, scoop)
ops.assignMaterial(dark, scoop_mouth)
details += [scoop, scoop_mouth]

# Fuel filler on the boot, right of centre.
filler_at = top_point(-188, 28)
filler = first(ops.polyCylinder(name="FuelFiller", radius=5.5, height=1.6, subdivisionsAxis=32, position=add(filler_at, (0, 1, 0), 0.4)))
filler_rim = first(ops.polyTorus(name="FuelFillerRim", radius=5.6, sectionRadius=0.6, subdivisionsAxis=32, subdivisionsHeight=8,
                                 position=add(filler_at, (0, 1, 0), 0.9)))
ops.assignMaterial(chrome, filler, filler_rim)
details += [filler, filler_rim]

# Interior: bucket seats, a wheel on the left (US cars drove from the left) and its column.
interior = []
floor_y = 44
seat_lean = math.radians(20)
for side in (1, -1):
    z = side * 29
    cushion = first(ops.bevelPlus(ep([(-20, floor_y + 6, z - 19), (-44, floor_y + 6, z - 21), (-56, floor_y + 6, z), (-44, floor_y + 6, z + 21),
                                      (-20, floor_y + 6, z + 19), (-14, floor_y + 6, z)], f"CushionOutline{side_tag(side)}", closed=True),
                                  extrudeDepth=7, frontProfile="convex", frontWidth=3, frontDepth=3, normal=(0, 1, 0), name=f"SeatCushion{side_tag(side)}"))
    back_outline = [(-58 - math.sin(seat_lean) * h, floor_y + 8 + math.cos(seat_lean) * h, z + w)
                    for h, w in ((0, -20), (26, -20), (40, -14), (44, 0), (40, 14), (26, 20), (0, 20))]
    back = first(ops.bevelPlus(ep(back_outline, f"SeatBackOutline{side_tag(side)}", closed=True), extrudeDepth=8, frontProfile="convex", frontWidth=3,
                               frontDepth=3, normal=(math.cos(seat_lean), math.sin(seat_lean), 0), name=f"SeatBack{side_tag(side)}"))
    ops.assignMaterial(leather, cushion, back)
    interior += [cushion, back]

wheel_axis = unit((-1.0, 0.55, 0.0))
hub = (8, 80, -29)
across = unit(cross(wheel_axis, (0, 0, 1)))
steering = first(ops.polyTorus(name="SteeringWheel", radius=19, sectionRadius=1.3, subdivisionsAxis=48, subdivisionsHeight=10, normal=wheel_axis,
                               position=hub))
spokes = []
for k in range(3):
    a = math.radians(90 + 120 * k)
    rim_point = add(add(hub, across, 18.5 * math.cos(a)), (0, 0, 1), 18.5 * math.sin(a))
    spokes.append(first(ops.pipe(poly([hub, rim_point], f"SpokePath{k}"), radius=0.7, sides=8, name=f"Spoke{k}")))
column = first(ops.pipe(poly([hub, add(hub, wheel_axis, -40)], "ColumnPath"), radius=2, sides=12, name="SteeringColumn"))
ops.assignMaterial(leather, steering)
ops.assignMaterial(chrome, *spokes, column)
interior += [steering, *spokes, column]

ops.hide(*guides)
ops.group(*body_sections, body, *cutters, screen, frame, name="Shell")
ops.group(*wheels, name="Wheels")
ops.group(*details, name="Details")
ops.group(*interior, name="Interior")
ops.group(*[g for g in guides if g not in body_sections], name="Guides")
ops.group("Shell", "Wheels", "Details", "Interior", "Guides", name="CurveCar")

ops.select(clear=True)
cam = ops.createNode("camera", name="Cam_Car", skipSelect=True)
ops.viewPlace(cam, eye=(560, 210, 520), lookAt=(0, 55, 0), up=(0, 1, 0))
ops.saveScene(os.path.join(out_dir, "curve_car.lila"))
