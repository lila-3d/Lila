---
name: lila-geometry-nodes
description: Build procedural geometry-node groups (modifiers) in Lila through its lila MCP server: scatter, displace, select by box or sphere, store vertex colours, set normals, animate with keyed inputs, nest and share groups. Use when the user asks for a node setup, a procedural effect or VFX mesh, or to rebuild a Blender geometry-nodes graph in Lila.
---

# Geometry nodes in Lila

1. Read the guide first: the `lila://guide/geometry-nodes` resource, or the `read_guide` tool with topic `geometryNodes`. It has the spec format and the full node catalogue (every socket, type, default and option).
2. Write the whole group as one spec and call `gnBuildGroup(spec)` in one `execute_python` call. If it fails, the error lists every problem with the valid names: fix them all and call again.
3. `gnApply(mesh, group)` adds it as a modifier and returns the modifier name. Exposed inputs are keyable attrs on it (`setKeyframe(f"{mod}.input", time=, value=)`).
4. Check with `gnGroupInfo(group)` (compile error, resolved sockets) and a capture.

## Translating a Blender graph

- Same node names without spaces: Delete Geometry is `DeleteGeometry`, Set Position `SetPosition`, Store Named Attribute `StoreNamedAttribute`, Voronoi Texture `VoronoiTexture`, Combine Color `CombineColor`, Set Mesh Normal `SetMeshNormal`, Vector Math `VectorMath`, Map Range `MapRange`.
- Sockets and options are camelCase: `Randomness` is `randomness`, Minkowski is `distance: "minkowski"`, `4D` is `dimensions: "4D"`, F1 is `feature: "f1"`, Vector Math Scale is `operation: "scale"` with input `scale`.
- Units: Lila is cm and Y-up; Blender is metres and Z-up. Multiply lengths by 100 and swap Y and Z (Blender (x, y, z) is Lila (x, z, -y)); divide texture scales by 100.
- A custom group (a red-headed node in Blender) is its own spec in `dependencies`, used through a `Group` node. `BoxSelection` and `SphereSelection` ship with Lila.
- A Color attribute (`Col`) stored on points becomes a vertex colour set FBX exports; Set Mesh Normal Free writes exported custom normals.
