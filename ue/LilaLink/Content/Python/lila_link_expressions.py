"""Builds the Control Rig that runs a character's Lila expressions. Lila's unrealExpressionRig command calls this."""

import json

import unreal

UNIT = "/Script/LilaLinkRig.RigUnit_LilaExpression"
PREFIX = "LilaExpr_"
BEGIN = "/Script/ControlRig.RigUnit_BeginExecution"


def _ensure_rig(path, mesh, errors):
    rig = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if rig is not None:
        return rig, False
    made = unreal.ControlRigBlueprintFactory.create_control_rig_from_skeletal_mesh_or_skeleton(mesh)
    if made is None:
        errors.append("couldn't create a Control Rig from " + mesh.get_path_name())
        return None, False
    made_path = made.get_path_name().split(".")[0]
    if made_path != path:
        unreal.EditorAssetLibrary.save_loaded_asset(made)
        if not unreal.EditorAssetLibrary.rename_asset(made_path, path):
            errors.append("couldn't move the new rig to " + path)
            return made, True
        made = unreal.load_asset(path)
    return made, True


def _begin_pin(controller):
    """The Forwards Solve event's execute output, adding the event when the rig has none (a rig made headless)."""
    graph = controller.get_graph()
    for node in graph.get_nodes():
        if node.get_node_title() == "Forwards Solve":
            for pin in node.get_pins():
                if pin.is_execute_context() and pin.get_direction() == unreal.RigVMPinDirection.OUTPUT:
                    return pin.get_pin_path()
    node = controller.add_unit_node_from_struct_path(BEGIN, "Execute", unreal.Vector2D(0.0, 200.0), "RigUnit_BeginExecution", False)
    return node.get_name() + ".ExecutePin" if node else None


def _mul(a, b):
    return [[sum(a[r][i] * b[i][c] for i in range(3)) for c in range(3)] for r in range(3)]


def _apply(a, v):
    return [sum(a[r][i] * v[i] for i in range(3)) for r in range(3)]


def _t(a):
    return [[a[c][r] for c in range(3)] for r in range(3)]


def _inverse(m):
    a, b, c = m[0]
    d, e, f = m[1]
    g, h, i = m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


def _rows(flat):
    return [flat[0:3], flat[3:6], flat[6:9]]


