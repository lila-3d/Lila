"""The agent modeling workflow end to end, headless: lilabatch --batch agent_modeling.py

Builds the guide's low-poly dog and checks it is one closed, manifold, symmetric shell with its parts extruded where
they belong, then joins a separately built part with the guide's repair recipe. LILA_AGENT_OUT, when set, receives
dog.obj for a look.
"""

import os
import sys
import tempfile

from lila import cmds

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.join(HERE, "..", "..", "resources", "agents", "examples", "low_poly_dog.py")
OUT = os.environ.get("LILA_AGENT_OUT") or tempfile.mkdtemp(prefix="lila_agent_")
os.environ.setdefault("LILA_CAPTURE_DIR", OUT)

ns = {"__name__": "low_poly_dog"}
exec(compile(open(EXAMPLE, encoding="utf-8").read(), EXAMPLE, "exec"), ns)
dog = ns["dog"]

r = cmds.meshCheck(dog)
assert r["ok"], r["issues"]
assert r["shells"] == 1 and r["borderEdges"] == 0 and r["nonManifoldEdges"] == 0 and r["nonManifoldVerts"] == 0, r
assert r["ngons"] == 0 and r["tris"] == 0, "all quads"
assert 150 <= r["faces"] <= 400, f"very low poly, got {r['faces']} faces"
assert r["symmetry"]["maxAsymmetry"] < 1e-6 and r["symmetry"]["unmatched"] == 0, r["symmetry"]
(lo, hi) = r["box"]
assert abs(lo[0] + hi[0]) < 1e-6, "centred on x=0"
assert abs(lo[1]) < 1e-6, "feet on the ground"
assert set(cmds.meshInfo(dog)["materialSlots"]) == {"DogFur", "DogNose", "DogEye"}


def centers(**query):
    ids = cmds.findComponents(dog, kind="face", **query)
    return [f["center"] for f in cmds.componentInfo(dog, kind="face", ids=ids, fields=["center"])]


# Four legs reach the ground: a foot in each quadrant.
feet = centers(normal=(0, -1, 0), box=[[None, None, None], [None, 0.01, None]])
assert {(x > 0, z > 0) for x, _, z in feet} == {(True, True), (True, False), (False, True), (False, False)}, feet
# The ears are the widest part and hang well below the top of the head, on both sides.
for side in ("+x", "-x"):
    x, y, _ = centers(extreme=side)[0]
    assert abs(x) > 9 and y < hi[1] - 8, (side, x, y)
# The snout and nose stick out in front of the eyes.
eyes = centers(normal=(0, 0, 1), side="+x", extreme="+y")
assert all(z < hi[2] - 3 for _, _, z in eyes), eyes

cmds.exportObj(os.path.join(OUT, "dog.obj"), dog)

# The repair recipe: a part built on its own, joined to the body so it is one shell again.
cmds.newScene()
body, _ = cmds.polyCube(name="Body", width=10, height=10, depth=10, position=(0, 5, 0))
part, _ = cmds.polyCube(name="Part", width=4, height=4, depth=4, position=(0, 5, 9))
both = cmds.polyCombine(body, part, name="Body")
assert cmds.meshCheck(both)["shells"] == 2
cmds.selectComponentsWhere(both, kind="face", shell=0, extreme="+z")
cmds.selectComponentsWhere(both, kind="face", shell=1, extreme="-z", add=True)
cmds.deleteComponents()
cmds.selectComponentsWhere(both, kind="edge", border=True)
cmds.polyBridge()
joined = cmds.meshCheck(both)
assert joined["ok"] and joined["shells"] == 1, joined

print("agent modeling OK:", r["faces"], "faces,", r["triangles"], "triangles; out", OUT)
sys.stdout.flush()
