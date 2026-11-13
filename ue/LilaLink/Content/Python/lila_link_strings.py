"""Builds the Control Rig that poses a character the way Lila's Strings do. Lila's unrealStringsRig command calls this."""

import json

import unreal

from lila_link_expressions import _begin_pin, _ensure_rig

UNIT = "/Script/LilaLinkRig.RigUnit_LilaStrings"
PREFIX = "LilaStrings_"
CONTROL_PREFIX = "LS_"
# Lila's mode names in ELilaStringsMode's order: a control's Mode channel holds the index.
MODES = ["default", "free", "forward", "forwardAll", "backward", "backwardAll", "bidirectional", "all", "ik", "fullBody"]
MODE_CHANNEL = "Mode"
ISOLATE_CHANNEL = "Isolate Rotation"


def _add_control(hierarchy, hc, bone):
    """A transform control named after the bone, where the bone is at rest, so dragging it poses the bone."""
    name = CONTROL_PREFIX + bone
    key = unreal.RigElementKey(unreal.RigElementType.CONTROL, name)
    if hierarchy.contains(key):
        return name
    bone_key = unreal.RigElementKey(unreal.RigElementType.BONE, bone)
    if not hierarchy.contains(bone_key):
        return None
    settings = unreal.RigControlSettings()
    settings.control_type = unreal.RigControlType.EULER_TRANSFORM
    settings.display_name = bone
    settings.shape_name = "Sphere_Thick"
    rest = hierarchy.get_global_transform(bone_key, True)
    value = unreal.RigHierarchy.make_control_value_from_euler_transform(unreal.EulerTransform(rest.translation, rest.rotation.rotator(), rest.scale3d))
    hc.add_control(name, unreal.RigElementKey(), settings, value, False, False)
    return name


def _channel(hierarchy, hc, control, name, settings, value):
    """The control's animation channel called name, added when missing, set to value (initial and current)."""
    parent = unreal.RigElementKey(unreal.RigElementType.CONTROL, control)
    key = None
    for child in hierarchy.get_children(parent):
        if child.type == unreal.RigElementType.CONTROL:
            settings_of = hierarchy.get_control_settings(child)
            if settings_of.animation_type == unreal.RigControlAnimationType.ANIMATION_CHANNEL and str(settings_of.display_name) == name:
                key = child
    if key is None:
        settings.display_name = name
        key = hc.add_animation_channel(name, parent, settings, False, False)
    for kind in (unreal.RigControlValueType.INITIAL, unreal.RigControlValueType.CURRENT):
        hierarchy.set_control_value(key, value, kind)
    return key


def _add_channels(hierarchy, hc, control, mode, isolate):
    """A Mode channel (Lila's modes as a dropdown) and an Isolate Rotation channel on the control, which its unit reads."""
    settings = unreal.RigControlSettings()
    settings.control_type = unreal.RigControlType.INTEGER
    channel = _channel(hierarchy, hc, control, MODE_CHANNEL, settings, unreal.RigHierarchy.make_control_value_from_int(MODES.index(mode)))
    unreal.LilaStringsLibrary.set_mode_enum(hierarchy, channel)
    settings = unreal.RigControlSettings()
    settings.control_type = unreal.RigControlType.BOOL
    _channel(hierarchy, hc, control, ISOLATE_CHANNEL, settings, unreal.RigHierarchy.make_control_value_from_bool(isolate))


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

    controller = rig.get_controller_by_name("RigVMModel") or rig.get_controller()
    graph = controller.get_graph()
    begin = _begin_pin(controller)
    if begin is None:
        return {"errors": errors + ["the rig has no Forwards Solve event"]}

    # What ran after Forwards Solve before (the user's own graph) runs after the Strings units.
    after = sorted({link.get_target_pin().get_pin_path() for link in graph.get_links()
                    if link.get_source_pin().get_pin_path() == begin and not link.get_target_pin().get_node().get_name().startswith(PREFIX)})
    for node in list(graph.get_nodes()):
        if node.get_name().startswith(PREFIX):
            controller.remove_node(node, False, False)
    for path in after:
        controller.break_link(begin, path, False)

    web = json.dumps(p["ruleWeb"])
    mode = p.get("mode", "fullBody")
    if mode not in MODES:
        return {"errors": errors + ["no Strings mode " + mode + " (" + ", ".join(MODES) + ")"]}
    isolate = bool(p.get("isolate", False))
    prev = begin
    made = []
    for i, bone in enumerate(p.get("controls", [])):
        control = _add_control(hierarchy, hc, bone)
        if control is None:
            warnings.append("no bone named " + bone + " on " + mesh.get_name())
            continue
        unreal.LilaStringsLibrary.place_control_at_bone(hierarchy, control, bone)
        _add_channels(hierarchy, hc, control, mode, isolate)
        node = controller.add_unit_node_from_struct_path(UNIT, "Execute", unreal.Vector2D(400.0 + 360.0 * i, 200.0), PREFIX + bone, False)
        if node is None:
            errors.append("couldn't add a Lila Strings unit (is the LilaLinkRig module loaded?)")
            break
        name = node.get_name()
        controller.set_pin_default_value(name + ".RuleWeb", web, False, False)
        controller.set_pin_default_value(name + ".Bone", bone, False, False)
        controller.set_pin_default_value(name + ".Control", control, False, False)
        controller.set_pin_default_value(name + ".Mode", "IK" if mode == "ik" else mode[0].upper() + mode[1:], False, False)
        controller.set_pin_default_value(name + ".bIsolateRotation", "True" if isolate else "False", False, False)
        controller.add_link(prev, name + ".ExecutePin", False)
        prev = name + ".ExecutePin"
        made.append(name)
    for path in after:
        controller.add_link(prev, path, False)

    unreal.BlueprintEditorLibrary.compile_blueprint(rig)
    unreal.EditorAssetLibrary.save_loaded_asset(rig)
    return {"rig": rig.get_path_name().split(".")[0], "created": created, "nodes": made, "mode": mode, "isolate": isolate,
            "errors": errors, "warnings": warnings}


def build_and_print(text):
    try:
        result = build(json.loads(text))
    except Exception as e:  # noqa: BLE001 - reported back to Lila
        result = {"errors": ["{}: {}".format(type(e).__name__, e)]}
    print("LILA_RESULT:" + json.dumps(result))