def _calibrate(hierarchy, bindings):
    """A root bone's frame in Unreal: whatever turns its Lila bind pose into the reference pose the import made."""
    k = _rows(bindings["basis"])
    scale = bindings.get("unitScale", 1.0)
    for b in bindings["inputs"] + [bindings["target"]]:
        if b.get("kind") != "bone" or "bindLocal" not in b:
            continue
        key = unreal.RigElementKey(unreal.RigElementType.BONE, b["name"])
        if not hierarchy.contains(key):
            continue
        tr = hierarchy.get_local_transform(key, True)
        q = tr.rotation
        x, y, z, w = q.x, q.y, q.z, q.w
        rot = [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
               [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
               [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]
        sc = [tr.scale3d.x, tr.scale3d.y, tr.scale3d.z]
        u = [[rot[r][c] * sc[c] for c in range(3)] for r in range(3)]
        a = _mul(_t(k), _mul(u, k))
        ta = [v / scale for v in _apply(_t(k), [tr.translation.x, tr.translation.y, tr.translation.z])]
        bl = _rows(b.pop("bindLocal"))
        tb = b.pop("bindTranslation")
        p = _mul(a, _inverse(bl))
        pt = [ta[i] - v for i, v in enumerate(_apply(p, tb))]
        b["parentRotation"] = [v for row in p for v in row]
        b["parentTranslation"] = pt


def _control_value(hierarchy, value):
    return unreal.RigHierarchy.make_control_value_from_float(float(value))


def build(p):
    errors, warnings = [], []
    mesh = unreal.load_asset(p["skeletalMesh"])
    if mesh is None:
        return {"errors": ["no skeletal mesh at " + p["skeletalMesh"]]}
    rig, created = _ensure_rig(p["rig"], mesh, errors)
    if rig is None:
        return {"errors": errors}

    hierarchy = rig.get_hierarchy()
    hc = rig.get_hierarchy_controller()
    hc.import_bones_from_asset(mesh.get_path_name(), "None", True, True, False, False)
    hc.import_curves_from_asset(mesh.get_path_name(), "None", False, False)
    for name in p.get("curves", []):
        key = unreal.RigElementKey(unreal.RigElementType.CURVE, name)
        if not hierarchy.contains(key):
            hc.add_curve(name, 0.0, False)
    for c in p.get("controls", []):
        key = unreal.RigElementKey(unreal.RigElementType.CONTROL, c["name"])
        if hierarchy.contains(key):
            continue
        settings = unreal.RigControlSettings()
        settings.control_type = unreal.RigControlType.FLOAT
        settings.display_name = c.get("plug", c["name"])
        if "min" in c or "max" in c:
            settings.limit_enabled = [unreal.RigControlLimitEnabled(True, True)]
            settings.minimum_value = unreal.RigHierarchy.make_control_value_from_float(float(c.get("min", -1.0e6)))
            settings.maximum_value = unreal.RigHierarchy.make_control_value_from_float(float(c.get("max", 1.0e6)))
        hc.add_control(c["name"], unreal.RigElementKey(), settings, _control_value(hierarchy, c.get("value", 0.0)), False, False)

    controller = rig.get_controller_by_name("RigVMModel") or rig.get_controller()
    graph = controller.get_graph()
    begin = _begin_pin(controller)
    if begin is None:
        return {"errors": errors + ["the rig has no Forwards Solve event"]}

    # What ran after Forwards Solve before (the user's own graph) runs after the expressions.
    after = []
    for node in list(graph.get_nodes()):
        if node.get_name().startswith(PREFIX):
            continue
        for link in graph.get_links():
            if link.get_source_pin().get_pin_path() == begin:
                target = link.get_target_pin()
                if not target.get_node().get_name().startswith(PREFIX):
                    after.append(target.get_pin_path())
        break
    after = sorted(set(after))
    for node in list(graph.get_nodes()):
        if node.get_name().startswith(PREFIX):
            controller.remove_node(node, False, False)
    for path in after:
        controller.break_link(begin, path, False)

    prev = begin
    y = 200.0
    made = []
    for i, e in enumerate(p["expressions"]):
        name = PREFIX + e["name"]
        node = controller.add_unit_node_from_struct_path(UNIT, "Execute", unreal.Vector2D(400.0 + 360.0 * i, y), name, False)
        if node is None:
            errors.append("couldn't add " + name + " (is the LilaLinkRig module loaded?)")
            continue
        node_name = node.get_name()
        controller.set_pin_default_value(node_name + ".Expression", e.get("text", ""), False, False)
        controller.set_pin_default_value(node_name + ".Program", e["program"], False, False)
        _calibrate(hierarchy, e["bindings"])
        controller.set_pin_default_value(node_name + ".Bindings", json.dumps(e["bindings"]), False, False)
        controller.add_link(prev, node_name + ".ExecutePin", False)
        prev = node_name + ".ExecutePin"
        made.append(node_name)
    for path in after:
        controller.add_link(prev, path, False)

    unreal.BlueprintEditorLibrary.compile_blueprint(rig)
    unreal.EditorAssetLibrary.save_loaded_asset(rig)
    return {"rig": rig.get_path_name().split(".")[0], "created": created, "nodes": made, "errors": errors, "warnings": warnings}


def build_and_print(text):
    try:
        result = build(json.loads(text))
    except Exception as e:  # noqa: BLE001 - reported back to Lila
        result = {"errors": ["{}: {}".format(type(e).__name__, e)]}
    print("LILA_RESULT:" + json.dumps(result))
