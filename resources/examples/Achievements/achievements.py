"""The Lila scene that renders each Steam achievement's icon (names and descriptions are in table.py).

Each builder makes meshes through `S`, which records the material each mesh takes. Runs inside lilabatch.
"""

import math

from lila import ops

BLUE, FROST, DARK = "blue", "frost", "dark"
LOOKS = {
    BLUE: dict(baseColor=[0.02, 0.22, 0.95], metallic=0.0, roughness=0.25),
    FROST: dict(baseColor=[0.72, 0.8, 0.92], metallic=0.0, roughness=0.3),
    DARK: dict(baseColor=[0.03, 0.04, 0.06], metallic=0.0, roughness=0.3),
}


def materials():
    """Create the icon materials in the current scene. Returns their names by look."""
    return {k: ops.createMaterial(f"Icon_{k}", **v) for k, v in LOOKS.items()}


class Scene:
    def __init__(self):
        self.meshes = []

    def add(self, result, material=BLUE):
        mesh = result[0] if isinstance(result, (list, tuple)) else result
        self.meshes.append((mesh, material))
        return mesh


S = Scene()


def bevel(mesh, offset=0.6, segments=3):
    ops.autoBevel(mesh, offset=offset, segments=segments, clamp=True)
    return mesh


def faces(mesh, fn):
    """Run fn with every face of mesh selected."""
    ops.select(mesh)
    ops.selectMode("face")
    ops.selectAllComponents()
    fn()
    ops.selectMode("object")


def panel(mesh, inset=0.25, depth=-0.25):
    """Inset every face on its own and sink it, so the quad layout reads as tiles."""
    faces(mesh, lambda: ops.polyInset(thickness=inset, depth=depth, individual=True))
    return mesh


def cube(w, h, d, pos=(0, 0, 0), rot=(0, 0, 0), material=BLUE, round=0.5):
    m = S.add(ops.polyCube(width=w, height=h, depth=d, position=list(pos), rotation=list(rot)), material)
    return bevel(m, round) if round else m


def cyl(r, h, pos=(0, 0, 0), rot=(0, 0, 0), material=BLUE, sides=32, round=0.3):
    m = S.add(ops.polyCylinder(radius=r, height=h, subdivisionsAxis=sides, position=list(pos), rotation=list(rot)), material)
    return bevel(m, round) if round else m


def sphere(r, pos=(0, 0, 0), material=BLUE, fraction=1.0, rot=(0, 0, 0)):
    return S.add(ops.polySphere(radius=r, subdivisionsAxis=48, subdivisionsHeight=32, fraction=fraction,
                                position=list(pos), rotation=list(rot)), material)


def torus(r, section, pos=(0, 0, 0), rot=(0, 0, 0), material=BLUE, fraction=1.0):
    return S.add(ops.polyTorus(radius=r, sectionRadius=section, subdivisionsAxis=64, subdivisionsHeight=24,
                               fraction=fraction, position=list(pos), rotation=list(rot)), material)


def cone(r, h, pos=(0, 0, 0), rot=(0, 0, 0), material=BLUE, sides=32):
    return S.add(ops.polyCone(radius=r, height=h, subdivisionsAxis=sides, position=list(pos), rotation=list(rot)), material)


def capsule(r, h, pos=(0, 0, 0), rot=(0, 0, 0), material=BLUE):
    return S.add(ops.polyCapsule(radius=r, height=h, subdivisionsAxis=32, position=list(pos), rotation=list(rot)), material)


def text(t, size=8, depth=1.5, pos=(0, 0, 0), rot=(0, 0, 0), material=FROST, font="Inter SemiBold"):
    m = S.add(ops.textMesh(text=t, font=font, size=size, alignment="center", anchor="middle", extrudeDepth=depth,
                           frontBevel=True, frontWidth=0.15), material)
    ops.move(m, to=list(pos))
    ops.rotate(m, to=list(rot))
    return m


