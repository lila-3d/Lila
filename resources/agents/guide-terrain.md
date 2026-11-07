# Terrain in Lila, for agents

A terrain is a mesh whose geometry-nodes modifier builds a heightfield. Lengths and heights are cm, in world space. Points are `[x, z]` on the ground (`[x, y]` on a Z-up scene) or `[x, y, z]`. Every read returns the terrain's `revision`: equal revisions mean the same ground, so cache by it.

## Recipes

- From a description (a prompt): write one spec and build it, see "One spec builds it all". Start from a preset: `terrainSpec(list=True)`.
- From a painted image: `terrainImport('sketch.png', mode='sketch')`, or `source: {type: 'image'}` in a spec. See "From a real place, a painting or drawn lines".
- From a real place: `terrainImportDem(place='Matterhorn')`, or `source: {type: 'dem'}`. Same section.
- Name a place, then use it: `terrainQuery(t, named='passes', store='pass')`, then `'region:pass'` in any point, path or polygon. See "Targets in words".
- Add a road: `terrainRoute(t, start=, end=, road='road')` or `terrainAddRoad`. See "Roads".
- Add a river, lake or sea: `terrainAddRiver`, `terrainAddLake`, `terrainSeaLevel`. See "Water: rivers, lakes, sea".
- Caves, overhangs, arches: `terrainCave`, `terrainOverhang`, `terrainArch`. See "Caves, overhangs and arches".
- Check it: `terrainCheck(t)` until `ok`. Look at it: `terrainMap(t, file=)`.
- Compare options: `terrainWedge(spec=, vary={'seed': [1, 2, 3]})`. See "Wedges".
- Ship it: `terrainSendToUnreal(t)` or `terrainExport(t, target='unreal'|'unity'|'godot'|'files'|'mesh')`. See "To Unreal" and "To other engines and as meshes".

## One spec builds it all

