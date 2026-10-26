# Tree Generator

Grows a trunk with recursive branches, drops canopy blobs on the branch tips and scatters a leaf mesh over them, bending every copy onto the canopy so the shell reads as one mass. Exports the high and low poly pair and the json a texturing stack is built from.

## Get it working

1. Create > Tree Generator, first item, opens the panel.
2. Leaf Plane section: **Make Leaf Plane**. It lands in the Leaf Mesh field. A modelled leaf or leaf cluster works too: select it and click **Set** next to Leaf Mesh(es).
3. Pick a preset and click **Build Whole Tree**.

From Python or MCP the same thing is:

```python
leaf = treeLeafPlane()
tree = treeBuild(preset="Kaede Mature", leaf=leaf)
treeExport(tree, settings={"export.path": "D:/Bakes/Maple", "export.name": "Maple"})
```

Every button is an action under the Tree Generator category, so any of them can go on a hotkey, shelf or marking menu.

## Export for baking

Set **Export Path** and **File Name** in the Export section, select the tree and click **Export FBX Pair**:

| File | Contents |
| --- | --- |
| `<name>_Lo.fbx` | Trunk and leaves at their lowest detail, world space |
| `<name>_Hi.fbx` | Trunk and leaves at their finest detail, subdivided by the High Poly Divisions, merged into one mesh |
| `<name>_tree.json` | Preset, species, colour set name, material names and face counts per part |
| `<name>_palette.json` | Leaf and bark tones per season. Written once, never overwritten, so hand edits survive |

The part materials (Trunk, Canopy, Leaves by default) are assigned for real, since the baker names its texture sets after them.

**Export Single Leaf** writes the leaf mesh on its own as `<name>_Leaf_Lo.fbx` and `<name>_Leaf_Hi.fbx` (the densest leaf mesh, subdivided), with `<name>_Leaf_tree.json` and `<name>_Leaf_opacity.png`, for particle cards.

**Measure Bake Distances** logs how far the high poly stands off the low poly per part. Use the 99% numbers for the baker's ray lengths.

The leaves carry the `leafShade` colour set: R how exposed the leaf is (up facing and proud is light), G a value per leaf, B a value per bunch. Bake it to a map named `<name>_leafshade.png` (or `_vertexcolor`) in the export folder, then **Split Vertex Colour Masks** writes `<name>_maskR.png`, `_maskG.png` and `_maskB.png`.

**Write Opacity Map** rasterises the leaf mesh's UV shells into `<name>_opacity.png`, a cutout for leaves modelled to shape.

## Troubleshoot

| Message | Fix |
| --- | --- |
| Select the leaf mesh, or pass leaf= | Fill Leaf Mesh(es), or select the leaf before Build Whole Tree |
| Grow a trunk first, or select one built by this tool | Blobs At Branch Tips needs a trunk the tool grew, selected or just grown |
| No sample points survived the filters | Lower Min Distance, raise Min Normal Up toward -1, or raise Clump Radius |
| Blob Clip removed the whole blob | Lower Blob Clip Bottom below 0.95 |
| ... belongs to a generated tree | Export Single Leaf wants the source leaf, not the scattered leaves |
| Set an export path and a file name first | Fill both in the Export section |
| The export has no canopy | Turn on Keep Blobs and Canopy In High Poly / Low Poly |

Leaves bend along the leaf's longest axis and lie on its thinnest. A leaf modelled diagonally confuses that: set **Leaf Up Axis** and **Leaf Forward Axis** by hand.

Lengths in the panel are centimetres. Presets and stored settings hold metres.

## Tune it

- **Presets**: Sugi (cedar), Akamatsu (red pine) and Kaede (maple), each Young, Mature and Ancient. Save As stores your own in the Lila preferences (`treeGenerator.json`). Reapply keeps the up axis, stylization and export block unless the preset sets them.
- **Stylization** fattens the trunk and leaves for a chunkier tree off the same preset, thinning the leaf count to keep coverage.
- **Seeds**: the same trunk and leaf seeds always rebuild the same tree. Randomize Seed rolls both.
- **Trunk**: Length Falloff shortens branches higher up their parent, turning a round crown into a cone. UV Mode packed gives every limb its own island at one texel density for baking; tiled repeats a bark texture every UV Tile Size.
- **Canopy Blobs**: Taper sizes blobs by their branch's thickness, Align swings them toward their branch, Offset slides them along their axis in multiples of their radius, Clip Bottom cuts them into domes.
- **Leaves**: Wrap bends leaves onto the canopy curvature, Curl rolls them along their length, Gather pulls them toward the branch tips, Align swings them along the branch (needle tufts), Clumps breaks the scatter into bunches with bare canopy between. Canopy Normals blends the leaf normals toward the canopy's for soft shading. Several leaf meshes in Leaf Mesh(es), densest first, build detail levels placed vertex for vertex on top of each other.
- **Step by step**: Grow Trunk, Blobs At Branch Tips, then Scatter Leaves with your own canopy meshes in Canopy Mesh(es). The parts link back to the trunk, so Load From Selection, Rebuild and Export work from any of them.
- **Bake handoff**: `treeBakeHandoff(tree, settings=...)` returns the pair, json paths, materials, face counts and ray distances a bake project needs, without writing anything.
