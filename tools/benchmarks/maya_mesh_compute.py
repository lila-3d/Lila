"""Mesh compute benchmark for Maya: the same gather and maths as Lila's python/examples/mesh_compute_benchmark.py,
with the maths imported from mesh_compute.py beside this file so both apps time identical code.

    mayapy tools/benchmarks/maya_mesh_compute.py
    # or in Maya's script editor (Python): exec(open(r"X:/Lila/tools/benchmarks/maya_mesh_compute.py").read())

Measures the meshes already in the scene; with none, imports MAYA_BENCH_MESH (else DEFAULT) as OBJ first. Needs numpy
in Maya's Python (mayapy -m pip install numpy). MAYA_BENCH_DIR overrides where mesh_compute.py is found.
"""

import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

DEFAULT = "X:/Tress/Art/Ship/Gen/manyships.obj"
SUBSET = 100_000

if os.path.basename(sys.executable).lower().startswith("mayapy"):
    import maya.standalone

    maya.standalone.initialize(name="python")

import numpy as np
import maya.api.OpenMaya as om
from maya import cmds


def _bench_dir():
    if os.environ.get("MAYA_BENCH_DIR"):
        return os.environ["MAYA_BENCH_DIR"]
    if globals().get("__file__"):
        return os.path.dirname(os.path.abspath(__file__))
    return "X:/Lila/tools/benchmarks"


sys.path.insert(0, _bench_dir())
import mesh_compute  # noqa: E402


def timed(fn):
    start = time.perf_counter()
    value = fn()
    return value, time.perf_counter() - start


def mesh_paths():
    out = []
    it = om.MItDependencyNodes(om.MFn.kMesh)
    while not it.isDone():
        fn = om.MFnDagNode(it.thisNode())
        if not fn.isIntermediateObject:
            out.append(fn.getPath())
        it.next()
    return out


def _raw_points(path):
    """API 1 MFnMesh.getRawPoints: a pointer to Maya's own float32 xyz buffer, read through ctypes without a per-point
    Python object. None when that route isn't available."""
    try:
        import ctypes

        import maya.OpenMaya as om1

        sel = om1.MSelectionList()
        sel.add(path.fullPathName())
        dag = om1.MDagPath()
        sel.getDagPath(0, dag)
        fn = om1.MFnMesh(dag)
        n = fn.numVertices()
        ptr = fn.getRawPoints()
        buf = (ctypes.c_float * (n * 3)).from_address(int(ptr))
        return np.ctypeslib.as_array(buf).reshape(n, 3).astype(np.float64)
    except Exception:
        return None


def _api2_points(path):
    """API 2 MFnMesh.getPoints, then numpy over the MPointArray: (n, 4) homogeneous, w dropped."""
    pts = om.MFnMesh(path).getPoints(om.MSpace.kObject)
    return np.array(pts, dtype=np.float64)[:, :3]


def gather(paths, route):
    arrays = [route(p) for p in paths]
    return arrays[0] if len(arrays) == 1 else np.concatenate(arrays)


def main():
    paths = mesh_paths()
    if not paths:
        path = os.environ.get("MAYA_BENCH_MESH") or DEFAULT
        cmds.loadPlugin("objExport", quiet=True)
        _, import_s = timed(lambda: cmds.file(path, i=True, type="OBJ", ignoreVersion=True, options="mo=1"))
        paths = mesh_paths()
        print(f"import {path}: {import_s:.2f} s, {len(paths)} mesh(es)")

    route, name = _api2_points, "MFnMesh.getPoints + np.array"
    if _raw_points(paths[0]) is not None:
        route, name = (lambda p: _raw_points(p)), "MFnMesh.getRawPoints + ctypes"
    points, gather_s = timed(lambda: gather(paths, route))
    print(f"vertices: {len(points):,}")
    print(f"gather:   {gather_s:.4f} s ({name})")
    if route is not _api2_points:
        _, api2_s = timed(lambda: gather(paths, _api2_points))
        print(f"gather:   {api2_s:.4f} s (MFnMesh.getPoints + np.array)")

    (mean, cov), numpy_s = timed(lambda: mesh_compute.compute(points))
    print(f"numpy:    {numpy_s:.3f} s  {mesh_compute.checksum(mean, cov)}")

    workers = os.cpu_count() or 4
    with ThreadPoolExecutor(max_workers=workers) as pool:
        mesh_compute.compute_threaded(points[: workers * 1024], pool, workers)
        (tmean, tcov), threaded_s = timed(lambda: mesh_compute.compute_threaded(points, pool, workers))
    print(f"threaded: {threaded_s:.3f} s  {mesh_compute.checksum(tmean, tcov)}  ({workers} workers)")

    n = min(SUBSET, len(points))
    subset = points[:n].tolist()
    _, python_s = timed(lambda: mesh_compute.python_partial(subset))
    print(f"python:   {python_s * len(points) / n:.1f} s extrapolated ({python_s:.2f} s for {n:,} vertices)")


main()
