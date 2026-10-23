import json, os, unreal

OUT = os.environ["MANNY_OUT"]
os.makedirs(OUT, exist_ok=True)
path = "/Game/Characters/Mannequins/Meshes/SKM_Manny"
skm = unreal.load_asset(path)
unreal.log("MANNY loaded %s" % skm)

skel = skm.skeleton
pose = unreal.AnimPoseExtensions.get_reference_pose(skel)
names = unreal.AnimPoseExtensions.get_bone_names(pose)
bones = []
for n in names:
    w = unreal.AnimPoseExtensions.get_ref_bone_pose(pose, n, unreal.AnimPoseSpaces.WORLD)
    l = unreal.AnimPoseExtensions.get_ref_bone_pose(pose, n, unreal.AnimPoseSpaces.LOCAL)
    def tj(t):
        q = t.rotation
        return {"t": [t.translation.x, t.translation.y, t.translation.z], "q": [q.x, q.y, q.z, q.w], "s": [t.scale3d.x, t.scale3d.y, t.scale3d.z]}
    try:
        parent = str(unreal.AnimPoseExtensions.get_parent_bone_name(pose, n)) if hasattr(unreal.AnimPoseExtensions, "get_parent_bone_name") else ""
    except Exception:
        parent = ""
    bones.append({"name": str(n), "parent": parent, "world": tj(w), "local": tj(l)})
with open(os.path.join(OUT, "SKM_Manny_bones.json"), "w") as f:
    json.dump({"bones": bones}, f, indent=1)
unreal.log("MANNY bones %d" % len(bones))

src = "/Game/Characters/Mannequins/Animations/Manny/MM_Idle"
anim = unreal.EditorAssetLibrary.duplicate_asset(src, "/Game/RefPose") or unreal.load_asset("/Game/RefPose")
for f in (lambda: anim.set_preview_skeletal_mesh(skm), lambda: skel.set_preview_mesh(skm)):
    try:
        f()
    except Exception as e:
        print("MANNY preview", e)
ctl = anim.controller
try:
    ctl.open_bracket(unreal.Text("ref pose"), False)
except Exception as e:
    print("MANNY bracket", e)
try:
    ctl.remove_all_bone_tracks(False)
except Exception as e:
    print("MANNY tracks", e)
try:
    ctl.set_number_of_frames(unreal.FrameNumber(1), False)
except Exception as e:
    print("MANNY frames", e)
try:
    ctl.close_bracket(False)
except Exception as e:
    print("MANNY close", e)
task = unreal.AssetExportTask()
task.object = anim
task.filename = os.path.join(OUT, "SKM_Manny_skeleton.fbx")
task.automated = True
task.prompt = False
task.replace_identical = True
opt = unreal.FbxExportOption()
opt.export_preview_mesh = False
opt.level_of_detail = False
opt.export_morph_targets = False
task.options = opt
ok = unreal.Exporter.run_asset_export_task(task)
print("MANNY anim fbx export %s" % ok)
