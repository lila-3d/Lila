# Lila's API for agents

## Ways in

- MCP `execute_python`: Python in Lila's interpreter, state kept between calls. `lila` and `ops` are pre-imported; printed output and a trailing expression's value come back.
- MCP `run_command`: one command by name with JSON args, for clients without Python.
- Headless: `lilabatch --batch script.py` or `lilabatch --batch -c "code"` (on PATH in Lila's terminal). No window, same commands; `captureView` / `captureTurntable` still render.
- Lila's terminal sets `LILA=1`, `LILA_MCP_URL`, `LILA_MCP_TOKEN`, `LILA_WORKSPACE` and `LILA_BIN`.

## Where things live

- `lila.ops.<name>(*objects, flag=value)`: every registered command (Maya style). Positional args are object names; with none, most commands use the selection. Flags take long or short names. `help(ops.polyExtrude)` prints the flags; the MCP `list_commands` tool with `filter` does the same.
- `lila.nodes`: an object API over the scene (`lila.node("Dog").translate_y = 10`, `lila.selected()`).
- `lila.math`: Vector, Rotator, Transform (Unreal-flavoured, cm, Y-up by default).
- `lila.ui`: declarative panels; not needed for modeling.
- Every mutation is a command, so it is undoable (`ops.undo()`, `ops.redo()`) and echoes in Lila's script editor.

## Command families

- Scene: `ls`, `select`, `delete`, `rename`, `parent`, `group`, `duplicate`, `getAttr`, `setAttr`, `xform`, `move`, `rotate`, `scale`, `newScene`, `openScene`, `saveScene`.
- Primitives: `polyCube`, `polySphere`, `polyQuadSphere`, `polyCylinder`, `polyCone`, `polyTorus`, `polyPlane`, `polyDisc`, `polyPipe` (each returns `[mesh, generator]`). `fraction` (0 to 1, default 1) keeps part of one: the top of a sphere, quad sphere or ico sphere (0.5 is a dome open at the bottom), or that much of a turn of a cylinder, cone, torus, pipe or disc (`cutCaps` closes the cut ends). `position=` with `normal=` stands one on a surface: up axis along the normal, a dome by its open base. Quad sphere `subdivisions` defaults to 3.
- Components: `selectMode`, `selectComponents` (by id), `selectComponentsWhere` / `findComponents` (by position, normal, side, box, shell), `componentInfo` (bulk positions/normals/adjacency, `fields=` to trim), `componentSelection`, `convertSelection`, `growSelection`, `shrinkSelection`, `selectLoop`, `selectBorder`, `selectByAngle`.
- Mesh edits (on the component selection): `polyExtrude`, `polyInset`, `polyBevel`, `polyInsertLoop`, `polyConnect`, `polyBridge`, `polyMerge`, `polyMergeCenter`, `polyCollapse`, `polyDissolve`, `polyFillHole`, `polyPoke`, `polySpinEdge`, `polyFlip`, `deleteComponents`, `moveComponents`, `rotateComponents`, `scaleComponents`.
- Whole meshes: `polySubdivide`, `polyMirror`, `polyCombine`, `polySeparate`, `polyBoolean`, `polyTriangulate`, `polyQuadrangulate`, `polyReduce`, `relaxMesh`, `quadRemesh`, `meshCheck` (pass/fail: shells, holes, non-manifold, with fixes), `meshInfo`, `meshData`, `createMesh`.
- Symmetry: `setSymmetry`, `symmetryCheck`, `symmetrize`, `symmetryRebuild`.
- Modifiers: `addModifier` (mirror, subdivision, bevel, array, triangulate, weightedNormals, solidify, decimate), `modifierStack`, `applyModifier`, `removeModifier`, `smoothPreview`.
- Deformers (in the same stack, each with `envelope` and a per-point map via `setDeformerWeights`): `morph(targets..., base)` + `addMorphTarget`, `morphTargets`, `renameMorphTarget`, `addInBetween`, `mirrorMorphTarget`, `editMorphTarget`, `extractMorphTarget`, `setMorphTarget` (morph targets; set `<target>.weight`, exported as Unreal morph targets in skeletal FBX), `sculptCorrective` + `addCorrective` (pose-space correctives driven by `poseDriver`), `lattice` (edit the lattice's points in vertex mode), `nonlinear(type=bend|twist|taper|squash|sine|wave)` (returns `[handle, deformers...]`), `softMod`, `deltaMush`, `wrap(meshes..., driver)`, `cage(meshes..., cage)`, `rebindDeformer`, `deformers`.
- Drivers: `poseDriver(joint)` (1 at the joint's pose now, 0 at rest), `driveChannel(driven, driver=, keys=[[in, out], ...])` (set driven key on any Float plug, material params included; `curveName=` bakes an Unreal curve on export), `setDrivenKey`, `editDrivenChannel`, `drivenChannels`, `removeDrivenChannel`, `exportDriverData(path)`.
- Geometry nodes (procedural modifiers, read the `lila://guide/geometry-nodes` resource or `read_guide` topic `geometryNodes` first): `gnBuildGroup(spec)` builds or replaces a whole group from one JSON spec with every validation error listed, `gnApply(mesh, group)`, `gnGroupInfo`, `gnNodeTypes(detail=True)`, interface `gnAddInput` / `gnSetInputInfo` / `gnRenameInput` / `gnMoveInput`, nesting `gnGroupNodes` / `gnUngroup`, sharing `gnExportGroup` / `gnImportGroup` (.lilanodes), preview `gnSetActiveViewer`.
- Materials: `createMaterial`, `assignMaterial`, `listMaterials`, `materialInfo`.
- UVs: `uvAuto`, `uvUnwrap`, `uvProject`, `uvPack`, ...
- Looking: `captureView` / `captureTurntable` (PNG files), and the MCP tools `viewport_capture` / `capture_turntable` (images returned to you), `scene_summary`, `mesh_check` (compact validation), `mesh_info`.
- Seeing the UI itself (menus, panels, HUD, gizmos): `ops.screenshot(path=r"C:/tmp/ui.png", target="window")` writes a PNG of the window as shown; `target="viewport"` crops to the active view, or name a panel (`"outliner"`, `"channelBox"`). The MCP tool `capture_window` (same `target`) does it over MCP. Needs the window: in batch mode use `captureView`.
- Export: `exportFbx`, `exportObj`, `unrealSync` (Unreal link).
