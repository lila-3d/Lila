"""
Headless Blender check of a Lila skeletal FBX against Lila's own evaluation.

    blender --background --factory-startup --python validate_blender.py -- <dcc_skin_ue5.json | dcc_skin_generic.json>

The json (written by the fbxskin test "reference data for external DCC validation") names the FBX and holds Lila's
file-space joint heads and skinned points per frame. Blender converts axes and units on import, so its frame-0
points are first aligned onto Lila's with a best-fit similarity; that one transform must then match every frame.
Prints a json summary line starting with RESULT and exits non-zero on a mismatch.
"""
import json
import sys

import bpy
import numpy as np

TOLERANCE_CM = 0.01


def similarity(src, dst):
    """Scale, rotation and translation taking src points onto dst (Umeyama)."""
    mu_s, mu_d = src.mean(0), dst.mean(0)
    a, b = src - mu_s, dst - mu_d
    u, sig, vt = np.linalg.svd(b.T @ a / len(src))
    d = np.eye(3)
    if np.linalg.det(u @ vt) < 0:
        d[2, 2] = -1
    r = u @ d @ vt
    s = np.trace(np.diag(sig) @ d) / a.var(0).sum()
    return s, r, mu_d - s * r @ mu_s


def main():
    ref = json.load(open(sys.argv[sys.argv.index("--") + 1]))
    bpy.ops.wm.read_factory_settings(use_empty=True)
    importer = "import_scene.fbx"
    bpy.ops.import_scene.fbx(filepath=ref["fbx"])
    scene = bpy.context.scene

    arm = next(o for o in scene.objects if o.type == "ARMATURE")
    mesh = next(o for o in scene.objects if o.type == "MESH")
    action = arm.animation_data.action if arm.animation_data else None
    bones = sorted(b.name for b in arm.data.bones)
    groups = sorted(g.name for g in mesh.vertex_groups)
    max_groups = max(sum(1 for g in v.groups if g.weight > 0) for v in mesh.data.vertices)
    sums = [sum(g.weight for g in v.groups) for v in mesh.data.vertices]

    def blender_frame(f):
        scene.frame_set(int(round(scene.frame_start + f * scene.render.fps / ref["fps"])))
        dg = bpy.context.evaluated_depsgraph_get()
        ev = mesh.evaluated_get(dg)
        m = ev.to_mesh()
        pts = np.array([tuple(ev.matrix_world @ v.co) for v in m.vertices])
        ev.to_mesh_clear()
        heads = {b: np.array(tuple(arm.matrix_world @ arm.pose.bones[b].head)) for b in ref["bones"]}
        return pts, heads

    pts0, _ = blender_frame(0)
    lila0 = np.array(ref["perFrame"][0]["points"])
    s, r, t = similarity(pts0, lila0)
    worst_pt = worst_head = 0.0
    for f, fr in enumerate(ref["perFrame"]):
        pts, heads = blender_frame(f)
        mapped = (s * (r @ pts.T)).T + t
        worst_pt = max(worst_pt, float(np.abs(mapped - np.array(fr["points"])).max()))
        for b in ref["bones"]:
            h = s * r @ heads[b] + t
            worst_head = max(worst_head, float(np.abs(h - np.array(fr["heads"][b])).max()))

    result = {
        "blender": bpy.app.version_string,
        "importer": importer,
        "bones": bones,
        "vertexGroups": groups,
        "maxInfluences": max_groups,
        "weightSumRange": [min(sums), max(sums)],
        "vertices": len(mesh.data.vertices),
        "fps": scene.render.fps,
        "actionRange": list(action.frame_range) if action else None,
        "alignScale": s,
        "worstPointErrorCm": worst_pt,
        "worstHeadErrorCm": worst_head,
    }
    print("RESULT " + json.dumps(result))
    ok = (len(bones) == len(ref["bones"]) and action is not None and len(pts0) == len(lila0)
          and worst_pt < TOLERANCE_CM and worst_head < TOLERANCE_CM)
    sys.exit(0 if ok else 1)


try:
    main()
except SystemExit:
    raise
except Exception:
    import traceback
    traceback.print_exc()
    sys.exit(2)
