"""Turns the CMU (ASF/AMC) and ACCAD (BVH) mocap into canonical joint positions for training the auto-poser.

Writes an .npz of every sampled frame as POINTS world positions in cm (Y up), with the clip each came from. Only
the datasets in DATA-PROVENANCE.md go in.

    python -I prepare.py X:/LilaData/mocap X:/LilaData/autopose/poses.npz
"""

import os
import re
import sys

import numpy as np

# The canonical points, named after the UE5 Manny joints they land on.
POINTS = [
    "pelvis", "spine_02", "spine_04", "neck_01", "head",
    "upperarm_l", "lowerarm_l", "hand_l", "thigh_l", "calf_l", "foot_l", "ball_l",
    "upperarm_r", "lowerarm_r", "hand_r", "thigh_r", "calf_r", "foot_r", "ball_r",
]

# Where each point sits: ("start", bone) is the bone's start, ("end", bone) its end.
CMU_POINTS = {
    "pelvis": ("start", "root"), "spine_02": ("end", "lowerback"), "spine_04": ("end", "upperback"),
    "neck_01": ("end", "thorax"), "head": ("end", "upperneck"),
}
for s in "lr":
    CMU_POINTS.update({
        f"upperarm_{s}": ("end", f"{s}clavicle"), f"lowerarm_{s}": ("end", f"{s}humerus"), f"hand_{s}": ("end", f"{s}radius"),
        f"thigh_{s}": ("end", f"{s}hipjoint"), f"calf_{s}": ("end", f"{s}femur"), f"foot_{s}": ("end", f"{s}tibia"),
        f"ball_{s}": ("end", f"{s}foot"),
    })

BVH_POINTS = {"pelvis": "Hips", "spine_02": "Spine", "spine_04": "Spine1", "neck_01": "Neck", "head": "Head"}
for s, side in (("l", "Left"), ("r", "Right")):
    BVH_POINTS.update({
        f"upperarm_{s}": f"{side}Arm", f"lowerarm_{s}": f"{side}ForeArm", f"hand_{s}": f"{side}Hand",
        f"thigh_{s}": f"{side}UpLeg", f"calf_{s}": f"{side}Leg", f"foot_{s}": f"{side}Foot", f"ball_{s}": f"{side}ToeBase",
    })

CMU_CM = 2.54 / 0.45  # ASF length units to cm
CMU_STEP = 4  # 120 fps to 30


def rot(axis, deg):
    """Rotation matrices about one axis for an array of angles in degrees."""
    a = np.radians(np.asarray(deg, dtype=np.float64))
    c, s = np.cos(a), np.sin(a)
    m = np.zeros(a.shape + (3, 3))
    i, j = {"x": (1, 2), "y": (2, 0), "z": (0, 1)}[axis]
    k = 3 - i - j
    m[..., k, k] = 1.0
    m[..., i, i] = c
    m[..., j, j] = c
    m[..., i, j] = -s
    m[..., j, i] = s
    return m


def euler(order, angles):
    """Euler angles applied in `order` (the first is innermost), as matrices: R = R[order[-1]] ... R[order[0]]."""
    m = np.broadcast_to(np.eye(3), np.asarray(angles[0]).shape + (3, 3)).copy()
    for axis, a in zip(order, angles):
        m = rot(axis, a) @ m
    return m


# CMU ---------------------------------------------------------------------------------------------------------------

def read_asf(path):
    text = open(path, encoding="latin-1").read()
    bones = {"root": {"dir": np.zeros(3), "len": 0.0, "axis": np.eye(3), "dof": ["tx", "ty", "tz", "rx", "ry", "rz"]}}
    root_order = re.search(r":root.*?order\s+([^\n]+)", text, re.S).group(1).split()
    root_axis = re.search(r":root.*?axis\s+(\w+)", text, re.S).group(1).lower()
    bones["root"]["dof"] = [d.lower() for d in root_order]
    bones["root"]["axis_order"] = root_axis
    for block in re.findall(r"begin(.*?)end", text.split(":bonedata")[1].split(":hierarchy")[0], re.S):
        name = re.search(r"name\s+(\S+)", block).group(1)
        direction = np.array([float(v) for v in re.search(r"direction\s+(\S+)\s+(\S+)\s+(\S+)", block).groups()])
        length = float(re.search(r"length\s+(\S+)", block).group(1))
        ax = re.search(r"axis\s+(\S+)\s+(\S+)\s+(\S+)\s+(\w+)", block)
        order = ax.group(4).lower()
        axis = euler(order, [np.float64(ax.group(1 + "xyz".index(o))) for o in order])
        dof = re.search(r"dof\s+([^\n]+)", block)
        bones[name] = {"dir": direction, "len": length, "axis": axis, "dof": dof.group(1).split() if dof else []}
    parents = {}
    for line in text.split(":hierarchy")[1].split("begin")[1].split("end")[0].strip().splitlines():
        names = line.split()
        for child in names[1:]:
            parents[child] = names[0]
    return bones, parents


def read_amc(path, bones):
    frames, cur = [], None
    for line in open(path, encoding="latin-1"):
        line = line.strip()
        if not line or line[0] in "#:":
            continue
        if line.isdigit():
            cur = {}
            frames.append(cur)
        elif cur is not None:
            parts = line.split()
            cur[parts[0]] = np.array([float(v) for v in parts[1:]])
    return frames


