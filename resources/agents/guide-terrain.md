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

## From a real place, a painting or drawn lines

- Real place: `terrainImportDem(place='Matterhorn', size=[1200000, 1200000])` or `bbox=[west, south, east, north]` in degrees. `source='copernicus'` (GLO-30, default), `'terrarium'` (AWS Terrain Tiles, `spacing=` cm picks the detail) or `'opentopography'` (`dataset='SRTMGL1'`, `apiKey=`, `saveKey=True` once). A local DEM works too: `terrainImportDem('dem/N46E007.hgt')`. `projection='utm'` (default), `'enu'` or `'local'`. Copernicus includes trees and roofs: `bareEarth=True`. Coarse 30 m data: `amplify=4` adds eroded detail at 7.5 m and keeps the place.
- The result's `georef` and `terrainGeoref(t, latLon=[[lat, lon]])` give world points for real coordinates; `points=[[x, z]]` gives latitude and longitude back.
- Attribution for every source goes to the workspace's `terrain/ATTRIBUTION.txt`; ship it with the game. Read `attribution` and `notes` in the result (open-sea tiles at sea level, tile zoom used).
- OSM: `terrainImportOsm('area.osm', terrain=t)` (or `.json` from Overpass, `.osm.pbf`, or `fetch=True`). Roads, paths, rivers, streams, lakes and coastline come in as curves draped on the ground, one curve per kind under `<terrain>_OSM` (`perFeature=True` for one per feature); `osmFeatures` on each curve lists id, name, class and width (cm) per spline. Land cover becomes masks `landcover_forest`, `landcover_urban`, `landcover_water` ... Terrains not from real data need `origin=[lat, lon]` of their centre.
- Painted sketch: `terrainImport('sketch.png', mode='sketch', size=400000, high=60000)`. Grey is the base height; red strokes ridges, green valleys, blue rivers, yellow flat ground. Erosion keeps them. 8-bit paintings are de-stepped in every mode.
- Drawn constraints: `terrainConstrain(t, kind='ridge'|'valley'|'cliff'|'river'|'flat', curve='Line' or path=[[x, z], ...], height=, width=)`. They are held while erosion runs. Draw rivers downstream: the bed only falls along the curve. Same `id` replaces; `remove=id`, `clear=True`.
- Live link: `terrainLiveLink(t)` re-runs the terrain whenever its file is saved (in the app). Headless, call `terrainReload(t)` after writing the file.
