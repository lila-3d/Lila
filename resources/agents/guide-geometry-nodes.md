# Geometry nodes in Lila, for agents

Geometry nodes are node groups that run as a modifier in a mesh's stack, like Blender's. A group's exposed inputs become keyable attrs on its modifier, so they can be keyed, driven and edited in the attribute editor and channel box. Groups can be nested, shared as files and written in one call.

## The fast path: one spec, one call

`gnBuildGroup(spec)` creates a group, or replaces an existing group's whole definition, from compact JSON. Nothing changes unless the whole spec is valid; the error lists every problem at once with the valid socket and property names, so fix them all and call again.

```python
spec = {
    "name": "Ripple",
    "description": "Pushes points along their normal by animated cells.",
    "inputs": [
        {"name": "evolution", "type": "float", "default": 0, "label": "Evolution", "description": "Key this to animate.", "panel": "Noise"},
        {"name": "strength", "type": "float", "default": 5, "softMin": 0, "softMax": 50, "subtype": "distance", "panel": "Noise"},
    ],
    "nodes": [
        {"name": "Cells", "type": "VoronoiTexture", "props": {"dimensions": "4D"}, "values": {"scale": 0.05}},
        {"name": "Mul", "type": "Math", "props": {"operation": "multiply"}},
        {"name": "Normal", "type": "Normal"},
        {"name": "Push", "type": "VectorMath", "props": {"operation": "scale"}},
        {"name": "Move", "type": "SetPosition"},
    ],
    "links": [
        "GroupInput.geometry -> Move.geometry", "GroupInput.evolution -> Cells.w",
        "Cells.distance -> Mul.a", "GroupInput.strength -> Mul.b",
        "Normal.value -> Push.a", "Mul.value -> Push.scale", "Push.vector -> Move.offset",
        "Move.geometry -> GroupOutput.geometry",
    ],
}
ops.gnBuildGroup(spec)
mod = ops.gnApply("pSphere1", "Ripple")          # the modifier's name
ops.setKeyframe(f"{mod}.evolution", time=0, value=0)
ops.setKeyframe(f"{mod}.evolution", time=2, value=3)
```

Spec rules:

- `nodes[].type` is a node type from the catalogue below. `props` are choices (operation, dataType, domain...); `values` are unlinked input values. Props are applied first because they decide which sockets exist.
- `GroupInput` and `GroupOutput` are added when you leave them out. `GroupInput.geometry` is the mesh coming up the stack; every exposed input is `GroupInput.<name>`. `GroupOutput.geometry` is the result.
- Links are `"Node.output -> Node.input"`, or `[from, to]`, or `[fromNode, fromSocket, toNode, toSocket]`. A field input left unlinked with an "implicit" in the catalogue reads that (position, normal, index or id).
- Positions are laid out in columns when omitted.
- Rotations are degrees. Colours are linear `[r, g, b, a]` (or `"#rrggbb"`, sRGB). Units are cm, Y is up.
- `dependencies: [spec, ...]` builds other groups first, so one call can make a group and the groups it nests.
- Re-running `gnBuildGroup` with the same name replaces the definition; modifiers already using it keep their values.

Check the result with `gnGroupInfo(name)` (resolved sockets, values, any compile error) and look at it with `captureView` / `viewport_capture`.

## Interface: what the modifier shows

Each exposed input has: `name` (the attr name), `type` (float, int, bool, vector, rotation, color, string), `default`, `label` (nice name), `description` (tooltip), `subtype` (factor, percentage, angle, distance, color), `min`/`max` (hard limits), `softMin`/`softMax` (slider range), `step`, and `panel` (heading it is grouped under). Inputs show in interface order, grouped by panel. An `angle` float is given in degrees and reaches the graph in radians.

- `gnAddInput(group, name, type, label=..., panel=..., ...)`: a new exposed input, not wired yet.
- `gnExposeInput(group, "Node.socket", name)`: expose a node's input and wire it.
- `gnSetInputInfo(group, name, label=, description=, subtype=, min=, max=, softMin=, softMax=, step=, default=, panel=)`; `""` clears a number.
- `gnRenameInput(group, old, new)`: links follow, modifiers keep the value and keys.
- `gnMoveInput(group, name, index, panel=)`, `gnRemoveInput(group, name)`.

Everything lives in the group definition, so it saves with the scene and travels with exports.

## Nested groups

A `Group` node runs another group: `{"name": "Box", "type": "Group", "group": "BoxSelection", "values": {"size": [50, 50, 50]}}`. Its inputs are the inner group's exposed inputs, its outputs are the inner group's outputs (declare them with `outputs: [{"name": "selection", "type": "bool"}]` in the inner spec and link `X.result -> GroupOutput.selection`). Editing the inner group updates every group that uses it. A group can't contain itself, directly or through another group.

- `gnGroupNodes(group, nodes..., name="Inner")`: collapse nodes into a new group; crossing links become its inputs and outputs.
- `gnUngroup(group, node)`: inline a Group node's nodes.
- `gnExposeOutput(group, "Node.socket", name)`, `gnRemoveOutput(group, name)`.

