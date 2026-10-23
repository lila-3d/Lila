# Modeling in Lila from code

Python for the `execute_python` tool, where `ops` is `lila.ops` (the same code runs headless with `lilabatch --batch script.py`). Units are cm, Y is up, the asset's front faces +Z. A small dog is about 30 cm tall, a door 220 cm, a crate 60 cm.

## Rule zero: one connected mesh

A creature or prop is ONE mesh grown out of ONE primitive. The head, legs, ears, snout and tail are faces of the body pulled out with `polyExtrude`; eyes and a nose are faces of the head inset, bulged and given a material. Never make a second primitive for a body part and push it against the first: that is a floating shell with a visible seam, and `meshCheck` fails it. Separate parts only when the user asks for them (say, removable eyeballs), and then seat them into the surface.

## Spend few tokens

- Write each stage (blockout, features, finish) as ONE `execute_python` call with many commands. Print one line at the end, not the result of every command.
- Track progress with `mesh_check` (a few lines), not `mesh_info` or `scene_summary`.
- Look with `capture_turntable(views=["front", "right", "top", "persp"], display="shaded")`: one image, 320 px tiles. Three or four captures for a whole model is normal. Do not raise the resolution to judge proportions.
- `componentInfo(..., fields=["id", "center"])` trims what comes back; `findComponents` returns only ids.
- The recipes below are enough for most models; read the example (`read_guide` topic `example`) only if stuck.

## The workflow

1. Plan in a few lines: the main masses with sizes in cm and which face each part grows out of.
2. Base: one `polyCube` split down the middle (`subdivisionsX=2`, so a loop lies on x=0) with as many rows along its length as you need attachment points (`subdivisionsZ=3`: front legs, middle, back legs). Then `ops.setSymmetry(mesh, axis="x")` and work on the +x side; -x follows.
3. Block out: extrude the limbs, neck and head out of the base's faces. `mesh_check` + one capture.
4. Features: an edge loop where the head needs more faces, then snout, nose, eyes, ears, tail, all from faces of the same mesh. `mesh_check` + one capture.
5. Shape: `moveComponents` / `scaleComponents` on faces, edges or loops to taper and round the silhouette. Compare front, side and top views with what was asked (big head, short legs, floppy ears...).
6. Optional smoothing (below), materials, a meaningful name.
7. Finish: `ops.meshCheck(mesh)` must say `ok` with 1 shell (closed, manifold), then a final capture.

## Recipes

Each edit works on the current component selection and leaves its result selected (the extruded caps, the new loop), so transforms chain. `polyExtrude(thickness=...)` pushes along the normals; its `offset=` also shrinks the new cap (negative grows it). `OPEN = None` leaves a side of a `box=` open.

```python
OPEN = None
dog, _ = ops.polyCube(name="Dog", width=14, height=11, depth=20, subdivisionsX=2, subdivisionsZ=3, position=(0, 15, 0))
ops.setSymmetry(dog, axis="x")
ops.createMaterial("DogFur", baseColor=(0.78, 0.55, 0.32), roughness=0.85)
ops.assignMaterial("DogFur", dog)  # faces made later inherit it

# Legs: a bottom corner face, inset so the leg is thinner than the body, extruded down in two segments.
for end in ("+z", "-z"):
    ops.selectComponentsWhere(dog, kind="face", normal=(0, -1, 0), side="+x", extreme=end)
    ops.polyInset(thickness=0.8)
    ops.polyExtrude(thickness=4)
    ops.scaleComponents(by=(0.85, 1, 0.85))  # a pair clear of x=0 scales about each leg's own centre
    ops.polyExtrude(thickness=5.5)

# Neck and head: the body's front faces (the box fences off the legs' front faces), extruded, moved up, widened.
ops.selectComponentsWhere(dog, kind="face", normal=(0, 0, 1), box=[[OPEN, 10, OPEN], [OPEN, OPEN, OPEN]])
ops.polyExtrude(thickness=2); ops.scaleComponents(by=(0.8, 0.8, 1)); ops.moveComponents(by=(0, 5, 0))
ops.polyExtrude(thickness=2); ops.scaleComponents(by=(1.6, 1.5, 1)); ops.moveComponents(by=(0, 3, 0))
ops.polyExtrude(thickness=13)

# A horizontal loop around the head: cut the ring of one vertical edge on its front.
head_z = ops.meshCheck(dog)["box"][1][2]
front = [[OPEN, OPEN, head_z - 0.5], [OPEN, OPEN, OPEN]]
vertical = ops.findComponents(dog, kind="edge", direction=(0, 1, 0), side="+x", box=front)
ops.selectComponents(dog, edges=vertical[:1])
ops.polyInsertLoop(fraction=0.5)

# Snout: the two lower front faces as one region across x=0 (centred), inset, extruded, narrowed.
ops.selectComponentsWhere(dog, kind="face", normal=(0, 0, 1), box=front, extreme="-y", count=2)
ops.polyInset(thickness=1.0); ops.polyExtrude(thickness=4); ops.scaleComponents(by=(0.75, 0.8, 1))

# Eye (paired, touches the seam, so individual=True): inset, squared up, bulged out in two steps, dark material.
ops.selectComponentsWhere(dog, kind="face", normal=(0, 0, 1), side="+x", box=front, extreme="+y")
ops.polyInset(thickness=1.0, individual=True); ops.scaleComponents(by=(0.7, 1, 1))
ops.polyExtrude(thickness=0.8, individual=True)
ops.polyExtrude(thickness=0.6, individual=True); ops.scaleComponents(by=(0.6, 0.6, 1))
ops.createMaterial("DogEye", baseColor=(0.02, 0.02, 0.03), roughness=0.1)
ops.assignMaterial("DogEye")  # no objects: the selected faces

# Floppy ear: the head's upper side face, a narrow root, then segments hanging down and out.
ops.selectComponentsWhere(dog, kind="face", normal=(1, 0, 0), side="+x", box=[[OPEN, 22, head_z - 13.5], [OPEN, OPEN, OPEN]], extreme="+y")
ops.polyInset(thickness=1.0); ops.scaleComponents(by=(1, 0.7, 0.6))
ops.polyExtrude(thickness=1.5)
ops.polyExtrude(thickness=1.0); ops.moveComponents(by=(1, -6, 0)); ops.scaleComponents(by=(0.7, 1, 1.5))
ops.polyExtrude(thickness=1.0); ops.moveComponents(by=(0.5, -6, 0)); ops.scaleComponents(by=(1, 1, 0.7))
print(ops.meshCheck(dog)["issues"])
```

