"""Writes the .blend fixtures for lila_blend_tests and the oracle Blender evaluates for each.

Run by hand with an installed Blender, once per Blender version wanted:
    blender -b --factory-startup --python tests/blend/fixtures/make_fixtures.py -- <out dir>

For each fixture it writes <name>.blend plus <name>.json: Blender's own evaluated result (world matrices, evaluated
vertex/face counts, bounds), which the tests compare Lila's import against.
"""

import json
import math
import os
import sys

import bpy
import bmesh
from mathutils import Matrix, Vector

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else os.path.dirname(__file__)
os.makedirs(OUT, exist_ok=True)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = 24
    scene.frame_start = 1
    scene.frame_end = 48
    return scene


def link(ob, coll=None):
    (coll or bpy.context.scene.collection).objects.link(ob)
    return ob


def mesh_object(name, build, coll=None):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    build(bm)
    bm.to_mesh(me)
    bm.free()
    return link(bpy.data.objects.new(name, me), coll)


def cube(bm, size=1.0):
    bmesh.ops.create_cube(bm, size=size)


def oracle(name, extra=None):
    deps = bpy.context.evaluated_depsgraph_get()
    objects = {}
    for ob in bpy.context.scene.objects:
        e = ob.evaluated_get(deps)
        entry = {"type": ob.type, "world": [list(r) for r in ob.matrix_world], "parent": ob.parent.name if ob.parent else None,
                 "parentBone": ob.parent_bone or None}
        if ob.type == "MESH":
            m = e.to_mesh()
            entry["verts"] = len(m.vertices)
            entry["faces"] = len(m.polygons)
            entry["origVerts"] = len(ob.data.vertices)
            entry["origFaces"] = len(ob.data.polygons)
            if m.vertices:
                lo = [min(v.co[i] for v in m.vertices) for i in range(3)]
                hi = [max(v.co[i] for v in m.vertices) for i in range(3)]
                entry["bounds"] = [lo, hi]
            entry["modifiers"] = [md.type for md in ob.modifiers]
            e.to_mesh_clear()
        objects[ob.name] = entry
    data = {"blender": bpy.app.version_string, "objects": objects}
    if extra:
        data.update(extra)
    with open(os.path.join(OUT, name + ".json"), "w") as f:
        json.dump(data, f, indent=1)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, name + ".blend"), compress=True)