Library groups that ship with Lila (load with `gnLoadGroup(name)`, or name them in a spec's Group node): `BoxSelection` (center, size -> selection, inverted), `SphereSelection` (center, radius -> selection, inverted, distance). `gnLibrary()` lists every library group.

## Destruction (break apart, simulate, bake)

The fast path is one command: `breakApart(mesh, material="concrete"|"glass"|"wood", impact=[x, y, z] (world cm), pieces=, seed=, blast=, startFrame=, simulate=True, anchor=True)`. It builds a group (Fracture -> Anchor Pieces -> Rigid Body Solver -> Realize Instances) with `pieces`, `seed`, `impact`, `roughness`, `strength`, `blast` and `startFrame` exposed on the modifier, and returns the modifier. Then `exportVertexAnimation(mesh, start=, end=, folder=, mode="rigid")` bakes one position and rotation per piece per frame, or `unrealMap(mesh, kind="vertexAnimation", vat={"mode": "rigid"}, contentPath=...)` and `unrealSync()` send it.

Several meshes in one call (`breakApart("Ground", "WallA", "WallB")`) simulate together on a new `Destruction` mesh (originals hidden; pieces hit each other). `addExplosion(destruction, position=[x, y, z], frame=, blast=, radius=)` adds an explosion; call it once per explosion. `collapse(destruction, part="Ground", start=, duration=, direction=[x, y, z], rumble=, lead=, raggedness=, depth=)` makes a part rumble then fall away in a sweep along direction, lowering the floor by depth.

More outputs: `exportBreaks(mesh, path="x.csv"|"x.json", end=, profile="ue5")` writes each break's frame, time, position and impulse (a .csv imports into Unreal as a Data Table); `wedgeDestruction(mesh, count=, seed=, folder=)` bakes one rigid VAT per seed; `bakePiecesToJoints(mesh, start=, end=)` makes a skinned mesh with a keyed joint per piece for editing and FBX skeletal export; `exportAlembic(mesh, path=, rigidPieces=True)` writes each piece once with a transform per frame.

Building your own graph (needs the Destruction extension, which ships enabled):

- `Fracture` (mesh -> pieces, links): props `material`, `grain` (wood), `linkType`; values `pieces`, `seed`, `impact`, `impactFocus`, `impactRadius`, `roughness`, `roughnessSize`, `chunks`, `interiorMaterial`, `interiorUvScale`, `linkStrength` (N per cm2 of shared area). Needs a closed mesh.
- `MeshToPieces` (mesh -> pieces): one piece per connected part, or per value of an integer attribute (`by: "attribute"`).
- `ConnectPieces` (pieces -> links) for pieces that didn't come from Fracture; `AnchorPieces` (pieces, links -> links) ties pieces reaching the ground to the world.
- `JoinPieces` (pieces, links, pieces2, links2 -> pieces, links): one simulation from two sets, links renumbered. `Explosion` (position, frame, speed, radius -> blast): join several with `JoinGeometry` into the solver's `blasts`. `CollapsePieces` (pieces, links, frame from `SceneTime`, start, duration, direction, rumble, lead, raggedness, seed -> pieces, links): pieces kinematic and shaking until the sweep reaches them, then their links are cut (strength 0) and they fall.
- `RigidBodySolver` (pieces, links, colliders, blasts -> pieces, links, breaks): `startFrame`, `substeps`, `gravity` (cm/s2), `ground`, `groundHeight`, `density`, `friction`, `bounce`, `damping`, `sleep`, `strength` (multiplier), `blastCenter`, `blastSpeed` (cm/s), `blastRadius`, `blastFrame`. It caches per frame through the simulation framework: changing anything upstream re-simulates from the first frame it touches, and results are identical on every run.
- Pieces are instances; the solver reads instance attributes `density`, `friction`, `bounce`, `active` (0 = follows its transform until it turns 1), `guide` (0 to 1, pull toward its own animated transform; `guideStiffness` on the solver), `velocity`, `angularVelocity`. Links are two-point curves with curve attributes `pieceA`, `pieceB` (-1 = world), `type` (0 glue, 1 spring, 2 hard), `strength` (N). `breaks` is a point per broken link with `frame`, `impulse`, `pieceA`, `pieceB`.
- Moving colliders: `ObjectInfo` with `transformSpace: "instance"` into `colliders`.
- One `RigidBodySolver` per group, at the top level of the modifier's group. Keep `RealizeInstances` after it so the modifier outputs a mesh whose points carry `piece`, which the rigid VAT bake reads.

## Colours and normals for export

- `StoreNamedAttribute` with `dataType: "color"` writes a vertex colour set (`color:<name>` on corners) that FBX exports as vertex colour; point and face values spread to the corners. Vectors stored under `Col`, `Cd`, `color:<set>` or an existing set's name become a colour set too.
- `CombineColor` / `SeparateColor` work in rgb, hsv or hsl.
- `SetMeshNormal`: `mode: "free"` writes custom normals per point, face or corner (FBX exports them); `"sharpness"` and `"smoothByAngle"` set hard edges instead. `SetShadeSmooth` after it drops the custom normals.

## Seeing values

A `Viewer` node previews the geometry reaching it, coloured by its `value` field, as a viewport overlay while it is active: `gnSetActiveViewer(group, "View")`, `gnSetActiveViewer(group)` to turn it off. The modifier's result is unchanged.

## Sharing

- `gnExportGroup(group, path)` writes a `.lilanodes` file: `{"format": "lila.nodes", "version": 1, "group": ..., "dependencies": [...]}` with every nested group.
- `gnImportGroup(path)` brings it in (replacing groups the scene already has, modifiers update). Returns the names, main group last.
- `gnSaveGroup` / `gnLoadGroup` / `gnLibrary` use the user and workspace libraries.

## Editing one piece at a time

`gnCreateGroup`, `gnAddNode`, `gnConnect(group, "A.out", "B.in")`, `gnDisconnect`, `gnSetValue(group, "Node.socket", value)` (also props: `gnSetValue(group, "Math.operation", "multiply")`), `gnDeleteNode`, `gnSetDefinition`, `gnApply(mesh, group)`. `gnNodeTypes(detail=True)` returns the catalogue below as JSON.
