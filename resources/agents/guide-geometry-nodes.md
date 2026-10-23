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
