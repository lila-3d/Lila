"""Headless Lila timings, run with lilabatch -b. One test per process, matching maya_ops.py. Args via LILA_BENCH=<out>|<test>."""
import json, os, time

from lila import ops

SHIP = os.environ.get("BENCH_SOUP", "ship_soup.obj")  # make_soup.py output
MANY = os.environ.get("BENCH_MESH", "manyships.obj")
out, test = os.environ["LILA_BENCH"].split("|")
res = {"test": test}


def load(path):
    t = time.perf_counter()
    node = ops.importFile(path)[0]
    return node, time.perf_counter() - t


if test == "import":
    node, res["seconds"] = load(MANY)
    res["faces"] = ops.meshInfo(node)["faces"]
elif test == "merge":
    node, res["importSeconds"] = load(SHIP)
    res["vertsBefore"] = ops.meshInfo(node)["points"]
    ops.select(node)
    t = time.perf_counter()
    ops.polyMerge(distance=0.0001)
    res["seconds"] = time.perf_counter() - t
    res["vertsAfter"] = ops.meshInfo(node)["points"]
elif test == "subdivide":
    node, res["importSeconds"] = load(MANY)
    t = time.perf_counter()
    ops.polySubdivide(node, levels=1)
    res["seconds"] = time.perf_counter() - t
    res["facesAfter"] = ops.meshInfo(node)["faces"]
res["node"] = node
json.dump(res, open(out, "w"), indent=2)
