# Terrain Tools

Varies a mesh you have already built, so one modelled shrub gives blades of different thickness and lean rather than a ring of identical ones.

## Get it working

1. Open **Windows > Terrain Tools**, or **Mesh > Terrain Tools** for the same commands as menu items.
2. Select the faces to grow blades from and click **Extrude Blades**. Thickness, Offset, Divisions and Keep Faces Together under it set the new blades.
3. Select the mesh, tick the values to vary, set their ranges and click **Randomize Extrude**. Click it again to re-roll.
4. Select the blade faces (**Select Stems** does it), pick a deformer **Type** and click **Add Deformer**.

From Python or MCP the same steps are `bladeExtrude`, `randomizeBladeExtrude` and `addBladeDeformer`:

```py
plane = polyPlane(width=10, depth=10, subdivisionsX=4, subdivisionsZ=4)[0]
bladeExtrude(plane, faces=list(range(8)), thickness=3.0, offset=0.2, divisions=2)
randomizeBladeExtrude(plane, fields={"thickness": [1.0, 5.0]}, seed=7)
selectBladeFaces(plane, stems=True)
addBladeDeformer(kind="bend", low=20, high=60, seed=5)
```

Everything is undoable.

## Troubleshoot

| Message | Fix |
| --- | --- |
| `... has no blade extrude. Extrude Blades first.` | Randomize, Read Selection, Reset and the Select buttons only work on blades made with **Extrude Blades**. A plain extrude keeps nothing to vary. |
| `Turn on something to randomize.` | Tick at least one of Thickness, Offset, Local Translate Z. |
| `Select a mesh, or the faces to deform.` | Nothing usable is selected. A whole mesh counts as every face. |
| `No bend deformers on the selection.` | Randomize and Delete only find deformers of the current **Type**. Select the mesh, a component or the handle. |
| Every run gives a different result | Seed is 0. Set any other number to repeat a result. |
| A deformer bends the whole blade from the middle | **From Base** is off, which centres the handle. |
| Blades lean the same way | Turn on **Random Direction**. |
| Bias does nothing | The blades' roots all sit at one height along the up axis, so there is no top to bias toward. Check **Up Axis**. |

Randomizing rebuilds each blade from its root, so it never stacks on the last run, and editing the blade points by hand is lost the next time you randomize.

## Tune it

**Up Axis** and **Seed** apply to both sections. Auto follows the scene's up axis.

### Extrude

| Field | Is |
| --- | --- |
| Thickness | How far the blade grows along its face normal |
| Offset | How much the blade narrows as it grows |
| Local Translate Z | Extra growth along the same direction as thickness |
| Bias To Top | 1 gives the high end of the range to blades near the top, -1 to blades near the bottom, 0 is pure random |
| Keep Faces Together | Connected faces grow as one blade; off, every face is its own |

Every blade keeps its own values on the mesh, and the values it was extruded with. **Read Selection** fills the ranges with give or take 30% of what the selected blades use now. **Reset** puts every blade back to its extruded values. `setBladeExtrude(mesh, blades=[3], thickness=2.5)` sets one blade exactly, and `bladeExtrudeInfo(mesh)` lists them.

| Button | Selects |
| --- | --- |
| Select Extruded Faces | The faces the blades grew from, which are their caps now |
| Select Stems | Every face a blade made, caps and walls, down to the ring it stands on |

### Deformers

Bend, Flare, Twist, Sine, Squash and Wave, each with its own amount range remembered separately. Flare is a taper deformer; a bend's Curvature is degrees per handle unit, so the deformer's angle is curvature times the length of its bounds.

| Option | Does |
| --- | --- |
| One Per Blade | A deformer per blade rather than one for the whole selection |
| From Base | Anchors the handle at the blade's root, bounds 0 to 2, so it deforms root to tip. Off centres it, bounds -1 to 1 |
| Random Direction | Spins each handle around the up axis |
| Bias To Top | As above, over the amount range |

Only the points a blade made are weighted, so a bend from the base leaves the roots planted and stretches the blade above them. One Per Blade finds blades from the extrude, so blades grown off neighbouring faces come apart even though they share edges; without blades it falls back to connected faces.

The panel's values are stored under `Tools` in `terrainGenerator.json` in the preferences folder, shared with Terrain Generator. Delete that key to reset them.
