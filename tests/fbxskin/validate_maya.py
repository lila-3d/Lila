"""
Headless Maya check of a Lila skeletal FBX against Lila's own evaluation.

    mayapy validate_maya.py <dcc_skin_ue5.json | dcc_skin_generic.json>

Same reference json and alignment as validate_blender.py: Maya's frame-0 skinned points are fitted onto Lila's
with a similarity (Maya converts the file's axes on import), then every frame must match through that one fit.
Prints a json summary line starting with RESULT and exits non-zero on a mismatch.
"""
import json
import os
import sys

# Keep the user's startup scripts and modules out of a headless check.
os.environ["MAYA_SKIP_USERSETUP_PY"] = "1"
os.environ["MAYA_NO_HOME"] = "1"

import maya.standalone

TOLERANCE_CM = 0.01


def main():
    maya.standalone.initialize(name="python")
    import maya.cmds as cmds
    import maya.mel as mel
    import numpy as np

    def similarity(src, dst):
        mu_s, mu_d = src.mean(0), dst.mean(0)
        a, b = src - mu_s, dst - mu_d
        u, sig, vt = np.linalg.svd(b.T @ a / len(src))
        d = np.eye(3)
        if np.linalg.det(u @ vt) < 0:
            d[2, 2] = -1
        r = u @ d @ vt
        s = np.trace(np.diag(sig) @ d) / a.var(0).sum()
        return s, r, mu_d - s * r @ mu_s

    ref = json.load(open(sys.argv[1]))
    cmds.loadPlugin("fbxmaya", quiet=True)
    mel.eval("FBXResetImport")
    mel.eval("FBXImportMode -v add")
    mel.eval("FBXImportFillTimeline -v true")
    mel.eval('FBXImport -f "{}"'.format(ref["fbx"]))

    joints = sorted(j.split("|")[-1] for j in cmds.ls(type="joint"))
    clusters = cmds.ls(type="skinCluster")
    sc = clusters[0]
    influences = sorted(cmds.skinCluster(sc, q=True, inf=True))
    mesh = cmds.listRelatives(cmds.skinCluster(sc, q=True, g=True)[0], parent=True)[0]
    nverts = cmds.polyEvaluate(mesh, v=True)
    weights = [cmds.skinPercent(sc, "{}.vtx[{}]".format(mesh, v), q=True, v=True) for v in range(nverts)]
    max_inf = max(sum(1 for w in ws if w > 1e-6) for ws in weights)
    orient_b = list(cmds.getAttr("bone_b.jointOrient")[0])
    fps_unit = cmds.currentUnit(q=True, time=True)
    start, end = cmds.playbackOptions(q=True, min=True), cmds.playbackOptions(q=True, max=True)
    fps = {"film": 24, "ntsc": 30, "pal": 25, "game": 15, "show": 48, "palf": 50, "ntscf": 60}.get(fps_unit)
    if fps is None and fps_unit.endswith("fps"):
        fps = float(fps_unit[:-3])

    def frame(f):
        cmds.currentTime(start + f * fps / ref["fps"])
        pts = np.array(cmds.xform(mesh + ".vtx[*]", q=True, ws=True, t=True)).reshape(-1, 3)
        heads = {b: np.array(cmds.xform(b, q=True, ws=True, t=True)) for b in ref["bones"]}
        return pts, heads

    pts0, _ = frame(0)
    lila0 = np.array(ref["perFrame"][0]["points"])
    s, r, t = similarity(pts0, lila0)
    worst_pt = worst_head = 0.0
    for f, fr in enumerate(ref["perFrame"]):
        pts, heads = frame(f)
        mapped = (s * (r @ pts.T)).T + t
        worst_pt = max(worst_pt, float(np.abs(mapped - np.array(fr["points"])).max()))
        for b in ref["bones"]:
            h = s * r @ heads[b] + t
            worst_head = max(worst_head, float(np.abs(h - np.array(fr["heads"][b])).max()))

    result = {
        "maya": cmds.about(version=True),
        "joints": joints,
        "skinClusters": clusters,
        "influences": influences,
        "maxInfluences": max_inf,
        "vertices": nverts,
        "boneBJointOrient": orient_b,
        "timeUnit": fps_unit,
        "playback": [start, end],
        "alignScale": float(s),
        "worstPointErrorCm": worst_pt,
        "worstHeadErrorCm": worst_head,
    }
    print("RESULT " + json.dumps(result))
    ok = len(joints) == len(ref["bones"]) and len(clusters) == 1 and worst_pt < TOLERANCE_CM and worst_head < TOLERANCE_CM
    return 0 if ok else 1


if __name__ == "__main__":
    code = 2
    try:
        code = main()
    except Exception:
        import traceback
        traceback.print_exc()
    sys.stdout.flush()
    maya.standalone.uninitialize()
    os._exit(code)
