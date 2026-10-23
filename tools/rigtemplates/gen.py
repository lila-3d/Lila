from lila import cmds
import json
cmds.newScene()
print(cmds.listExtensions() if hasattr(cmds, 'listExtensions') else '')
cmds.importFbx(r"X:\Lila\tests\data\rig\SKM_Manny_skeleton.fbx", animation=False)
r = cmds.templateFromSkeleton("root", id="UE5_Manny", name="UE5 Manny",
    description="Unreal Engine 5 mannequin (SKM_Manny, SK_Mannequin skeleton).",
    file=r"X:\Lila\extensions\RigTemplates\resources\templates\UE5_Manny.lilarig",
    source={"asset": "/Game/Characters/Mannequins/Meshes/SKM_Manny", "engine": "Unreal Engine 5.8",
            "export": "UnrealEditor-Cmd -run=pythonscript, reference pose as FBX, imported with importFbx",
            "axes": "Lila Y up, right handed, cm, as importFbx converts UE5 files: UE (x, y, z) is Lila (-y, z, x), and a UE bone's X (aim), Y and Z axes are its Lila local Z, -X and Y. Checked against UE's reference pose to 1e-13."})
print(r)
b = cmds.buildRigSkeleton("UE5_Manny", prefix="b_")
worst = 0; worstR = 0
for j in b["joints"]:
    src = j[2:]
    m1 = cmds.getAttr(src + ".worldMatrix"); m2 = cmds.getAttr(j + ".worldMatrix")
    worst = max(worst, max(abs(m1[i]-m2[i]) for i in (12,13,14)))
    worstR = max(worstR, max(abs(m1[i]-m2[i]) for i in range(12) if i % 4 != 3))
print("joints", len(b["joints"]), "pos err", worst, "rot err", worstR)
