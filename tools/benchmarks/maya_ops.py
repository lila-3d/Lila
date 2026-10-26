"""Headless Maya timings: mayapy maya_ops.py <out.json> <test>. One test per process, so a hang can be killed."""
import json, os, sys, time

import maya.standalone

maya.standalone.initialize(name="python")
from maya import cmds

cmds.loadPlugin("objExport", quiet=True)
SHIP = os.environ.get("BENCH_SOUP", "ship_soup.obj")  # make_soup.py output
MANY = os.environ.get("BENCH_MESH", "manyships.obj")
out, test = sys.argv[1], sys.argv[2]
res = {"test": test, "maya": cmds.about(version=True)}


def load(path):
    before = set(cmds.ls(assemblies=True))
    t = time.perf_counter()
    cmds.file(path, i=True, type="OBJ", ignoreVersion=True, options="mo=0;lo=0")
    secs = time.perf_counter() - t
    node = [n for n in cmds.ls(assemblies=True) if n not in before][0]
    return node, secs


def save():
    json.dump(res, open(out, "w"), indent=2)


if test == "import":
    node, res["seconds"] = load(MANY)
    res["faces"] = cmds.polyEvaluate(node, face=True)
elif test == "merge":
    node, res["importSeconds"] = load(SHIP)
    res["vertsBefore"] = cmds.polyEvaluate(node, vertex=True)
    cmds.select(node + ".vtx[*]")
    save()
    t = time.perf_counter()
    cmds.polyMergeVertex(d=0.0001, ch=False)
    res["seconds"] = time.perf_counter() - t
    res["vertsAfter"] = cmds.polyEvaluate(node, vertex=True)
elif test == "subdivide":
    node, res["importSeconds"] = load(MANY)
    save()
    t = time.perf_counter()
    cmds.polySmooth(node, dv=1, ch=False)
    res["seconds"] = time.perf_counter() - t
    res["facesAfter"] = cmds.polyEvaluate(node, face=True)
save()
