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

## Analyse it (for siting a city)

All take `riverArea=` (m2 of drainage that makes a river), `seaLevel=` and `floodReturn='frequent'|'rare'|'extreme'`.

- `terrainField(t, field=, resolution=64)`: a field grid. Fields: slope, aspect, curvature, roughness, flow, flowDir, basinId, streamOrder, wet, hand (height above drainage, cm), floodRisk, distanceToWater, nearestWaterId, waterDepth, rockHardness, soilDepth, excavationCost, slopeClass (0 flat .. 4 unbuildable), buildability, prominence, viewshedScore, solar, windExposure. `terrainSample(..., fields=[...])` reads them at points.
- `terrainWaterInfo(t)`: rivers (per point width, depth, water level, flow speed, discharge, bank height and type, floodplain width; stream order, navigable, tributaries, mouth), lakes and sea (level, shoreline, shore types, harbour sites), springs, wetlands, fords, scored bridge sites, confluences, rapids, waterfalls, and the canals, culverts and bridges you added.
- `terrainProfile(t, path=, step=)`: height, grade, cross-slope and water crossings along a line. `terrainRegionStats(t, polygon=, fields=)`: min/max/mean/percentiles under a footprint (foundation depth = height max - min).
- `terrainBuildable(t, minArea=, maxSlope=, avoidFlood=)`: polygons of buildable land. `terrainSites(t, kind=)`: townCentre, harbour, bridge, ford, mill, castle, temple, farmland, quarry.
- `terrainContours(t, interval=)`, `terrainContourStreets(t, region=, spacing=, maxGrade=)` (contour streets plus cross streets, stairs or switchbacks), `terrainRaycast(t, origin=, direction=)` or `isSolid=[points]`.
- `terrainDirtyRegions(t, since=revision)`: rectangles that changed, to re-plan only those blocks.

## Shape it

- `terrainSculpt(t, op='raise'|'lower'|'smooth'|'flatten'|'noise', points=|path=|curve=, radius=, strength=)`: raise/lower/noise strength is cm; smooth/flatten is 0 to 1. `stage='base'` (default) sculpts before erosion so it erodes naturally; `stage='final'` after erosion. Strokes replay on every evaluation and save with the scene; `clear=True` drops a stage's strokes.
- `terrainStampPad(t, id=, footprint=[...] | object='Mesh or closed curve', mode=, falloff=)`: building pads. Modes: `flatten` (cut and fill to one level), `carve`, `raise`, `blend`, `terraced` (steps down the slope), `plinth` and `stilts` (no grading; read `plinthDepth` and `piles`). `level=mean|min|max|median|object` or `height=` picks the pad height. Returns `padHeight`, `cut`/`fill` (cm3 and m3), `walls` (where a retaining wall is needed), `piles`.
- `terrainStampRegion(t, ...)`: plazas, parks, yards: `grade=` percent toward `direction=`, `edge='slope'|'wall'|'steps'`; `mode='dredge', depth=` lowers a harbour basin.
- `terrainAddFeature(t, kind=, path=|object='Curve', width=)`: `street` (graded to `maxGrade`; returns bridge and tunnel spans and culverts), `canal`/`moat` (carved below their water level), `bridge`, `culvert`, `tunnel`. `terrainStampQuay(t, kind='quay'|'levee'|'embankment'|'seawall', side=, path=)` for waterfronts.
- `terrainCheck(t, buildings=[meshes])`: pads over the cut/fill limit, streets over grade, things in flood without a levee, buildings over water or floating, streets or pads blocking rivers, canals reaching no water. Fix until `ok`.
- Stamps live in layers after erosion (`layer='Stamps'`; a city uses `layer='City'`), so erosion never re-cuts them. Stamping an id again replaces it; `object=` stamps follow that object live. They write `cityFootprint` by default (`masks=['cityFootprint', 'paved']`). `terrainStamps(t)` lists them with their results; `terrainStampRemove(t, ids=[...], layer=)` removes them. Everything undoes.

## Order that works