def tube(points, radius=0.6, material=BLUE, closed=False, bend=1.5):
    c = ops.curve(points=[list(p) for p in points], type="poly", closed=closed)
    return S.add(ops.pipe(c, radius=radius, sides=24, autoFillet=True, bendRadius=bend, startCap="round", endCap="round"), material)


def cut(base, cutter, op="difference"):
    ops.polyBoolean(base, cutter, operation=op)
    S.meshes = [(m, mat) for m, mat in S.meshes if m != cutter]
    return base


def cutter(result):
    return result[0] if isinstance(result, (list, tuple)) else result


def keycap(label, w=10, size=4.5, label2=None):
    cube(w, 10, 4, pos=(0, 0, 0), round=1.2)
    cube(w - 1.6, 8.4, 1, pos=(0, 0.3, 2.1), material=BLUE, round=0.8)
    text(label, size=size, depth=0.8, pos=(0, 0.4 if not label2 else 1.6, 2.7))
    if label2:
        text(label2, size=size * 0.55, depth=0.8, pos=(0, -2.4, 2.7))


def envelope(pos=(0, 0, 0), rot=(0, 0, 0), material=BLUE):
    """A blue envelope with a pale V flap on its front."""
    body = cube(14, 9, 1.2, material=material, round=0.3)
    flap = [cube(8.6, 0.8, 0.4, pos=(s * 3.5, 2.2, 0.75), rot=(0, 0, s * 33), material=FROST, round=0.15) for s in (-1, 1)]
    g = ops.group(body, *flap)
    ops.rotate(g, to=list(rot))
    ops.move(g, to=list(pos))
    return body


# --- builders --------------------------------------------------------------------------------------------------------

def fresh_out_of_the_oven():
    loaf = S.add(ops.polyCapsule(radius=5, height=18, subdivisionsAxis=40, rotation=[0, 0, 90]))
    ops.scale(loaf, by=[1, 0.75, 1])
    for x in (-4.5, 0, 4.5):
        c = cutter(ops.polyCube(width=1.0, height=4, depth=12, position=[x, 4.6, 0], rotation=[0, 0, -30]))
        cut(loaf, c)


def phoenix():
    torus(7, 2.2, rot=(90, 0, 0))
    for a in (0, 90, 180, 270):
        torus(7, 2.35, rot=(90, 0, 0), material=FROST, fraction=0.12)
        ops.rotate(S.meshes[-1][0], by=[0, 0, a + 40])


def snitch():
    envelope()


def frequent_flyer():
    for i in range(3):
        envelope(pos=(i * 2.5 - 2.5, i * 3 - 3, -i * 2), rot=(0, 0, (i - 1) * 6))


def ctrl_z_enthusiast():
    keycap("Z", size=6)


def freeze():
    for i in range(6):
        a = math.radians(i * 60)
        d = (-math.sin(a), math.cos(a))
        capsule(0.9, 18, pos=(0, 0, 0), rot=(0, 0, i * 60))
        for t, s in ((4.5, 4.5), (6.8, 3.2)):
            for side in (-1, 1):
                capsule(0.6, s, pos=(d[0] * t, d[1] * t, 0), rot=(0, 0, i * 60 + side * 45))
                ops.move(S.meshes[-1][0], by=[d[0] * s * 0.35, d[1] * s * 0.35, 0])
    sphere(2.2, material=FROST)


def clone_wars():
    for x in range(-2, 3):
        for z in range(-2, 3):
            cube(3, 3, 3, pos=(x * 4.5, 0, z * 4.5), material=BLUE if (x + z) % 2 else FROST, round=0.4)


def demolition_expert():
    off = [(-1, -1, -1), (1, -1, -1), (-1, 1, -1), (1, 1, -1), (-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1)]
    for i, (x, y, z) in enumerate(off):
        spread = 3.4 if y > 0 else 2.8
        cube(5, 5, 5, pos=(x * spread, y * spread + (1.2 if y > 0 else 0), z * spread),
             rot=(x * 8 * (i % 3), y * 10, z * 7), material=FROST if i in (2, 5) else BLUE, round=0.35)