`terrainSpec(spec={...})` builds a whole terrain from one JSON and returns `ok` (every step ran and `terrainCheck` passed), `steps` (each one's `command`, `ok`, `warnings`, `error`, `ms` and result), `check`, `exports` and `stats` with the `revision`. The schema is `lila://schema/terrain-spec`; every step is the flags of one command, so that command's help is its reference.

```python
r = terrainSpec(spec={
    'name': 'Valley', 'size': [200000, 200000], 'resolution': 257, 'seed': 3,
    'source': {'type': 'noise', 'amplitude': 4000, 'featureSize': 40000},
    'constraints': [{'kind': 'ridge', 'path': [[-100000, 60000], [100000, 60000]], 'height': 16000, 'width': 70000}],
    'erosion': {'preset': 'natural', 'age': 0.4},
    'water': {'rivers': [{'id': 'river', 'path': [[-100000, 0], [100000, 0]], 'width': 1200}]},
    'regions': [{'name': 'pass', 'named': 'passes', 'where': {'z': [35000, 85000]}, 'pick': 0}],
    'roads': [{'id': 'passRoad', 'route': {'start': [0, 14000], 'end': 'region:pass', 'maxGrade': 10}}],
    'volume': [{'op': 'cave', 'path': ['region:pass', [0, 90000]], 'radius': 500}],
    'materials': True,
    'export': {'target': 'unreal'}})
r['ok'], [s for s in r['steps'] if not s['ok']], r['check']['issues']
```

- Steps run in this order: source (`noise`, `flat`, `image`, `dem`), constraints, sculpt, erosion, landforms, water (sea, rivers, lakes), regions, roads (`route` finds the way with `terrainRoute`), stamps, volume, biomes, materials, groundCover, check, export. A failed step stops the build (`stopOnError=False` goes on); read `failed`.
- The name must be free; `replace=True` rebuilds it.
- Presets: `alpineValley`, `mesaDesert`, `coastalCliffs`, `rollingFarmland`, `volcanicIsland`. `terrainSpec(preset='mesaDesert', overrides={'seed': 4, 'resolution': 257})` builds one; `build=False` returns the spec to edit.
- Read back: `terrainSpec(t)['spec']` rebuilds the terrain. It is the authored spec while the terrain is unchanged since; after other edits it is the node group (`from: 'graph'`).
- Iterate at resolution 257, then raise it for the final build.
- The MCP prompt `make_terrain` walks through spec, check and export.

## Targets in words

`terrainQuery(t, ...)` finds places so you never guess coordinates. It returns `regions` (largest first: `polygon`, `areaM2`, `centroid`, `point` inside, `lowest`, `highest`, `height` and `slope` stats) and `points`.

- `named=`: `highestPeak`, `lowestPoint`, `passes` (saddles ranked by how far the ridge rises either side), `valleyFloor`, `ridges`, `flatAreas`, `steepSlopes`, `cliffs`, `northSlopes` (and south, east, west), `highGround`, `lowGround`, `nearWater`, `rivers`, `lakes`.
- `where={...}`, all must hold: `x`, `z` (ground coordinates), `height` (cm), `relativeHeight` (0 to 1), `slope`, `aspect` (degrees, or `'north'`, `'southwest'`...), `curvature`, `flow`, `distanceToWater`, `distanceToRoads` (cm), `water`, `region` (a stored name), any `terrainField` field, any mask. Values: `[min, max]` with `None` open, a number (at least), or `{'above': , 'below': }`. With `named=`, the shortcut's own ranges are added and point shortcuts pick only where `where` holds.
- `minArea=` (m2), `maxRegions=`.
- `store='pass'` keeps the result as a region: mask `region_pass`, usable by material rules. `pick=k` keeps only region k. `list=True` lists stored regions; `remove='pass'` drops one.
- References: `'region:pass'` is the region's point (stored point, else a point inside), or its outline where a polygon is wanted; `'region:pass/1'` is its part 1. They work in `terrainAddLake` (`at`, `polygon`), `terrainAddRiver`, `terrainAddRoad`, `terrainConstrain`, `terrainStroke`, `terrainSample`, `terrainProfile` (path points), `terrainRoute` (`start`, `end`), `terrainStampPad`, `terrainStampRegion` (`footprint`), `terrainRegionStats` (`polygon`), `terrainCave`, `terrainTunnel`, `terrainOverhang` (path points), `terrainArch` (`from`, `to`) and `terrainVolumeStamp` (`center`).

## Wedges

`terrainWedge(spec= | preset= | t, vary={'seed': [1, 2, 3], 'erosion.age': [0.3, 0.6, 0.9]}, resolution=257)` builds each variant in its own headless `lilabatch` process, several at once, and writes a labelled contact sheet of top-down maps. Dotted keys reach nested settings; each variant takes the next value of every key (`count=` variants), or `grid=True` builds every combination. Returns `sheet`, `mode`, and per variant `vary`, `ok`, `revision`, `stats`, `check`, `image` and `error`; the same goes to `<folder>/<name>_wedge.json`. `mode` is `inProcess` with a `note` when the variants were built here one after another. Build the one you want with `terrainSpec(spec=s, overrides=...)`.

`terrainMap(t, file=, resolution=512)` writes one terrain's map: tinted by height, shaded, water blue, roads dark, north (-Z) at the top.

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
- `terrainCheck(t, buildings=[meshes])`: pads over the cut/fill limit, streets over grade, things in flood without a levee, buildings over water or floating, streets or pads blocking rivers, canals reaching no water. Also roads over their design grade or bank (`roadGrade`, `roadBank`) or over water with no bridge or culvert (`roadWater`), river beds or water rising downstream (`riverUphill`), lakes below their rim with no outlet (`lakeNoOutlet`) or above it with no dam (`lakeOverflow`). Fix until `ok`.
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
- Read back: `maxGrade`, `maxBank`, `minVerticalRadius`, `minCurveRadius`, `cutM3`/`fillM3`, `spans` (bridge, tunnel), `bridges` (deck, pillars), `tunnels` (axis, radius, portals: what a cave pass carves), `walls`, `guardrails`, `props`, `junctions`, `kerbReturns`, `islands`, `warnings`. Every warning means a limit was not met; fix the path or settings.
- `keys=[[x, y, z], ...]` pins heights (a pass, a bridge deck). `profile=`: rural, highway, urban, lane, path, track, rail, runway; any field by flag (`width=`, `lanes=`, `designSpeed=`, `bridgeHeight=`, ...) or `settings={...}`.
- One call from A to B: `terrainRoute(t, start=, end=, maxGrade=8, road='road', roadId='pass')`.
- Roads in one layer meet at grade: a later road ending on or crossing an earlier one takes its height and cross fall, with kerb returns at the corners. Lay main roads first.
- Roundabouts: `terrainAddRoundabout(t, at=[x, z], radius=2500)` after the roads it joins. Roads reaching the ring are cut back to its centreline (one running through becomes `<id>` and `<id>_b`, listed in `approaches`); the ring is level with a cross fall away from a raised central island, and each approach gets a splitter island (`islands`, mask `roadIsland`). The ring sends to Unreal as a closed spline.
- Masks for materials: `roadSurface`, `roadVerge`, `roadDirtEdge`, `roadWear`, `erosionProtect` (plus `paved` on roads and runways). `stage='base'` lays the road before erosion, which then leaves it alone.
- Meshes: `terrainRoadBuild(t)` makes `<id>_Road` groups (swept surface, bridge decks and pillars, portals, walls, junction patches, islands, guardrails, markers, poles). Build again after changes.
- Out: `terrainExport` writes `<name>_roads.json` (curves with grade, bank, width, span per point, world and Unreal positions); `terrainSendToUnreal` sends roads as landscape splines (`terrainSendRoadsToUnreal(t, segmentMesh='/Game/...', paintLayer='Road', raise=False)`).
- `terrainRoads(t)` lists them; `terrainRoadRemove(t, ids=[...])` removes them. Everything undoes.

## Caves, overhangs and arches

The heightfield is the ground; a volume adds to it and carves from it. Edits go in a Terrain Volume node last in the terrain's group, and save, undo and replay like stamps.

- `terrainCave(t, path=[[x, z], ...] | curve=, radius=300, branches=, chambers=, noise=)`: ground points sit `below` cm under the ground (three radii), the ends `endBelow` (a third of a radius) so the cave opens there; `[x, y, z]` points are used as they are.
- `terrainTunnel(t, path= | curve= | fromStamp='Stamps/street1', radius=400)`: a road tunnel with a flat floor at road level; `fromStamp` bores a street stamp's tunnel spans. `fromRoad='pass'` bores a terrainAddRoad road's tunnel spans at their radius.
- `terrainOverhang(t, path=, depth=400, height=600, strata=150, hardness=0.35, lip=)`: undercuts the side of the line the hill rises on, deeper in soft bands.
- `terrainArch(t, **{'from': [x, z], 'to': [x, z]}, height=1500, thickness=250, width=400)`: `carve=True` opens rock under it.
- `terrainVolumeStamp(t, object='Boulder' | shape='sphere'|'box'|'capsule', mode='add'|'subtract')`. `terrainVolumeRemove(t, ids=[])`, `terrainVolume(t)` lists edits and the split.
- Every edit returns `volume`: `pieces` (name, faces, LOD faces, bounds, holeCells, buriedCells, materials), `holeCells`, `buriedCells`, `heightError`, `unmeshedColumns`, `seamUncovered`, `seamGap`, `warnings`. Healthy: `unmeshedColumns == 0`, `seamUncovered == 0`, `seamGap` near 0.
- Split (`terrainSplit(t, split=True, lods=3, minRoof=200, margin=1, skirt=200, maxHeightError=0)`): what the heightfield can hold goes into its heights; each connected place it can't becomes a mesh piece. Caves with at least `minRoof` of rock above keep the landscape on top; openings get a `holes` mask and a piece that fills them exactly, its edge on the ground and a skirt below. `split=False` keeps the heights and meshes every change. `maxHeightError=100` also meshes sheer detail the cells are too coarse for.
- `terrainCheck(t)` adds `volumeSeam`, `volumeHoles`, `volumeUnmeshed` and `volumeEdit` issues.
- Export: `terrainExport` writes `<name>_<piece>.fbx` (LOD group, world space about each piece's centre) and, for Unreal, `<name>_visibility.raw` instead of a holes paint layer. `terrainSendToUnreal` places the pieces as static meshes, writes the holes to the landscape's visibility layer, adds a Landscape Visibility Mask to Lila's generated landscape material, and gives the pieces `MI_<Label>_Volume` on `MF_LilaTerrainVolume` (triplanar, matching GroundColor); `volumeMaterial=` uses yours. Your own landscape material needs a Landscape Visibility Mask on Opacity Mask for the holes to show.

## Erode, landforms and brushes

- One knob: `terrainErode(t, preset='natural'|'alpine'|'desert'|'hills', age=0.6)` runs macro form (analytic stream power at low res), amplify, debris flow and scree, droplet rills and deposition (alpine adds snow). `age=0` is off, 1 is the oldest. `hardnessMask=` slows erosion and steepens scree on hard rock. `terrainCreate(age=0.5, erosionPreset='alpine')` sets it at creation; the result's `preset.stepsMs` times each step. Masks out: `flow`, `lakes`, `wear`, `deposit`, `sediment`, `wet`, `talus` (+ `snow`).
- One step at a time: `terrainLandform(t, process=, settings={...})` with process `analytic` (age), `amplify` (factor: more samples, drainage kept), `debris`, `thermal` (`materialMask`, `materialAngle` for a talus angle per material), `hydraulic` (droplets: `density`, `seed`), `deposition`, `snow` (`snowline`, `maxSlope`), `filter` (instant gullies, `device='auto'` runs it on the GPU), `terrace`, `strata` (writes `strata`, use it as `hardnessMask`), `fold`, `warp`, `ridgeSharpen`, `cliff`. Steps go after erosion and before stamps; `stage='base'` puts one before Erode Terrain. Same `id` changes it, `remove=True` drops it.
- Brushes: `terrainStroke(t, brush=, points=|path=|curve=, radius=, strength=)` with brush raise, lower, smooth, flatten (`height=` or `plane=True`), noise, ramp (first to last point, `height=` and `endHeight=`), terrace (`step=`), stamp (`stamp='peak'|'crater'|'dune'|'rocks'|'mesa'|'ridge'|'volcano'` or an image, `angle=`), erode (`iterations=`), uplift (paints the `uplift` mask Erode Terrain lifts by, before erosion), relax, mask (`mask=`, `value=`). `pressure=[...]` weights each point, `mirror='x'|'z'|'xz'` mirrors across the centre, `falloff=` takes a brush falloff. Raise, lower, noise and stamp `strength` is cm; the rest 0 to 1. Each call is kept, so the same calls on the same terrain give the same `revision`.
- Overhangs: `terrainSculptRegion(t, center=[x, z], size=15000)` copies a patch to a mesh in sculpt mode; sculpt it, then `terrainSculptRegion(t, finish=True)` bakes the height change back and lists `overhangs`. The hollow under them is carved from the volume (a hidden `<t>_overhangHollow` mesh, `overhangVolume` in the result), so they survive as volume pieces; `keepOverhangs=False` only lists them.

## From a real place, a painting or drawn lines

- Real place: `terrainImportDem(place='Matterhorn', size=[1200000, 1200000])` or `bbox=[west, south, east, north]` in degrees. `source='copernicus'` (GLO-30, default), `'terrarium'` (AWS Terrain Tiles, `spacing=` cm picks the detail) or `'opentopography'` (`dataset='SRTMGL1'`, `apiKey=`, `saveKey=True` once). A local DEM works too: `terrainImportDem('dem/N46E007.hgt')`. `projection='utm'` (default), `'enu'` or `'local'`. Copernicus includes trees and roofs: `bareEarth=True`. Coarse 30 m data: `amplify=4` adds eroded detail at 7.5 m and keeps the place.
- The result's `georef` and `terrainGeoref(t, latLon=[[lat, lon]])` give world points for real coordinates; `points=[[x, z]]` gives latitude and longitude back.
- Attribution for every source goes to the workspace's `terrain/ATTRIBUTION.txt`; ship it with the game. Read `attribution` and `notes` in the result (open-sea tiles at sea level, tile zoom used).
- OSM: `terrainImportOsm('area.osm', terrain=t)` (or `.json` from Overpass, `.osm.pbf`, or `fetch=True`). Roads, paths, rivers, streams, lakes and coastline come in as curves draped on the ground, one curve per kind under `<terrain>_OSM` (`perFeature=True` for one per feature); `osmFeatures` on each curve lists id, name, class and width (cm) per spline. Land cover becomes masks `landcover_forest`, `landcover_urban`, `landcover_water` ... Terrains not from real data need `origin=[lat, lon]` of their centre.
- Painted sketch: `terrainImport('sketch.png', mode='sketch', size=400000, high=60000)`. Grey is the base height; red strokes ridges, green valleys, blue rivers, yellow flat ground. Erosion keeps them. 8-bit paintings are de-stepped in every mode.
- Drawn constraints: `terrainConstrain(t, kind='ridge'|'valley'|'cliff'|'river'|'flat', curve='Line' or path=[[x, z], ...], height=, width=)`. They are held while erosion runs. Draw rivers downstream: the bed only falls along the curve. Same `id` replaces; `remove=id`, `clear=True`.
- Live link: `terrainLiveLink(t)` re-runs the terrain whenever its file is saved (in the app). Headless, call `terrainReload(t)` after writing the file.

## Materials, biomes, ground cover

1. `terrainBiomes(t)` after erosion and stamps: masks `temperature` (0 to 1 over -30 to 40 C), `moisture` and `biome_<name>` (snow, alpine, forest, meadow, scrub, wetland by default; weights sum to 1). `climate={'temperature': 8, 'lapseRate': 0.65, 'windAzimuth': 270, 'rainShadow': 0.8, 'seaLevel': 0}` sets the climate; `biomes=[{'name': 'taiga', 'when': {'temperature': [None, 5], 'moisture': [0.4, 1]}, 'paint': 'myMask'}]` your own; `landcover={'mask': 'worldcover'}` follows an imported landcover mask (ESA WorldCover codes by default).
2. `terrainMaterialRules(t)` lays default layers fitted to the terrain (grass base, dirt on wear, gravel on deposit, riverbed on flow, rock over 38 degrees, snow by temperature or the top of the range, sand with `seaLevel=`, road where a road mask exists). Layers go bottom first; the first is the base. `set={'name': 'moss', 'material': 'meadowMoss', 'when': {'moisture': [0.7, None], 'slope': [0, 25]}, 'noise': {'amount': 0.3, 'scale': 2000}}` adds or replaces one, `remove=['moss']` drops one, `layers=[...]` replaces all. `when` inputs: height (cm), slope, aspect (degrees), curvature, nearCliff, temperature (C), moisture, flow, wear, deposit, lakes, wet, any mask, `biome: 'forest'`. Each range is `[min, max]` (None for open) or `{'range': [a, b], 'fade': f, 'invert': True}`. Read `layers[].coverage` (share of the terrain), `maxSumError` (should be ~0) and `missing` (inputs the terrain lacks, read as 0).
3. Rules also set the heightfield draw's paint layers (each `layer_<name>` in its colour). `terrainMaterialPreview(t)` shows the layers through the terrain's material (a texture set on the terrain; rerun after changes). `terrainSatmap(t, shade=0.5)` bakes a colour image; `terrainMaskImages(t, masks=[...], layout='uv')` writes masks for texture graphs.
4. `terrainGroundCover(t, items={'rock': 'SM_Rock', 'tree': 'SM_Pine', 'grassTuft': 'SM_Tuft', 'debris': 'SM_Debris'})` scatters rocks on scree, trees in the forest biome, grass on the grass layer and debris below cliffs, live, on `<t>_GroundCover`. Own rules: `rules=[{'name': 'pines', 'items': ['pine'], 'density': 0.02, 'footprint': 250, 'when': {'biome': 'forest', 'slope': [0, 30]}, 'scale': [0.8, 1.3], 'align': 0}]` (density per m2; `layer: 'grass'` in `when` reads a layer; `solid=False` for grass under trees). Missing items get hidden placeholders (`placeholders` says which). `settle=True` drops them with physics; `sendToUnreal=True` or `unrealPlaceInstances('<t>_GroundCover', mode='hism')` ships them.
5. Ship: `terrainSendToUnreal(t, rvt=True)` sends material layers weight-blended as Grass, Rock, ... with `M_<t>_Terrain` on `MF_LilaTerrainLayer` and a runtime virtual texture; `terrainExport(t, target='files')` adds RGBA splats (`files.splats`, bytes summing to 255).

## See it in the viewport

- `terrainDisplay(draw='heightfield')` draws terrains from their heights: level of detail for 8K to 16K grids, paint layers and the overlays below. `draw='mesh'` draws the meshed terrain. `quadPixels=` sets the detail (4 by default; lower is finer and slower).
- Overlays, any of which switches terrains to the heightfield draw while on: `overlay='flow'` (any mask; `overlayRamp='grey'|'turbo'`, `overlayOpacity=`), `contours=1000` (cm between lines, `contourMajor=5`), `flowArrows=True`. `overlay=''`, `contours=0` and `flowArrows=False` turn them off. The result lists each terrain's masks and warns when no terrain has the overlay's mask.
- `terrainDisplayLayers(t, layers=[{'name': 'rock', 'mask': 'wear', 'color': '#7a7068', 'roughness': 0.9, 'baseColor': 'rock.png', 'normal': 'rock_n.png', 'tiling': 400}])` paints layers over the terrain's material by their masks' weights (0 to 1), in order. It returns each layer's mask coverage and whether the mask exists. Layer rules can write the same JSON list to the mesh's `terrainLayers` attribute. Undoable.
- Check the result with `captureView(display='shaded')`; selected terrains show their wire.
