# Terrain in Lila, for agents

A terrain is a mesh whose geometry-nodes modifier builds a heightfield. The Terrain extension (on by default) adds the commands below. Lengths and heights are cm, in world space. Points are `[x, z]` on the ground (`[x, y]` on a Z-up scene) or `[x, y, z]`. Every read returns the terrain's `revision`: equal revisions mean the same ground, so cache by it.

## Make one

- `terrainCreate(size=[w, d], resolution=1025, amplitude=, featureSize=, ridged=, seed=)`: noise terrain. `flat=True` for level ground. Its inputs become keyable attrs on the modifier: `setAttr('<modifier>.amplitude', 30000)`.
- `terrainImport(file)`: GeoTIFF / SRTM `.hgt` / `.asc` / `.xyz` / Terrarium at real size, or a painted image (`mode='guide'` keeps its big shapes and erodes in detail, `height`, `uplift`, `mask`).
- `terrainErode(t, iterations=80)`: rivers carve drainage-consistent valleys and write masks `flow`, `wear`, `deposit`, `lakes`. `device='gpu'` in the app is much faster at 4K; `cpu` (the default) is exact everywhere.
- Big terrains: build at 513 to iterate, raise `resolution` at the end. `terrainBake(t)` collapses the graph to a `.lilahf` (stored relative to the scene) so it stops re-evaluating.

## Read it

- `terrainInfo(t)`: resolution, cell size, `min`/`max`/`center` on the ground plane, `heightRange`, each mask's min/max/mean.
- `terrainSample(t, points=[...], masks=['flow'])`: height, position, normal, slope (deg) per point. `path=[...]` with `step=` gives a profile with `distance` and `grade` (%).
- `terrainCostField(t, weights={'slope': 1, 'water': 20, 'cityFootprint': 5}, resolution=64)`: cost grid for planning.
- `terrainRoute(t, start=, end=, maxGrade=8, weights=)`: cheapest path that never exceeds the grade (switchbacks appear on steep ground); `withinGrade` false means it had to. `curve=True` makes a curve along it.

## Shape it

- `terrainSculpt(t, op='raise'|'lower'|'smooth'|'flatten'|'noise', points=|path=|curve=, radius=, strength=)`: raise/lower/noise strength is cm; smooth/flatten is 0 to 1. `stage='base'` (default) sculpts before erosion so it erodes naturally; `stage='final'` after erosion. Strokes replay on every evaluation and save with the scene; `clear=True` drops a stage's strokes.
- `terrainStampPad(t, id=, footprint=[...] | object='Mesh or closed curve', mode=, falloff=)`: building pads. Modes: `flatten` (cut and fill to one level), `carve`, `raise`, `blend`, `terraced` (steps down the slope), `plinth` and `stilts` (no grading; read `plinthDepth` and `piles`). `level=mean|min|max|median|object` or `height=` picks the pad height. Returns `padHeight`, `cut`/`fill` (cm3 and m3), `walls` (where a retaining wall is needed), `piles`.
- `terrainStampRegion(t, ...)`: plazas, parks, yards: `grade=` percent toward `direction=`, `edge='slope'|'wall'|'steps'`.
- Stamps live in layers after erosion (`layer='Stamps'`; a city uses `layer='City'`), so erosion never re-cuts them. Stamping an id again replaces it; `object=` stamps follow that object live. They write `cityFootprint` by default (`masks=['cityFootprint', 'paved']`). `terrainStamps(t)` lists them with their results; `terrainStampRemove(t, ids=[...], layer=)` removes them. Everything undoes.

## Order that works

1. Create or import, then erode.
2. Sculpt big landforms with `stage='base'` (they erode), details with `stage='final'`.
3. Find sites with `terrainSample` / `terrainCostField`, connect them with `terrainRoute`.
4. Stamp the giant structure first (a pad with a wide `falloff`), then the settlement pads in `layer='City'`; check `walls` and `cutM3`.
5. Verify heights with `terrainSample` at the footprints; buildings sit on `padHeight`.

## To Unreal

`terrainSendToUnreal(t)` builds or updates a Landscape: heights on a `Lila` edit layer, rotation and Z-up handled, every mask as a paint layer (weightmap, layer info asset) and a generated material on `MF_LilaLandscapeLayer`. `layers=['cityFootprint', 'flow']` or `{'mask': 'LayerName'}` limits and renames them; `material='/Game/...'` uses your own landscape material (it needs those layer names). `terrainExport(t, target='unreal'|'files')` writes the same files without a live editor.