def urban_planner():
    cube(26, 1.2, 26, pos=(0, -0.6, 0), material=FROST, round=0.4)
    for x, z, h, w in [(-7, -7, 14, 5), (0, -7, 20, 5), (7, -7, 10, 5), (-7, 0, 8, 5), (0.5, 0.5, 12, 6),
                       (7, 0, 16, 5), (-7, 7, 6, 5), (0, 7, 9, 5), (7, 7, 5, 5)]:
        b = cube(w, h, w, pos=(x, h / 2, z), round=0.3)
        if h > 9:
            panel(b, 0.35, -0.2)


def hello_unreal():
    torus(5, 1.3, pos=(-3.4, 0, 0), rot=(90, 0, 0))
    torus(5, 1.3, pos=(3.4, 0, 0), rot=(0, 0, 0), material=FROST)


def polygon_hoarder():
    m = S.add(ops.polyIcoSphere(radius=9))
    panel(m, 0.25, 0.9)


def unwrapped():
    for x, z in [(0, 0), (-6.2, 0), (6.2, 0), (12.4, 0), (0, -6.2), (0, 6.2)]:
        cube(6, 0.8, 6, pos=(x - 3, 0, z), material=FROST if (x, z) == (0, 0) else BLUE, round=0.25)


def retopologist():
    m = S.add(ops.polyQuadSphere(radius=9))
    panel(m, 0.22, -0.35)


def night_owl():
    moon = cyl(8, 3, rot=(90, 0, 0), sides=64, round=0)
    c = cutter(ops.polyCylinder(radius=7.0, height=6, subdivisionsAxis=64, position=[4.4, 2.8, 0], rotation=[90, 0, 0]))
    cut(moon, c)
    for x, y, s in [(7.5, 6.5, 1.8), (10.5, 0.5, 1.2)]:
        S.add(ops.polyCone(radius=s, height=0.8, subdivisionsAxis=4, position=[x, y, 0], rotation=[90, 0, 0]), FROST)


def pythonista():
    cube(22, 14, 2, round=1.0)
    text(">>>", size=7, depth=0.8, pos=(0, 0, 1.4))


def defector():
    cube(16, 11, 5, round=1.0)
    tube([(-3.5, 5.5, 0), (-3.5, 8, 0), (3.5, 8, 0), (3.5, 5.5, 0)], radius=0.8, material=FROST, bend=1.2)
    cube(16.2, 1.4, 5.2, pos=(0, 0, 0), material=FROST, round=0.3)


def ancient_ruins():
    cube(26, 1.5, 12, pos=(0, -0.75, 0), material=FROST, round=0.4)
    for x, h in [(-8, 16), (0, 9), (8, 13)]:
        cyl(2.2, h, pos=(x, h / 2, 0), sides=12, round=0.2)
        if h > 10:
            cube(6, 1.2, 6, pos=(x, h + 0.6, 0), material=FROST, round=0.3)
    cyl(2.2, 9, pos=(2, 2.2, 5), rot=(0, 30, 90), sides=12, round=0.2)


def terraformer():
    cyl(13, 1.5, pos=(0, -0.75, 0), material=FROST, round=0.4)
    for x, z, r, h, s in [(-3, -2, 7, 14, 7), (5, 1, 5, 9, 6), (-6, 5, 4, 6, 5)]:
        cone(r, h, pos=(x, h / 2, z), sides=s)


def interior_decorator():
    cube(10, 1.2, 10, pos=(0, 6, 0), round=0.4)
    cube(10, 11, 1.2, pos=(0, 12, -4.4), round=0.4)
    for x, z in [(-4.2, -4.2), (4.2, -4.2), (-4.2, 4.2), (4.2, 4.2)]:
        cube(1.2, 6, 1.2, pos=(x, 2.7, z), material=FROST, round=0.3)
    cube(8, 1.6, 8, pos=(0, 7.2, 0.4), material=FROST, round=0.6)


