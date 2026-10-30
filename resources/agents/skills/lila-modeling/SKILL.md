---
name: lila-modeling
description: Model 3D assets in Lila (the Maya-style 3D app this workspace uses) through its lila MCP server. Use whenever the user asks to model, sculpt, block out, build or change a 3D object, character, prop or mesh, or asks what something looks like in the viewport. Use Lila's tools, never Blender or Maya.
---

# Modeling in Lila

Lila is running and exposes MCP tools under the `lila` server. Do every 3D step there.

## Before you start

1. Read the guide: the `lila://guide/modeling` resource, or the `read_guide` tool with topic `modeling`. It has the workflow and copyable recipes (legs, head, snout, eyes, floppy ears, tail).
2. `scene_summary` to see what exists. Leave the user's objects alone unless asked.
3. Units are cm, Y is up, the asset's front faces +Z.

## Workflow

1. One `polyCube` split on x=0 (`subdivisionsX=2`, rows along its length for attachment points), `setSymmetry(mesh, axis="x")`.
2. Grow every part out of that mesh's faces: `selectComponentsWhere(mesh, kind="face", normal=..., side="+x", box=..., extreme=...)`, then `polyInset`, `polyExtrude`, `moveComponents`, `scaleComponents`. Each op leaves its result selected, so transforms chain.
3. One `execute_python` call per stage (blockout, features, finish), many commands each, printing one line.
4. After each stage: `mesh_check` (must stay 1 shell) and `capture_turntable` with `views=["front", "right", "top", "persp"]`, `display="shaded"`. Fix what looks wrong before moving on.
5. Finish: materials (`createMaterial`, `assignMaterial`), a meaningful name, `mesh_check` OK, a final capture.

## Pitfalls

- A part made as its own primitive is a floating shell: `mesh_check` fails it. Extrude it from the body instead (the guide has the join recipe if one exists already).
- Never guess component ids; they change after every topology edit.
- With symmetry, paired faces touching x=0 (eyes) need `individual=True`; centred features (snout, tail) are one region across the seam.
- `polyInsertLoop` cuts the entire edge ring.
- "Very low poly" means flat facets and a few hundred quads; add one subdivision level only for a softer look.