Other parts follow the same pattern: a tail is the body's upper back faces inset as one region and extruded up in two steps; a nose is the snout tip inset, narrowed and extruded a little; paws are one more short leg extrusion scaled up.

## Selecting components (never guess ids)

Ids change after every topology edit, so select by where components are:

- `kind`: "face", "edge" or "vertex". Predicates combine; all must match.
- `normal=(x, y, z)` with `angle` (default 30 degrees): facing that way. Alone it also catches faces elsewhere that face the same way; add `box`, `side` or `extreme`.
- `side`: "+x", "-x", ... relative to the object's origin plane (the symmetry plane); "center" for components on it.
- `box=[[minX, minY, minZ], [maxX, maxY, maxZ]]` in world space, `None` for an open side.
- `extreme="+y"` with `count`: the components furthest that way (the top face, the front-most row).
- `direction=(0, 1, 0)`: edges running that way (vertical edges, whose ring takes a horizontal loop).
- `near=(x, y, z)` with `radius` or `count`; `border=True` for open borders; `shell=i` for one connected part (0 is the largest); `fromSelection=True` narrows the current selection.

`findComponents` returns ids; `selectComponentsWhere` selects them (`add=True` to add) and switches to that component mode. `componentInfo(mesh, kind="face", ids=[...], fields=["center", "normal", "area"])` measures (default: the selected components). `ops.meshCheck(mesh)["box"]` is the quickest way to an extent.

## Symmetry rules

- After an op the selection holds both sides. `moveComponents` mirrors (+x outward on the +x side is outward on both). `scaleComponents` / `rotateComponents` on a pair clear of the plane (legs, ears, eyes) work about each side's own centre; on a region crossing the plane (neck, snout, tail) about its shared centre.
- Paired faces that touch x=0 (the eyes on a 2-wide head) would inset or extrude as one region across the seam: pass `individual=True`.
- `polyInsertLoop` cuts the whole edge ring, which may run through the head, neck and body. Pick an edge whose ring you want and look at the result.
- `ops.undo()` steps back one command.

## Smoothing, sparingly

"Very low poly" means flat-shaded faceted forms: keep the cage, a few hundred faces for a small character, all quads. For a softer look add one level at the end and keep editing the cage: `ops.addModifier(mesh, kind="subdivision", params={"levels": 1})` (exported, about 4x the faces) or `ops.smoothPreview(mesh, level=1)` (display only). Thin parts shrink under subdivision, so make them a bit fuller first. `polyBevel(offset=..., segments=1)` on chosen edges keeps them crisp.

## Checking and fixing

`ops.meshCheck(mesh)` (MCP `mesh_check`) returns `ok`, `issues` with the fix for each, `shells` with each shell's world box, open border edges, non-manifold edges/vertices, counts and symmetry drift. Fix every issue before you finish:

- More than 1 shell: a part was built separately. Undo and extrude it from the body. To join an existing part: `polyCombine(body, part, name=...)` (returns a new name, symmetry off: `rename` and `setSymmetry` again), delete one facing face on each shell (`selectComponentsWhere(kind="face", shell=1, extreme=...)`, `deleteComponents()`), select both open loops (`selectComponentsWhere(kind="edge", border=True)`) and `polyBridge()`. `polyMerge(distance=0.05)` welds only points that already coincide.
- Open borders: `selectComponentsWhere(kind="edge", border=True)` then `polyFillHole()`.
- Non-manifold edges: usually an extrude into existing faces; `ops.undo()` it.
- `ops.symmetryCheck(mesh)` / `ops.symmetrize(mesh)` for drift between the halves.

## Game asset conventions

- Pivot at the base centre for things standing on the ground: model above y=0 (legs end at y=0) and `ops.centerPivot` / `ops.setPivot` if needed.
- Meaningful names for meshes and materials (`Dog`, `DogFur`).
- Keep the user's scene: do not `newScene` unless asked or the scene is empty.
- `ops.saveScene("path.lila")`, `ops.exportFbx(...)` for Unreal, `ops.exportObj(path, mesh)`.