def pipe_dream():
    tube([(-10, -6, 0), (-2, -6, 0), (-2, 2, 0), (4, 2, 0), (4, 8, 6), (10, 8, 6)], radius=1.6, bend=3.0)
    for p in [(-10, -6, 0), (10, 8, 6)]:
        cyl(2.4, 1.2, pos=p, rot=(0, 0, 90), material=FROST)


def sculptor():
    c = ops.curve(points=[[0, 0, 0], [5, 0, 0], [4, 2, 0], [7, 8, 0], [3, 13, 0], [3.4, 16, 0], [5, 17, 0], [0, 17, 0]],
                  type="nurbs", editPoints=True)
    S.add(ops.revolve(c, axis=[0, 1, 0], sections=48))
    sphere(2.2, pos=(0, 18, 0), material=FROST)


def bones_of_contention():
    capsule(1.6, 18, rot=(0, 0, 45))
    for s in (-1, 1):
        for o in (-1, 1):
            d = 6.6 * s
            sphere(2.3, pos=(d * 0.7071 + o * 1.4 * 0.7071, d * 0.7071 - o * 1.4 * 0.7071, 0))


def bookmarked():
    rib = cube(8, 18, 1, round=0.25)
    c = cutter(ops.polyCube(width=6, height=6, depth=4, position=[0, -9, 0], rotation=[0, 0, 45]))
    cut(rib, c)
    for i in range(3):
        cyl(1.1, 0.8, pos=(0, 5 - i * 3.6, 0.6), rot=(90, 0, 0), material=FROST)


def hotboxer():
    cyl(3, 1.5, rot=(90, 0, 0), material=FROST)
    for i in range(8):
        a = math.radians(i * 45)
        cube(4.2, 2.6, 1.5, pos=(math.cos(a) * 9, math.sin(a) * 9, 0), material=BLUE, round=0.5)


def extensionist():
    p = cube(12, 12, 2.5, round=0.4)
    knob = cutter(ops.polyCylinder(radius=2.4, height=2.5, subdivisionsAxis=32, position=[8, 0, 0], rotation=[90, 0, 0]))
    cut(p, knob, "union")
    knob2 = cutter(ops.polyCylinder(radius=2.4, height=2.5, subdivisionsAxis=32, position=[0, 8, 0], rotation=[90, 0, 0]))
    cut(p, knob2, "union")
    hole = cutter(ops.polyCylinder(radius=2.5, height=4, subdivisionsAxis=32, position=[0, -6.5, 0], rotation=[90, 0, 0]))
    cut(p, hole)


def universal_translator():
    sphere(8)
    torus(11.5, 0.7, rot=(70, 0, 20), material=FROST)
    for lat in (-4, 0, 4):
        torus(math.sqrt(64 - lat * lat) + 0.05, 0.25, pos=(0, lat, 0), material=FROST)


def polyglot():
    for pos, mat, flip in [((-3, 3, 0), BLUE, 1), ((4, -3, 2), FROST, -1)]:
        cube(13, 8, 2, pos=pos, material=mat, round=1.8)
        cone(1.8, 3.5, pos=(pos[0] - 3.5 * flip, pos[1] - 5, pos[2]), rot=(0, 0, 180 + 20 * flip), material=mat, sides=3)


def time_lord():
    for y in (-9, 9):
        cyl(6, 1.4, pos=(0, y, 0), material=FROST)
    cone(4.8, 8.4, pos=(0, 4.1, 0), rot=(180, 0, 0))
    cone(4.8, 8.4, pos=(0, -4.1, 0))
    for a in (45, 165, 285):
        r = math.radians(a)
        cyl(0.5, 18, pos=(math.cos(r) * 5.4, 0, math.sin(r) * 5.4), material=FROST, sides=12, round=0)