def make_basic():
    """Meshes with attributes, materials, cameras, lights, empties, parenting, collections."""
    reset()
    scene = bpy.context.scene
    props = bpy.data.collections.new("Props")
    scene.collection.children.link(props)
    inner = bpy.data.collections.new("Inner")
    props.children.link(inner)

    def uv_cube(bm):
        cube(bm, 2.0)
        uv = bm.loops.layers.uv.new("UVMap")
        bm.loops.layers.uv.new("Lightmap")
        for f in bm.faces:
            for i, l in enumerate(f.loops):
                l[uv].uv = ((i in (1, 2)) * 1.0, (i in (2, 3)) * 1.0)
    box = mesh_object("Box", uv_cube, props)
    box.location = (1.0, 2.0, 3.0)
    box.rotation_euler = (math.radians(10), math.radians(20), math.radians(30))
    box.scale = (1.0, 2.0, 0.5)
    me = box.data
    col = me.color_attributes.new("Col", "FLOAT_COLOR", "CORNER")
    for i, c in enumerate(col.data):
        c.color = (i / 24.0, 0.5, 1.0 - i / 24.0, 1.0)
    pc = me.color_attributes.new("PointCol", "BYTE_COLOR", "POINT")
    for i, c in enumerate(pc.data):
        c.color = (1.0, i / 8.0, 0.0, 1.0)
    for e in me.edges[:4]:
        e.use_edge_sharp = True
    crease = me.attributes.new("crease_edge", "FLOAT", "EDGE")
    crease.data[0].value = 1.0
    wear = me.attributes.new("wear", "FLOAT", "POINT")
    for i, v in enumerate(wear.data):
        v.value = i * 0.5

    red = bpy.data.materials.new("Red")
    red.use_nodes = True
    bsdf = red.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0.8, 0.1, 0.05, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.3
    bsdf.inputs["Metallic"].default_value = 1.0
    blue = bpy.data.materials.new("Blue")
    blue.use_nodes = True
    nt = blue.node_tree
    img = bpy.data.images.new("checker", 4, 4)
    img.pixels = [((i // 4 + i % 4) % 2) * 1.0 if k < 3 else 1.0 for i in range(16) for k in range(4)]
    img.pack()
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    nt.links.new(tex.outputs["Color"], nt.nodes["Principled BSDF"].inputs["Base Color"])
    nt.nodes["Principled BSDF"].inputs["Emission Strength"].default_value = 2.0
    nt.nodes["Principled BSDF"].inputs["Emission Color"].default_value = (0.0, 1.0, 0.0, 1.0)
    me.materials.append(red)
    me.materials.append(blue)
    for i, p in enumerate(me.polygons):
        p.material_index = i % 2
        p.use_smooth = i < 3

    ball = mesh_object("Ball", lambda bm: bmesh.ops.create_uvsphere(bm, u_segments=12, v_segments=6, radius=0.5), inner)
    ball.parent = box
    ball.location = (0.0, 0.0, 2.0)
    for p in ball.data.polygons:
        p.use_smooth = True

    empty = link(bpy.data.objects.new("Pivot", None))
    empty.empty_display_type = "ARROWS"
    empty.location = (-2.0, 0.0, 0.0)
    empty.rotation_mode = "QUATERNION"
    empty.rotation_quaternion = (0.9238795, 0.0, 0.3826834, 0.0)
    child = mesh_object("Child", cube)
    child.parent = empty
    child.location = (0.0, 0.0, 1.0)
    child.rotation_mode = "ZXY"
    child.rotation_euler = (math.radians(30), math.radians(40), math.radians(50))

    cam = link(bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam")))
    cam.location = (7.0, -6.0, 5.0)
    cam.rotation_euler = (math.radians(63), 0.0, math.radians(46))
    cam.data.lens = 35.0
    cam.data.clip_start = 0.2
    cam.data.clip_end = 250.0
    ortho = link(bpy.data.objects.new("Ortho", bpy.data.cameras.new("Ortho")))
    ortho.data.type = "ORTHO"
    ortho.data.ortho_scale = 10.0
    ortho.location = (0.0, 0.0, 10.0)

    for kind, loc in (("POINT", (2, 2, 2)), ("SUN", (0, 0, 5)), ("SPOT", (-3, 3, 3)), ("AREA", (3, -3, 3))):
        la = bpy.data.lights.new(kind.title(), kind)
        la.energy = 100.0 if kind != "SUN" else 3.0
        la.color = (1.0, 0.9, 0.8)
        if kind == "SPOT":
            la.spot_size = math.radians(60)
            la.spot_blend = 0.25
        if kind == "AREA":
            la.size = 2.0
        ob = link(bpy.data.objects.new(kind.title(), la))
        ob.location = loc
        ob.rotation_euler = (math.radians(30), 0.0, math.radians(20))

    hidden = mesh_object("Hidden", cube)
    hidden.hide_viewport = True
    hidden.location = (0, 5, 0)

    # A mesh shared by two objects.
    twin = link(bpy.data.objects.new("Twin", box.data))
    twin.location = (5, 0, 0)
    oracle("basic")


def make_modifiers():
    reset()
    specs = [
        ("Mirror", "MIRROR", dict(use_axis=(True, False, False), use_mirror_merge=True)),
        ("Subsurf", "SUBSURF", dict(levels=2)),
        ("Bevel", "BEVEL", dict(width=0.1, segments=2)),
        ("Array", "ARRAY", dict(count=3, relative_offset_displace=(1.5, 0, 0))),
        ("Solidify", "SOLIDIFY", dict(thickness=0.2)),
        ("Triangulate", "TRIANGULATE", dict()),
        ("Decimate", "DECIMATE", dict(ratio=0.5)),
        ("Weld", "WELD", dict(merge_threshold=0.01)),
    ]
    for i, (name, kind, settings) in enumerate(specs):
        if name in ("Solidify", "Decimate"):
            ob = mesh_object(name, lambda bm: bmesh.ops.create_grid(bm, x_segments=4, y_segments=4, size=1.0))
        elif name == "Mirror":
            def half(bm):
                cube(bm)
                bmesh.ops.delete(bm, geom=[v for v in bm.verts if v.co.x < 0], context="VERTS")
                bmesh.ops.translate(bm, verts=bm.verts, vec=(0.5, 0, 0))
            ob = mesh_object(name, half)
        else:
            ob = mesh_object(name, cube)
        ob.location = (i * 4.0, 0, 0)
        md = ob.modifiers.new(name, kind)
        for k, v in settings.items():
            setattr(md, k, v)
    # Two modifiers in a row: mirror then subdivide.
    ob = mesh_object("Stack", cube)
    ob.location = (0, 6, 0)
    ob.modifiers.new("Mirror", "MIRROR").use_axis = (False, True, False)
    ob.modifiers.new("Subsurf", "SUBSURF").levels = 1
    # A modifier Lila has no equivalent for, to check the dropped-modifier report.
    ob = mesh_object("Wave", cube)
    ob.location = (4, 6, 0)
    ob.modifiers.new("Wave", "WAVE")
    oracle("modifiers")


def make_rig():
    """Armature, skinned mesh, shape keys, object and bone animation."""
    reset()
    arm = bpy.data.armatures.new("Rig")
    rig = link(bpy.data.objects.new("Rig", arm))
    rig.location = (0, 0, 0.5)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    bones = []
    for i, (head, tail, roll) in enumerate((((0, 0, 0), (0, 0, 1), 0.0), ((0, 0, 1), (0, 0.5, 2), 0.3), ((0, 0.5, 2), (0.5, 0.5, 2.5), -0.2))):
        b = arm.edit_bones.new(["Root", "Mid", "Tip"][i])
        b.head, b.tail, b.roll = head, tail, roll
        if bones:
            b.parent = bones[-1]
            b.use_connect = True
        bones.append(b)
    bpy.ops.object.mode_set(mode="OBJECT")

    def column(bm):
        bmesh.ops.create_cube(bm, size=0.5)
        for v in bm.verts:
            v.co.z = (v.co.z + 0.25) * 5.0
        bmesh.ops.subdivide_edges(bm, edges=[e for e in bm.edges if abs(e.verts[0].co.z - e.verts[1].co.z) > 0.1], cuts=4)
    body = mesh_object("Body", column)
    body.parent = rig
    groups = [body.vertex_groups.new(name=n) for n in ("Root", "Mid", "Tip")]
    for v in body.data.vertices:
        z = v.co.z
        groups[0].add([v.index], max(0.0, 1.0 - z), "REPLACE")
        groups[1].add([v.index], max(0.0, 1.0 - abs(z - 1.25)), "REPLACE")
        groups[2].add([v.index], max(0.0, min(1.0, z - 1.5)), "REPLACE")
    md = body.modifiers.new("Armature", "ARMATURE")
    md.object = rig

    body.shape_key_add(name="Basis")
    bulge = body.shape_key_add(name="Bulge")
    for i, v in enumerate(bulge.data):
        v.co = v.co + Vector((v.co.x * 0.5, v.co.y * 0.5, 0.0))
    bulge.slider_max = 2.0
    bulge.value = 0.25

    rig.pose.bones["Mid"].rotation_mode = "XYZ"
    for f, angle in ((1, 0.0), (24, 45.0), (48, 0.0)):
        rig.pose.bones["Mid"].rotation_euler = (math.radians(angle), 0.0, 0.0)
        rig.pose.bones["Mid"].keyframe_insert("rotation_euler", frame=f)
        rig.pose.bones["Tip"].rotation_quaternion = Matrix.Rotation(math.radians(angle), 4, "Z").to_quaternion()
        rig.pose.bones["Tip"].keyframe_insert("rotation_quaternion", frame=f)
    for f, x in ((1, 0.0), (12, 2.0), (48, 4.0)):
        rig.location.x = x
        rig.keyframe_insert("location", index=0, frame=f)
    for f, w in ((1, 0.0), (24, 1.0)):
        bulge.value = w
        bulge.keyframe_insert("value", frame=f)
    # Posed positions over time: the rig's origin, each bone's head and tail, and the mesh's top corner point.
    frames = {}
    top = max(range(len(body.data.vertices)), key=lambda i: (body.data.vertices[i].co.z, body.data.vertices[i].co.x, body.data.vertices[i].co.y))
    for f in (1, 12, 24, 36):
        bpy.context.scene.frame_set(f)
        deps = bpy.context.evaluated_depsgraph_get()
        e = body.evaluated_get(deps)
        m = e.to_mesh()
        frames[str(f)] = {
            "rig": list(rig.matrix_world.translation),
            "heads": {b.name: list(rig.matrix_world @ b.head) for b in rig.pose.bones},
            "tails": {b.name: list(rig.matrix_world @ b.tail) for b in rig.pose.bones},
            "top": list(body.matrix_world @ m.vertices[top].co),
            "bulge": bulge.value,
        }
        e.to_mesh_clear()
    bpy.context.scene.frame_set(1)
    oracle("rig", {"frames": frames, "topIndex": top})


def make_instances():
    reset()
    scene = bpy.context.scene
    src = bpy.data.collections.new("Kit")
    scene.collection.children.link(src)
    a = mesh_object("KitCube", cube, src)
    b = mesh_object("KitBall", lambda bm: bmesh.ops.create_icosphere(bm, subdivisions=1, radius=0.5), src)
    b.location = (0, 0, 1.5)
    src.instance_offset = (0, 0, 0)
    for i in range(3):
        e = link(bpy.data.objects.new(f"KitInstance{i}", None))
        e.instance_type = "COLLECTION"
        e.instance_collection = src
        e.location = (3.0 * (i + 1), 0, 0)
        e.rotation_euler = (0, 0, math.radians(30 * i))
    view_layer = bpy.context.view_layer
    view_layer.layer_collection.children["Kit"].exclude = True
    oracle("instances")


def make_curves():
    reset()
    cu = bpy.data.curves.new("Path", "CURVE")
    cu.dimensions = "3D"
    sp = cu.splines.new("BEZIER")
    sp.bezier_points.add(2)
    for i, p in enumerate(sp.bezier_points):
        p.co = (i * 2.0, math.sin(i), 0.0)
        p.handle_left = (i * 2.0 - 0.5, math.sin(i), 0.0)
        p.handle_right = (i * 2.0 + 0.5, math.sin(i), 0.0)
    sp2 = cu.splines.new("NURBS")
    sp2.points.add(3)
    for i, p in enumerate(sp2.points):
        p.co = (i, 2.0, i * 0.5, 1.0)
    sp2.order_u = 3
    sp3 = cu.splines.new("POLY")
    sp3.points.add(2)
    for i, p in enumerate(sp3.points):
        p.co = (i, -2.0, 0.0, 1.0)
    sp3.use_cyclic_u = True
    link(bpy.data.objects.new("Path", cu))
    oracle("curves")


def make_constrained():
    """A deform bone posed only by a constraint from a rotated control bone, as production rigs do."""
    reset()
    arm = bpy.data.armatures.new("Ctrl")
    rig = link(bpy.data.objects.new("CtrlRig", arm))
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    root = arm.edit_bones.new("ROOT")
    root.head, root.tail = (0, 0, 0), (0, 0, 0.5)
    ctrl = arm.edit_bones.new("CTRL")
    ctrl.head, ctrl.tail, ctrl.parent = (0, 0, 0.5), (0, 0, 1.5), root
    ctrl.use_deform = False
    deform = arm.edit_bones.new("DEF")
    deform.head, deform.tail, deform.parent = (0, 0, 0.5), (0, 0, 1.5), root
    bpy.ops.object.mode_set(mode="OBJECT")
    rig.pose.bones["CTRL"].rotation_mode = "XYZ"
    rig.pose.bones["CTRL"].rotation_euler = (math.radians(50), 0, math.radians(20))
    c = rig.pose.bones["DEF"].constraints.new("COPY_TRANSFORMS")
    c.target, c.subtarget = rig, "CTRL"

    def column(bm):
        bmesh.ops.create_cube(bm, size=0.3)
        for v in bm.verts:
            v.co.z = v.co.z + 1.0
    body = mesh_object("Stick", column)
    g0 = body.vertex_groups.new(name="ROOT")
    g1 = body.vertex_groups.new(name="DEF")
    for v in body.data.vertices:
        (g1 if v.co.z > 1.0 else g0).add([v.index], 1.0, "REPLACE")
    body.modifiers.new("Armature", "ARMATURE").object = rig
    deps = bpy.context.evaluated_depsgraph_get()
    m = body.evaluated_get(deps).to_mesh()
    top = max(range(len(m.vertices)), key=lambda i: (m.vertices[i].co.z, m.vertices[i].co.x, m.vertices[i].co.y))
    pts = [list(body.matrix_world @ v.co) for v in m.vertices]
    body.evaluated_get(deps).to_mesh_clear()
    oracle("constrained", {"points": pts, "deformBone": "DEF"})


def make_cards():
    """Shading only seen in stylised scenes: an emission card cut out by its image's alpha, a Transparent-only card, a fog volume."""
    reset()
    img = bpy.data.images.new("card", 4, 4, alpha=True)
    img.pixels = [1.0 if k < 3 else float(i % 2) for i in range(16) for k in range(4)]
    img.pack()

    def material(name, build):
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        nt = m.node_tree
        for n in list(nt.nodes):
            if n.bl_idname != "ShaderNodeOutputMaterial":
                nt.nodes.remove(n)
        build(nt, nt.nodes["Material Output"])
        return m

    def card(nt, out):
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image = img
        em = nt.nodes.new("ShaderNodeEmission")
        tr = nt.nodes.new("ShaderNodeBsdfTransparent")
        mix = nt.nodes.new("ShaderNodeMixShader")
        nt.links.new(tex.outputs["Color"], em.inputs["Color"])
        nt.links.new(tex.outputs["Alpha"], mix.inputs["Fac"])
        nt.links.new(tr.outputs["BSDF"], mix.inputs[1])
        nt.links.new(em.outputs["Emission"], mix.inputs[2])
        nt.links.new(mix.outputs["Shader"], out.inputs["Surface"])

    def clear(nt, out):
        nt.links.new(nt.nodes.new("ShaderNodeBsdfTransparent").outputs["BSDF"], out.inputs["Surface"])

    def fog(nt, out):
        nt.links.new(nt.nodes.new("ShaderNodeVolumePrincipled").outputs["Volume"], out.inputs["Volume"])

    for i, (name, build) in enumerate((("Card", card), ("Clear", clear), ("Fog", fog))):
        ob = mesh_object(name, lambda bm: bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=1.0) if name != "Fog" else cube(bm, 10.0))
        ob.location = (i * 3.0, 0, 0)
        ob.data.materials.append(material(name, build))
    # Hidden with the outliner's eye: stored on the view layer, not the object.
    eye = mesh_object("EyeHidden", cube)
    eye.location = (0, 5, 0)
    eye.hide_set(True)
    oracle("cards")


for fn in (make_basic, make_modifiers, make_rig, make_instances, make_curves, make_constrained, make_cards):
    fn()
