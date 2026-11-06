@tool
extends EditorScript
## Imports a Lila terrain export (terrainExport target='godot') into a Terrain3D node.
## Needs the Terrain3D addon (1.0 or later). Copy the export folder into the project, open the scene to put the terrain
## in, open this script in the Script Editor and run it (File > Run). It reuses the scene's first Terrain3D node, or
## makes one named after the terrain, imports every region's heights and writes its control map from Lila's layer
## weights: the heaviest layer is the base texture, the next the overlay, blended by their weights. Texture slots follow
## the export's layer order; add your textures to the Terrain3D assets in that order. Save the scene to keep the data.

const TERRAIN_JSON := "%TERRAIN_JSON%"


func _run() -> void:
	var dir := (get_script() as Script).resource_path.get_base_dir()
	var text := FileAccess.get_file_as_string(dir.path_join(TERRAIN_JSON))
	var info = JSON.parse_string(text)
	if typeof(info) != TYPE_DICTIONARY or info.get("target") != "godot":
		push_error("Lila: %s isn't a Lila Terrain3D export" % dir.path_join(TERRAIN_JSON))
		return
	var root := get_scene()
	if root == null:
		push_error("Lila: open the scene to import the terrain into first")
		return

	var terrain := _find_terrain(root)
	if terrain == null:
		terrain = Terrain3D.new()
		terrain.name = info["name"]
		root.add_child(terrain)
		terrain.owner = root
	if terrain.data_directory.is_empty():
		var data_dir := dir.path_join(info["name"] + "_data")
		DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(data_dir))
		terrain.data_directory = data_dir
	terrain.vertex_spacing = float(info["vertexSpacing"])
	var region_size := int(info["regionSize"])
	if terrain.region_size != region_size:
		terrain.change_region_size(region_size)

	var layers: int = info["layers"].size()
	var footprint: Array = info["footprint"]
	var spacing := float(info["vertexSpacing"])
	for region in info["regions"]:
		var height := Image.load_from_file(dir.path_join(region["heightmap"]))
		if height == null:
			push_error("Lila: can't read %s" % region["heightmap"])
			return
		height.convert(Image.FORMAT_RF)
		var splats: Array[Image] = []
		for file in region["splats"]:
			splats.append(Image.load_from_file(dir.path_join(file)))
		var corner: Array = region["position"]
		var origin := Vector3(float(corner[0]), float(corner[1]), float(corner[2]))
		var control := _control_map(height.get_width(), splats, layers, origin, spacing, footprint)
		# Terrain3D's map order: height, control, color.
		terrain.data.import_images([height, control, null], origin, 0.0, 1.0)
	print("Lila: imported %d Terrain3D regions of %s" % [info["regions"].size(), info["name"]])


func _find_terrain(node: Node) -> Terrain3D:
	if node is Terrain3D:
		return node
	for child in node.get_children():
		var found := _find_terrain(child)
		if found != null:
			return found
	return null


## Base texture is the heaviest layer, overlay the next, blend their share; outside the terrain's footprint is a hole.
func _control_map(size: int, splats: Array[Image], layers: int, origin: Vector3, spacing: float, footprint: Array) -> Image:
	var control := Image.create_empty(size, size, false, Image.FORMAT_RF)
	var eps := spacing * 0.001
	for y in size:
		var wz := origin.z + y * spacing
		for x in size:
			var wx := origin.x + x * spacing
			var hole: bool = wx < footprint[0] - eps or wz < footprint[1] - eps or wx > footprint[2] + eps or wz > footprint[3] + eps
			var first := 0
			var second := 0
			var w1 := -1.0
			var w2 := -1.0
			for l in layers:
				var w: float = splats[l >> 2].get_pixel(x, y)[l & 3]
				if w > w1:
					second = first
					w2 = w1
					first = l
					w1 = w
				elif w > w2:
					second = l
					w2 = w
			var blend := 0
			if layers > 1 and w1 + w2 > 0.0:
				blend = int(round(255.0 * w2 / (w1 + w2)))
			var bits: int = Terrain3DUtil.enc_base(first) | Terrain3DUtil.enc_overlay(second) | Terrain3DUtil.enc_blend(blend) \
				| Terrain3DUtil.enc_hole(hole) | Terrain3DUtil.enc_auto(false)
			control.set_pixel(x, y, Color(Terrain3DUtil.as_float(bits), 0.0, 0.0, 1.0))
	return control