def rainbow():
    for i, mat in enumerate([BLUE, FROST, BLUE, FROST]):
        torus(10 - i * 2.1, 1.0, rot=(90, 0, 0), fraction=0.5, material=mat)


def ship_it():
    capsule(3.2, 16, pos=(0, 0, 0))
    cone(3.2, 5, pos=(0, 10, 0), material=FROST)
    for i in range(3):
        f = cube(0.6, 5, 4, pos=(0, -5, 0), rot=(0, i * 120, 0), material=FROST, round=0.2)
        ops.move(f, by=[math.sin(math.radians(i * 120)) * 3.4, 0, math.cos(math.radians(i * 120)) * 3.4])
    cyl(1.2, 0.6, pos=(0, 3, 3.1), rot=(90, 0, 0), material=FROST)


def ez_mode():
    sphere(8, material=FROST)
    arc = []
    for k in range(17):
        u = -0.9 + k * 1.8 / 16
        v = (u, -0.45 * u + 0.25 * math.sin(3.5 * u), 0.75)
        n = math.sqrt(sum(c * c for c in v))
        arc.append(tuple(8.9 * c / n for c in v))
    c = ops.curve(points=[list(p) for p in arc], type="nurbs", editPoints=True)
    S.add(ops.pipe(c, radius=0.8, sides=24, startCap="round", endCap="round"), BLUE)
    for p in (arc[0], arc[-1]):
        sphere(1.5, pos=p)


def deja_vu():
    keycap("Enter", w=16, size=3.6)


def cheat_code():
    keycap("F1", size=4.8)


def search_party():
    for x, y, s, mat in [(-4, 2, 1.0, BLUE), (5, -1, 0.75, FROST)]:
        torus(4 * s, 0.8 * s, pos=(x, y, 0), rot=(90, 0, 0), material=mat)
        cyl(0.9 * s, 7 * s, pos=(x + 5.2 * s, y - 5.2 * s, 0), rot=(0, 0, 45), material=mat, round=0.2)


def hide_and_seek():
    sphere(4.5, pos=(4.5, 3.5, -3))
    cube(12, 12, 4, pos=(-1, 0, 0), material=FROST, round=0.8)


def stick_the_landing():
    sphere(10, pos=(0, -10, 0), material=FROST)
    cube(6, 6, 6, pos=(3.4, 2.4, 0), rot=(0, 0, -20), round=0.6)


def measure_twice():
    cube(26, 4, 1.2, rot=(0, 0, 20), material=FROST, round=0.3)
    for i in range(13):
        h = 2.0 if i % 2 == 0 else 1.2
        x = -11 + i * (22 / 12)
        a = math.radians(20)
        y = 2 - h / 2
        cube(0.35, h, 0.4, pos=(x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a), 0.7),
             rot=(0, 0, 20), material=BLUE, round=0)


def copy_that():
    for i in range(4):
        cube(5, 5, 5, pos=(i * 6.5 - 10, i * 2.5 - 4, 0), material=FROST if i == 0 else BLUE, round=0.6)


def hubcap():
    cyl(10, 2, rot=(90, 0, 0), material=FROST, sides=64, round=0.5)
    cyl(2.5, 2.8, rot=(90, 0, 0), sides=32, round=0.4)
    for i in range(5):
        m = cube(2.2, 7, 1.4, round=0.5)
        ops.rotate(m, to=[0, 0, i * 72])
        a = math.radians(i * 72)
        ops.move(m, to=[-math.sin(a) * 5.2, math.cos(a) * 5.2, 1.2])


def edge_lord():
    cube(10, 10, 10, round=0.7)


def lord_of_the_rings():
    for i in range(6):
        S.add(ops.polyPipe(radius=6, thickness=1.6, height=2.0, subdivisionsAxis=48, position=[0, i * 2.6 - 6.5, 0]),
              BLUE if i % 2 == 0 else FROST)
        bevel(S.meshes[-1][0], 0.3, 2)


