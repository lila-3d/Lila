"""Maya viewport rows: 8x and no multisampling, each row the per-redraw time at 8 orbit angles. Run in Maya with OUT set
(the results JSON path), e.g. through a command port."""
import time, json, traceback, statistics
from maya import cmds, mel
res = {}
try:
    p = "modelPanel4"; cam = cmds.modelPanel(p, q=True, camera=True)
    mesh = [m for m in cmds.ls(type="mesh", long=True) if not cmds.getAttr(m + ".intermediateObject")][0]
    node = cmds.listRelatives(mesh, parent=True, fullPath=True)[0]
    g = "hardwareRenderingGlobals"
    cmds.modelEditor(p, e=True, displayAppearance="smoothShaded", wireframeOnShaded=False)
    def pose_ms(n=30):
        cmds.viewFit(cam, node)
        out = []
        for k in range(8):
            if k: cmds.orbit(cam, rotationAngles=(0, 45))
            for _ in range(5): cmds.refresh(force=True, currentView=True)
            t = time.perf_counter()
            for _ in range(n): cmds.refresh(force=True, currentView=True)
            out.append(round((time.perf_counter() - t) * 1000 / n, 3))
        return round(sum(out) / 8, 3), out
    def rows(tag):
        cmds.select(clear=True); cmds.selectMode(object=True)
        cmds.setAttr(g + ".ssaoEnable", 0); res[tag + "shaded"] = pose_ms()
        cmds.setAttr(g + ".ssaoEnable", 1); res[tag + "ao"] = pose_ms()
        cmds.setAttr(g + ".ssaoEnable", 0)
        cmds.modelEditor(p, e=True, wireframeOnShaded=True); res[tag + "wire"] = pose_ms()
        cmds.modelEditor(p, e=True, wireframeOnShaded=False)
        cmds.select(node); res[tag + "selected"] = pose_ms()
        mel.eval("ConvertSelectionToVertices"); res[tag + "vertex"] = pose_ms()
        cmds.select(clear=True); cmds.selectMode(object=True)
    cmds.setAttr(g + ".multiSampleCount", 8)
    for aa in (1, 0, 1):
        cmds.setAttr(g + ".multiSampleEnable", aa)
        rows(("aa8_" if aa else "aa1_") + ("b_" if ("aa8_shaded" in res and aa) else ""))
    res["size"] = [cmds.control(p, q=True, w=True), cmds.control(p, q=True, h=True)]
except Exception:
    res["error"] = traceback.format_exc()
open(OUT, "w").write(json.dumps(res, indent=1))