1. Create or import, then erode.
2. Sculpt big landforms with `stage='base'` (they erode), details with `stage='final'`.
3. Find sites with `terrainSample` / `terrainCostField`, connect them with `terrainRoute`.
4. Stamp the giant structure first (a pad with a wide `falloff`), then the settlement pads in `layer='City'`; check `walls` and `cutM3`.
5. Verify heights with `terrainSample` at the footprints; buildings sit on `padHeight`.

## To Unreal

`terrainSendToUnreal(t)` builds or updates a Landscape: heights on a `Lila` edit layer, rotation and Z-up handled, every mask as a paint layer (weightmap, layer info asset) and a generated material on `MF_LilaLandscapeLayer`. `layers=['cityFootprint', 'flow']` or `{'mask': 'LayerName'}` limits and renames them; `material='/Game/...'` uses your own landscape material (it needs those layer names). `terrainExport(t, target='unreal'|'files')` writes the same files without a live editor.

## Materials, biomes, ground cover

1. `terrainBiomes(t)` after erosion and stamps: masks `temperature` (0 to 1 over -30 to 40 C), `moisture` and `biome_<name>` (snow, alpine, forest, meadow, scrub, wetland by default; weights sum to 1). `climate={'temperature': 8, 'lapseRate': 0.65, 'windAzimuth': 270, 'rainShadow': 0.8, 'seaLevel': 0}` sets the climate; `biomes=[{'name': 'taiga', 'when': {'temperature': [None, 5], 'moisture': [0.4, 1]}, 'paint': 'myMask'}]` your own; `landcover={'mask': 'worldcover'}` follows an imported landcover mask (ESA WorldCover codes by default).
2. `terrainMaterialRules(t)` lays default layers fitted to the terrain (grass base, dirt on wear, gravel on deposit, riverbed on flow, rock over 38 degrees, snow by temperature or the top of the range, sand with `seaLevel=`, road where a road mask exists). Layers go bottom first; the first is the base. `set={'name': 'moss', 'material': 'meadowMoss', 'when': {'moisture': [0.7, None], 'slope': [0, 25]}, 'noise': {'amount': 0.3, 'scale': 2000}}` adds or replaces one, `remove=['moss']` drops one, `layers=[...]` replaces all. `when` inputs: height (cm), slope, aspect (degrees), curvature, nearCliff, temperature (C), moisture, flow, wear, deposit, lakes, wet, any mask, `biome: 'forest'`. Each range is `[min, max]` (None for open) or `{'range': [a, b], 'fade': f, 'invert': True}`. Read `layers[].coverage` (share of the terrain), `maxSumError` (should be ~0) and `missing` (inputs the terrain lacks, read as 0).
3. `terrainMaterialPreview(t)` shows the layers in the viewport (a texture set on the terrain; rerun after changes). `terrainSatmap(t, shade=0.5)` bakes a colour image; `terrainMaskImages(t, masks=[...], layout='uv')` writes masks for texture graphs.
4. `terrainGroundCover(t, items={'rock': 'SM_Rock', 'tree': 'SM_Pine', 'grassTuft': 'SM_Tuft', 'debris': 'SM_Debris'})` scatters rocks on scree, trees in the forest biome, grass on the grass layer and debris below cliffs, live, on `<t>_GroundCover`. Own rules: `rules=[{'name': 'pines', 'items': ['pine'], 'density': 0.02, 'footprint': 250, 'when': {'biome': 'forest', 'slope': [0, 30]}, 'scale': [0.8, 1.3], 'align': 0}]` (density per m2; `layer: 'grass'` in `when` reads a layer; `solid=False` for grass under trees). Missing items get hidden placeholders (`placeholders` says which). `settle=True` drops them with physics; `sendToUnreal=True` or `unrealPlaceInstances('<t>_GroundCover', mode='hism')` ships them.
5. Ship: `terrainSendToUnreal(t, rvt=True)` sends material layers weight-blended as Grass, Rock, ... with `M_<t>_Terrain` on `MF_LilaTerrainLayer` and a runtime virtual texture; `terrainExport(t, target='files')` adds RGBA splats (`files.splats`, bytes summing to 255).