def fan_fiction():
    d = S.add(ops.polyDisc(radius=9, subdivisionsAxis=24, subdivisionsRings=2, capType="quads", rotation=[90, 0, 0]))
    panel(d, 0.22, 0.6)


def sphere_of_influence():
    m = S.add(ops.polyCube(width=12, height=12, depth=12, subdivisionsX=12, subdivisionsY=12, subdivisionsZ=12))
    ops.select(m)
    ops.polySpherize(amount=0.55)


def going_in_circles():
    for i, mat in enumerate([BLUE, FROST, BLUE, FROST]):
        torus(10 - i * 2.6, 0.9, rot=(90, 0, 0), material=mat)
    sphere(1.6)


def hand_me_downs():
    tube([(-10, -2, 0), (0, 4.5, 0), (10, -2, 0), (-10, -2, 0)], radius=0.7, material=BLUE, bend=1.0)
    tube([(0, 4.5, 0), (0, 7.5, 0), (2.2, 9.5, 0), (3.6, 7.8, 0)], radius=0.6, material=FROST, bend=1.4)


def point_taken():
    cyl(1.4, 14, pos=(-2, 0, 0), rot=(0, 0, 90), round=0.2)
    cone(3.4, 6, pos=(8, 0, 0), rot=(0, 0, -90), material=FROST)


def care_package():
    cube(8, 8, 8, pos=(0, -4, 0), round=0.5)
    for y in (-0.3, -7.7):
        cube(8.4, 0.8, 8.4, pos=(0, y, 0), material=FROST, round=0.2)
    sphere(10, pos=(0, 8, 0), fraction=0.45, material=FROST)
    for x, z in [(-3.6, -3.6), (3.6, -3.6), (-3.6, 3.6), (3.6, 3.6)]:
        tube([(x, 0, z), (x * 2.4, 9, z * 2.4)], radius=0.18, material=FROST)


def assembly_line():
    for i in range(7):
        cyl(1.2, 10, pos=(i * 3 - 9, 0, 0), rot=(90, 0, 0), material=FROST, sides=24, round=0.15)
    for x in (-6, 1.5):
        cube(5, 5, 5, pos=(x, 3.7, 0), round=0.6)


def name_and_shame():
    tag = cube(16, 10, 1, round=0.8)
    hole = cutter(ops.polyCylinder(radius=1.0, height=4, subdivisionsAxis=24, position=[-5.8, 0, 0], rotation=[90, 0, 0]))
    cut(tag, hole)
    text("Aa", size=5, depth=0.6, pos=(1.5, 0, 0.8))
    tube([(-5.8, 0, 0), (-9, 4, -1), (-12, 5, -2)], radius=0.25, material=FROST)


def done_and_dusted():
    cube(3, 8, 3, pos=(-3.5, -1, 0), rot=(0, 0, 45), round=0.8)
    cube(3, 16, 3, pos=(3, 2.5, 0), rot=(0, 0, -35), round=0.8)


def going_live():
    capsule(3.6, 10, pos=(0, 4, 0), material=FROST)
    torus(5, 0.6, pos=(0, 2.5, 0), rot=(90, 0, 0), fraction=0.5)
    ops.rotate(S.meshes[-1][0], by=[0, 0, 180])
    cyl(0.6, 6, pos=(0, -5, 0), round=0)
    cyl(4, 1, pos=(0, -8, 0))


def zip_it():
    for s in (-1, 1):
        cube(3.2, 22, 0.8, pos=(s * 4.6, -2, -0.4), material=FROST, round=0.3)
    for i in range(9):
        cube(4.6, 1.4, 1.6, pos=(-1.2 if i % 2 else 1.2, i * 2.0 - 12, 0), round=0.4)
    cube(6, 4.6, 2.8, pos=(0, 6.5, 0.4), round=0.8)
    cube(2.6, 7, 0.9, pos=(0, 11, 1.6), rot=(-20, 0, 0), material=FROST, round=0.4)


def not_you():
    torus(8, 1.6, rot=(90, 0, 0))
    cube(15, 3, 3, rot=(0, 0, 45), round=0.8)


