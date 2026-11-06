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

## To other engines and as meshes

- `terrainExport(t, target='unity', folder=)`: edge-matched 16-bit RAW tiles at 2^n+1 (`resolution=` 33 to 4097, or picked; bigger terrains become more tiles under `maxResolution`), 8-bit splatmaps (`layers=` picks masks; `baseLayer` takes the weight left below 1), `<name>.json` and `LilaTerrainImporter.cs`. In Unity: put the script in an Editor folder, Tools > Lila > Import Terrain, pick the json (headless: `-executeMethod LilaTerrainImporter.ImportFromCommandLine -lilaTerrain <json>`).
- `terrainExport(t, target='godot')`: Terrain3D regions on its grid (`regionSize=` 64 to 2048, or picked to fit its 32 x 32 map), EXR heights in metres, layer weights as splat PNGs, and `<name>_terrain3d.gd`: open the scene, run the script (File > Run) and it imports the heights and writes the control map (heaviest layer base, next overlay). `mode='mesh'` writes glTF tiles instead, collision named `_colonly` for Godot's importer.
- `terrainExport(t, target='mesh', format='fbx'|'glb'|'usd'|'fbx,glb', tileSize=257, lods=3, lodRatio=0.25, skirt=0)`: one file per tile (`perTile=False` for one file), each an LOD group plus a `_collision` mesh; every LOD keeps its border vertices, so tiles never crack. Returns `trianglesPerLod` and `maxErrorPerLod` (cm). `terrainTiles(t, ...)` makes the same tiles in the scene (undoable) for use in Lila.
- `terrainExport(t, target='files', tileSize=1024)`: `_x<N>_y<M>` tiles (EXR cm, 16-bit PNG and R16 over one shared height range, a json sidecar each); `sharedEdges=True` repeats the edge row; `splats=True` packs masks four to an RGBA PNG.
- `terrainSendToUnreal(t, kind='meshTerrain')`: UE 5.8's experimental Mesh Terrain through its heightmap importer (needs the `LilaLinkMeshTerrain` plugin next to LilaLink); `sectionQuads=` sets the section size. Landscapes over 8193 x 8193 vertices (or `regions='on'`) are written region by region (`regionComponents=`, default `gridSize`), so huge maps stay within memory.
## Water: rivers, lakes, sea

- `terrainAddRiver(t, id=, auto=True, threshold=2e6)`: a river along every channel draining more than `threshold` m2 (ids `<id>_1`, `<id>_2`, ...). `path=[...]` or `object='Curve'` (drawn or imported, followed live) for one river; a line drawn uphill is turned round (`reversed`). Width and depth follow drainage (`width=`, `depth=` fix them; `widthScale`, `minWidth`). The bed falls all the way to the mouth; banks are re-eroded (`reErode=6`); `meander=1.5` bends it on flat ground; gravel bars form inside bends; a cliff crossing keeps a waterfall with a plunge pool. A river ending inland gets a pond.
- `terrainAddLake(t, id=, at=[x, z])` fills that basin to its spill level (`level=` to set it); `polygon=` or `object='ClosedCurve'` digs a lake (`depth=`, `shoreSlope=`), damming the rim when `level=` is above it; `auto=True, minDepth=, minArea=` makes every deep basin a lake.
- `terrainSeaLevel(t, level=0, beachSlope=10, shoreWidth=1500)`; `off=True` removes it. Basins no water holds are filled so everything drains (`drainPits=False` keeps them).
- Each call returns `rivers` (mouth: edge, sea, lake, river, pond; length, widthMax, discharge, maxCut), `lakes` (level, spillHeight, area m2, maxDepth, volume m3, damHeight), `waterfalls`, `springs`, `cutM3`/`fillM3`, `warnings` (read them: a lake level that overflows says what it used instead) and the feature's `splines`/`locators`.
- `terrainWater(t)` lists the specs and the water as splines (per point width, depth, bedHeight, waterLevel, flowSpeed m/s, discharge m3/s, flowDirection) and locators (lakeLevel, seaLevel, spring, waterfallTop, waterfallBottom). `terrainWaterRemove(t, ids=[...])` or `all=True`.
- Masks: riverBed, riverBank, gravelBar, waterfall, lakeBed, sea, water, shore, beach. A flat preview mesh `<terrain>_water` is rebuilt by every water call (`terrainWaterPreview(t)` after other edits).
- The water node sits after erosion and before the stamps, so add water after `terrainErode` and before city stamps. `terrainWaterInfo` is the analysis of the ground itself and ignores authored water's sea level unless given `seaLevel=`.
- Export: `terrainExport` writes `<name>_water.json` beside the heights; `terrainSendToUnreal` sends the water as plain spline and locator actors with a `LilaWaterData` component (`water=False` skips it; `terrainSendWaterToUnreal` alone). Never Water plugin bodies.
## Roads

- `terrainAddRoad(t, id='pass', path=[[x, z], ...] | 'Curve', kind='road'|'path'|'track'|'rail'|'runway')`: lays a road into the terrain. Corners round into curves of the design radius; the profile is grades no steeper than `maxGrade` (%) joined by vertical curves of at least `minVerticalRadius` (cm), smooth over bumps; curves bank up to `maxBank` (%). The surface is one banked plane across the width; cuttings and embankments slope at `cutAngle`/`fillAngle`; slopes taller than `wallHeight` become retaining walls.
- Read back: `maxGrade`, `maxBank`, `minVerticalRadius`, `minCurveRadius`, `cutM3`/`fillM3`, `spans` (bridge, tunnel), `bridges` (deck, pillars), `tunnels` (axis, radius, portals: what a cave pass carves), `walls`, `guardrails`, `props`, `junctions`, `kerbReturns`, `warnings`. Every warning means a limit was not met; fix the path or settings.
- `keys=[[x, y, z], ...]` pins heights (a pass, a bridge deck). `profile=`: rural, highway, urban, lane, path, track, rail, runway; any field by flag (`width=`, `lanes=`, `designSpeed=`, `bridgeHeight=`, ...) or `settings={...}`.
- One call from A to B: `terrainRoute(t, start=, end=, maxGrade=8, road='road', roadId='pass')`.
- Roads in one layer meet at grade: a later road ending on or crossing an earlier one takes its height and cross fall, with kerb returns at the corners. Lay main roads first.
- Masks for materials: `roadSurface`, `roadVerge`, `roadDirtEdge`, `roadWear`, `erosionProtect` (plus `paved` on roads and runways). `stage='base'` lays the road before erosion, which then leaves it alone.
- Meshes: `terrainRoadBuild(t)` makes `<id>_Road` groups (swept surface, bridge decks and pillars, portals, walls, junction patches, guardrails, markers, poles). Build again after changes.
- Out: `terrainExport` writes `<name>_roads.json` (curves with grade, bank, width, span per point, world and Unreal positions); `terrainSendToUnreal` sends roads as landscape splines (`terrainSendRoadsToUnreal(t, segmentMesh='/Game/...', paintLayer='Road', raise=False)`).
- `terrainRoads(t)` lists them; `terrainRoadRemove(t, ids=[...])` removes them. Everything undoes.