def cmu_clip(asf, amc):
    bones, parents = read_asf(asf)
    frames = read_amc(amc, bones)[::CMU_STEP]
    if not frames:
        return None
    n = len(frames)
    order = ["root"]
    for b in order:
        order += [c for c, p in parents.items() if p == b]
    start, end, mat = {}, {}, {}
    for b in order:
        info = bones[b]
        dof = info["dof"]
        values = np.stack([f.get(b, np.zeros(len(dof))) for f in frames]) if dof else np.zeros((n, 0))
        rx = [values[:, dof.index(d)] if d in dof else np.zeros(n) for d in ("rx", "ry", "rz")]
        if b == "root":
            local = euler(info.get("axis_order", "xyz"), [rx["xyz".index(o)] for o in info.get("axis_order", "xyz")])
            mat[b] = local
            start[b] = end[b] = np.stack([values[:, dof.index(t)] for t in ("tx", "ty", "tz")], axis=1)
            continue
        local = euler("xyz", rx)
        c = info["axis"]
        mat[b] = mat[parents[b]] @ c @ local @ c.T
        start[b] = end[parents[b]]
        end[b] = start[b] + info["len"] * (mat[b] @ info["dir"])
    pts = np.stack([(start if where == "start" else end)[bone] for where, bone in (CMU_POINTS[p] for p in POINTS)], axis=1)
    return pts * CMU_CM


# BVH ---------------------------------------------------------------------------------------------------------------

def bvh_clip(path):
    tokens = open(path, encoding="latin-1").read().split()
    joints, stack, i = [], [], 0
    while tokens[i] != "MOTION":
        t = tokens[i]
        if t in ("ROOT", "JOINT"):
            joints.append({"name": tokens[i + 1], "parent": stack[-1] if stack else -1, "offset": None, "channels": []})
            i += 2
        elif t == "End":
            joints.append({"name": None, "parent": stack[-1], "offset": None, "channels": []})
            i += 2
        elif t == "{":
            stack.append(len(joints) - 1)
            i += 1
        elif t == "}":
            stack.pop()
            i += 1
        elif t == "OFFSET":
            joints[stack[-1]]["offset"] = np.array([float(v) for v in tokens[i + 1:i + 4]])
            i += 4
        elif t == "CHANNELS":
            count = int(tokens[i + 1])
            joints[stack[-1]]["channels"] = tokens[i + 2:i + 2 + count]
            i += 2 + count
        else:
            i += 1
    frames = int(tokens[i + 2])
    frame_time = float(tokens[i + 5])
    data = np.array(tokens[i + 6:], dtype=np.float64)
    width = sum(len(j["channels"]) for j in joints)
    data = data[: frames * width].reshape(frames, width)
    step = max(1, round((1.0 / 30.0) / frame_time))
    data = data[::step]
    n = len(data)
    col = 0
    pos, mat = [], []
    for j in joints:
        ch = j["channels"]
        values = data[:, col:col + len(ch)]
        col += len(ch)
        t = np.zeros((n, 3)) + j["offset"]
        for k, c in enumerate(ch):
            if c.endswith("position"):
                t[:, "XYZ".index(c[0])] = values[:, k]
        rots = [(c[0].lower(), values[:, k]) for k, c in enumerate(ch) if c.endswith("rotation")]
        local = np.broadcast_to(np.eye(3), (n, 3, 3)).copy()
        for axis, a in rots:
            local = local @ rot(axis, a)
        if j["parent"] < 0:
            pos.append(t)
            mat.append(local)
        else:
            p = j["parent"]
            pos.append(pos[p] + np.einsum("nij,nj->ni", mat[p], t))
            mat.append(mat[p] @ local)
    by_name = {j["name"]: k for k, j in enumerate(joints) if j["name"]}
    if not all(BVH_POINTS[p] in by_name for p in POINTS):
        return None
    # ACCAD's BVH are in cm already.
    return np.stack([pos[by_name[BVH_POINTS[p]]] for p in POINTS], axis=1)


# -------------------------------------------------------------------------------------------------------------------

def main(root, out):
    clips, names = [], []
    for subject in sorted(os.listdir(os.path.join(root, "cmu", "raw", "all_asfamc", "subjects"))):
        folder = os.path.join(root, "cmu", "raw", "all_asfamc", "subjects", subject)
        asf = [f for f in os.listdir(folder) if f.endswith(".asf")]
        if not asf:
            continue
        for amc in sorted(f for f in os.listdir(folder) if f.endswith(".amc")):
            try:
                pts = cmu_clip(os.path.join(folder, asf[0]), os.path.join(folder, amc))
            except Exception as e:  # a malformed file costs one clip, not the run
                print("skip", amc, e)
                continue
            if pts is not None and len(pts):
                clips.append(pts.astype(np.float32))
                names.append("cmu/" + amc)
        print("cmu", subject, len(clips), flush=True)
    for dirpath, _, files in os.walk(os.path.join(root, "accad", "raw")):
        for f in sorted(files):
            if f.lower().endswith(".bvh"):
                try:
                    pts = bvh_clip(os.path.join(dirpath, f))
                except Exception as e:
                    print("skip", f, e)
                    continue
                if pts is not None and len(pts):
                    clips.append(pts.astype(np.float32))
                    names.append("accad/" + f)
    print("accad done", len(clips), flush=True)
    clip_of = np.concatenate([np.full(len(c), i, np.int32) for i, c in enumerate(clips)])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    np.savez_compressed(out, points=np.concatenate(clips), clip=clip_of, names=np.array(names), joints=np.array(POINTS))
    print("frames", len(clip_of), "clips", len(clips))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
