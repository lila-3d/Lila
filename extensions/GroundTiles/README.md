# Ground Tiles

Scatters source meshes into a seamlessly tiling high poly patch, ready to bake down to tiling textures for Unreal landscapes and surfaces. Painter's `terrain_stack.py` plugin builds the layer stack from what the export writes.

## Get it working

1. Open **Windows > Ground Tiles** (also under **Create > Ground Tiles**).
2. Select one or more meshes and click **Add Selected** under Source Meshes. Put each mesh's pivot where it meets the ground and model it growing away from the pivot.
3. Pick a preset, adjust anything, click **Generate**. The result is a `<Preset>_GroundTile_GRP` group holding the scatter.
4. Click **Create Bake Plane**, then set **Export Path** and **File Name** in the Export section and click **Export FBX Pair**.
5. Click **Bake ID Map** for `<name>_id.png` and **Measure Height Range** for the height bake numbers.

From Python or MCP:

```py
blade = polyCube(width=2, height=10, depth=0.5, subdivisionsY=8, baseAtOrigin=True)[0]
r = groundTileGenerate(preset="Dry Grass", sources=[{"name": blade, "weight": 1.0, "objectId": 0}], values={"tileSize": 200})
groundTileCreateBakePlane(r["group"])
groundTileExport(r["group"], path="D:/Tiles", name="DryGrass", material="DryGrass")
groundTileBakeIdMap(r["group"], path="D:/Tiles/DryGrass_id.png")
groundTileMeasureHeight(r["group"])
```

Everything that changes the scene is undoable. Selecting a generated group (or anything in it) loads its settings back into the panel.

What the export writes:

| File | Contents |
| --- | --- |
| `<name>_Hi.fbx` | The group without the bake plane, world space, with ID materials and the `terrainMasks` colour set |
| `<name>_Lo.fbx` | The bake plane on its own, its material named **Bake Plane Material** for the export |
| `<name>_ids.json` | Object IDs, their colours and sources, the tile size and height range |
| `<name>_colours.json` | The layer palette per ID, written once and never touched again, so hand edits survive |
| `<name>_id.png` | **Bake ID Map**: the exact ID colours, no filtering, black where nothing covers the tile |
| `<name>_maskR/G/B.png` | **Split Vertex Colour Masks** on a baked `<name>_vertexcolor` map |

`terrainMasks` channels: R is 0 at the root to 1 at the tip along each instance, G a random constant per instance, B the height layer (0, 0.5, 1).

## Troubleshoot

| Symptom | Fix |
| --- | --- |
| `Skipping source '...'` | The source was renamed or deleted. Right click it, or remove and add it again. |
| `Only N of M points fit` | Poisson can't place that many at that spacing. Lower **Min Spacing** or **Count**. |
| Clustered looks uniform | The readout under Distribution says the discs blanket the tile. Aim for 0.1 to 0.3 coverage: fewer or smaller clusters, Cluster Strength high. |
| Instances bend across the blade, not along it | The source's geometry runs diagonally in its own space. Right click the source and set **Length Axis** and **Base End**, or reorient the geometry along one axis. |
| `Shared meshes are unavailable...` | **Use Instances** only works without deformation, vertex colours and shear. |
| Measure Height Range refuses | It needs **Combine To Single Mesh** on and a bake plane in the group. |
| Export aborts on a name clash | Something is already named like **Bake Plane Material**. Rename it or pick another name. |
| ID map is all black | The group has no bake plane under the geometry, or the plane is not inside the group. |
| ID map colours look wrong in Painter | Load it as raw data, not sRGB, and pick IDs with a colour picker. |
| Visible seam in the bake | Turn UV padding and edge dilation off in the baker; the wrapped copies supply those pixels. |

The panel only shows what the extension's commands do; every message also lands in the script editor.

## Tune it

- **Presets** cover parameters only, never sources. The disc icon saves the current settings as a user preset in the Lila prefs (`groundTiles.json`). The star makes the current preset the one the panel opens with.
- **Distribution**: Uniform, Clustered (tufts, moss), Stratified (even coverage), Poisson (no interpenetration, spacing measured across the seam), Rows (raked gravel, whole sine cycles keep it seamless).
- **Wrap Edges** duplicates anything overhanging an edge onto the opposite edge. Both copies come from one seed, so their geometry matches exactly.
- **Rotation**: Random, Range, Snapped (with jitter) or Aligned. Max Tilt 180 gives fully random orientation.
- **Height Layers**: three bands with their own share, height and scale. The bake plane sits at the lowest band anything lands in.
- **Deformation**: bend, flare, twist and sine (angles in degrees, from the base), lattice, vertex noise, shear, flatten, per-axis scale variation. A nonlinear deformer already on a source is reused for its placement and bounds.
- **Source rows**: weight, Object ID, up axis offset, ID colour swatch. Sources sharing an ID bake as one colour. Right click for Length Axis, Base End and Select In Scene.
- **Output**: Snap Base To Height, Vertex Colours, Ground Plane (its own Object ID, offset and subdivisions), ID Materials, Bake Map Size, Use Instances, Combine To Single Mesh.
- **File Name** and **Bake Plane Material** follow the preset (Reeds & River Grass gives `ReedsRiverGrass`) until you type over them. Clear a field to follow the preset again.
- The group's channel box shows `groundTiles_minHeight`, `maxHeight`, `heightRange`, `bakePlaneHeight`, `heightAbovePlane`, and after a measurement `heightP50`, `heightP99`, `heightOverTile`. Scale a height bake by p99, not by heightAbovePlane.
- `groundTileBakeData` returns the high/low pair, sidecars, bake plane, height range and ID palette for a bake project.
