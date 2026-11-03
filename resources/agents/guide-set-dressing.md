# Set dressing in Lila, for agents

Stagehand (an extension that ships enabled) dresses scenes with props: from the library, arranged, settled with physics, checked. Every call is deterministic: the same scene and seed give the same result. Lengths are cm, Y up.

## The loop

1. `librarySearch("words", tags=[...], maxSize=cm)` to pick assets by id. Results carry `size` [x, y, z], `tags` and `physics`.
2. Place or arrange (below).
3. `dressCheck([meshes])`: `issues` lists `floating`, `overlapping` and `unstable` (would move if physics ran) per mesh; `ok` when clean.
4. `capture_view` the area to look at it. Fix and repeat until `ok`.

## Placing single things

- `place(assetId, at=[x, y, z])` stands it on the first surface below that point; `on="Desk"` puts it on top of a mesh (with `at=` for where on it). `rotate=` degrees about up, `scale=`, `count=` with `spacing=[x, y, z]` and `jitter=` degrees, `drop=True` to let physics settle it. Returns each placed root and its meshes.
- Placements are instances of a hidden master under `StagehandLibrary`; `mode="copy"` for independent copies. Never edit or delete `StagehandLibrary` itself.
- `replaceWith(objects, asset=, fit=True)` swaps blockout boxes for an asset, keeping position and heading.
- `dropToSurface(objects)` lands objects on what is below without physics; `alignObjects`, `distribute`, `randomizeTransforms(rotate=[0, 25, 0], seed=)` for layout.

## Physics

- `settle(meshes)`: fall and rest. `push(mesh, direction=[x, y, z], speed=150)`: shove, and whatever touches it reacts. `wiggle(meshes, strength=40)`: jostle into a natural rest. `drop(meshes, onto=, at=, spin=)`. `shake("Table")`: what rests on it shuffles.
- Each returns per mover `moved`, `turned`, `atRest`, `penetration` and `restsOn` (what holds it up), so you can tell what happened without a capture.
- What moves: the meshes you name, plus (for push and shake) meshes touching them that are no more than 8 times larger; pass `movable=[]` to move only the target. `physicsBody(meshes, role="static"|"dynamic"|"off", material="wood"|"book"|"stone"|...)` fixes roles for good.
- Runs go until everything rests (`time=` caps it, 120 s; the report says `capped` if it hit the cap). Only meshes within `reach=` (300 cm) of a mover collide, so a city-sized scene stays fast; terrains collide as heightfields. Concave props (arches, chairs, I-beams) collide as convex parts: `physicsBody(m, shape="decompose")`, which `auto` picks for hollow or branching meshes.
- Debris from Fracture or any geometry-nodes instances: `r = instancesToObjects("Wall", material="stone")` then `settle(r["objects"])`. `mode="unique"` gives each object its own mesh.
- Starter props ship built in: `librarySearch(tags=["debris"])`, `Industrial/...`, `Debris/StoneChunkS1`..`XL3`, `Facility/...`; `libraryStarter()` lists their ids.
- To Unreal: `unrealPlaceInstances` sends bodies as `UCX_` collision plus each actor's simulate, mass and physical material. `assemblyCreate(objects, name=)` then `unrealPlaceAssembly(name)` sends a group as one Level Instance (`kind="packedLevelActor"` packs it); `dryRun=True` shows the request. `libraryAdd(assembly)` saves it as a library asset of its member ids (nesting allowed); `place` puts it back whole.

## Arrangements

- `fillShelf(boards, items=[asset ids or meshes], seed=, leanChance=, lyingChance=, gap=, sag=True)`: rows along each board's top.
- `scatterProps(surface, items=, density=per m2, slopeMax=, mask=attribute, settle=True)`, `pileProps(items=, at=, count=, radius=, onto=)`, `stackProps(items=, at=, count=, onto=)`.
- Default is live: a geometry-nodes group on a new mesh, with its inputs on the modifier; change `seed` with setAttr to re-arrange. Use `realize=True` when you will push, check or edit individual items: you get real objects back in `nodes`.
- `variants="pool"` (default) gives a few bowed variants per item, cheap in Unreal; `unique` one each; `none`.

## Modular kits

- `addSnapPoint(piece, type="wallEnd", position=[local], normal=[local, out of the piece])`, `snapTo(piece, target=)`, `snapAll(pieces, tolerance=25)`, `openSnapPoints()` for gaps. Points join when types match (or `accepts=[...]` lists the other's type).

## Saving to the library

`libraryAdd(objects, name=, collection="Props/Kitchen", tags=[...], material=, role=, tier="workspace")` writes the model as .glb with its snap points and physics, so the next `place` finds it.
