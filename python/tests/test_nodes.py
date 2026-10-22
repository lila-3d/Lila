"""Run with: lila --batch -c "exec(open(r'python/tests/test_nodes.py').read())" """
import lila
from lila import cmds

cmds.newScene()
a = lila.locator("A", position=(1, 2, 3))
assert a.name == "A"
assert tuple(a.translate) == (1, 2, 3), a.translate
a.translate_y = 10
assert a.translateY == 10
a.name = "Renamed"
assert a.name == "Renamed" and a.exists
b = lila.locator("B")
b.parent = a
assert b.parent == a and a.children == [b]
b.parent = None
assert b.parent is None
g = lila.group(a, b, name="Grp")
assert {c.name for c in g.children} == {"Renamed", "B"}
cmds.select("B", "Renamed")
assert [n.name for n in lila.selected()] == ["B", "Renamed"]
a.move(0, 5, 0)
assert abs(a.world_position[1] - 15) < 1e-9
a.rotate_by(0, 90, 0)
assert abs(a.rotate_y - 90) < 1e-9
assert "translate" in a.attrs(keyable=True)
cmds.parent(b, a)  # Node objects pass to commands by name
assert b.parent == a
cmds.undo()
assert b.parent == g
a.delete()
assert not a.exists
print("test_nodes: ok")

tri = lila.nodes.mesh([(0, 0, 0), (100, 0, 0), (0, 100, 0), (100, 100, 0)], [[0, 1, 3, 2]], name="Quad")
assert len(tri.points) == 4 and tri.faces == [[0, 1, 3, 2]]
assert tri.mesh_info()["faces"] == 1
print("test_nodes mesh: ok")

lay = lila.nodes.display_layer(tri, name="Props")
assert tri.display_layer == lay and [n.name for n in lila.nodes.display_layers()] == ["Props"]
lay.visibility = False
assert cmds.displayLayer("Props")["visibility"] is False
tri.display_layer = None
assert tri.display_layer is None
print("test_nodes display layers: ok")