# --- views -----------------------------------------------------------------------------------------------------------

# Each achievement's builder and camera (yaw, pitch), by API name.
BUILDERS = {
    "FRESH_OUT_OF_THE_OVEN": (fresh_out_of_the_oven, (30, -25)),
    "PHOENIX": (phoenix, (20, -15)),
    "SNITCH": (snitch, (20, -15)),
    "FREQUENT_FLYER": (frequent_flyer, (20, -15)),
    "CTRL_Z_ENTHUSIAST": (ctrl_z_enthusiast, (20, -15)),
    "FREEZE": (freeze, (15, -10)),
    "CLONE_WARS": (clone_wars, (35, -35)),
    "DEMOLITION_EXPERT": (demolition_expert, (35, -25)),
    "URBAN_PLANNER": (urban_planner, (35, -30)),
    "HELLO_UNREAL": (hello_unreal, (30, -20)),
    "POLYGON_HOARDER": (polygon_hoarder, (30, -20)),
    "UNWRAPPED": (unwrapped, (20, -50)),
    "RETOPOLOGIST": (retopologist, (30, -20)),
    "NIGHT_OWL": (night_owl, (10, -25)),
    "PYTHONISTA": (pythonista, (20, -15)),
    "DEFECTOR": (defector, (30, -15)),
    "ANCIENT_RUINS": (ancient_ruins, (30, -20)),
    "TERRAFORMER": (terraformer, (30, -25)),
    "INTERIOR_DECORATOR": (interior_decorator, (40, -20)),
    "PIPE_DREAM": (pipe_dream, (25, -20)),
    "SCULPTOR": (sculptor, (20, -15)),
    "BONES_OF_CONTENTION": (bones_of_contention, (15, -10)),
    "BOOKMARKED": (bookmarked, (20, -10)),
    "HOTBOXER": (hotboxer, (15, -10)),
    "EXTENSIONIST": (extensionist, (20, -15)),
    "UNIVERSAL_TRANSLATOR": (universal_translator, (25, -15)),
    "POLYGLOT": (polyglot, (20, -10)),
    "TIME_LORD": (time_lord, (25, -12)),
    "RAINBOW": (rainbow, (15, -10)),
    "SHIP_IT": (ship_it, (30, -10)),
    "EZ_MODE": (ez_mode, (15, -10)),
    "DEJA_VU": (deja_vu, (20, -15)),
    "CHEAT_CODE": (cheat_code, (20, -15)),
    "SEARCH_PARTY": (search_party, (15, -10)),
    "HIDE_AND_SEEK": (hide_and_seek, (25, -15)),
    "STICK_THE_LANDING": (stick_the_landing, (25, -15)),
    "MEASURE_TWICE": (measure_twice, (20, -20)),
    "COPY_THAT": (copy_that, (30, -20)),
    "HUBCAP": (hubcap, (20, -15)),
    "EDGE_LORD": (edge_lord, (35, -30)),
    "LORD_OF_THE_RINGS": (lord_of_the_rings, (30, -25)),
    "FAN_FICTION": (fan_fiction, (20, -15)),
    "SPHERE_OF_INFLUENCE": (sphere_of_influence, (35, -25)),
    "GOING_IN_CIRCLES": (going_in_circles, (15, -10)),
    "HAND_ME_DOWNS": (hand_me_downs, (10, -5)),
    "POINT_TAKEN": (point_taken, (25, -15)),
    "CARE_PACKAGE": (care_package, (25, -15)),
    "ASSEMBLY_LINE": (assembly_line, (30, -25)),
    "NAME_AND_SHAME": (name_and_shame, (20, -15)),
    "DONE_AND_DUSTED": (done_and_dusted, (15, -10)),
    "GOING_LIVE": (going_live, (20, -10)),
    "ZIP_IT": (zip_it, (20, -10)),
    "NOT_YOU": (not_you, (15, -10)),
}
