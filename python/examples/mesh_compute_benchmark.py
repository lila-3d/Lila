"""Mesh compute benchmark for Lila: gather every vertex of a big mesh into numpy, then run the same per-vertex maths
(tools/benchmarks/mesh_compute.py) three ways: numpy, numpy across worker threads, and pure Python on 100,000
vertices, extrapolated. tools/benchmarks/maya_mesh_compute.py runs the identical maths in Maya.

    lilabatch -b python/examples/mesh_compute_benchmark.py

The mesh comes from LILA_EXAMPLE_MESH, else DEFAULT. LILA_BENCH_DIR overrides where mesh_compute.py is found.
"""

import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

import lila
from lila import ops

DEFAULT = "X:/Tress/Art/Ship/Gen/manyships.obj"
SUBSET = 100_000


def _bench_dir():
    if os.environ.get("LILA_BENCH_DIR"):
        return os.environ["LILA_BENCH_DIR"]
    here = os.path.dirname(os.path.abspath(globals().get("__file__") or "."))
    return os.path.join(os.path.dirname(os.path.dirname(here)), "tools", "benchmarks")


sys.path.insert(0, _bench_dir())
import mesh_compute  # noqa: E402


def timed(fn):
    start = time.perf_counter()
    value = fn()
    return value, time.perf_counter() - start


def gather(meshes):
    arrays = [m.points_array() for m in meshes]
    return arrays[0] if len(arrays) == 1 else np.concatenate(arrays)


def main():
    path = os.environ.get("LILA_EXAMPLE_MESH") or DEFAULT
    names, import_s = timed(lambda: ops.importFile(path))
    meshes = [lila.Node(n) for n in names if lila.Node(n).is_a("mesh")]
    if not meshes:
        raise RuntimeError(f"{path} has no meshes")
    print(f"import {path}: {import_s:.2f} s, {len(meshes)} mesh(es)")

    points, gather_s = timed(lambda: gather(meshes))
    print(f"vertices: {len(points):,}")
    print(f"gather:   {gather_s * 1e6:.1f} us")

    (mean, cov), numpy_s = timed(lambda: mesh_compute.compute(points))
    print(f"numpy:    {numpy_s:.3f} s  {mesh_compute.checksum(mean, cov)}")

    workers = os.cpu_count() or 4
    with ThreadPoolExecutor(max_workers=workers) as pool:
        mesh_compute.compute_threaded(points[: workers * 1024], pool, workers)  # starts the threads outside the timing
        (tmean, tcov), threaded_s = timed(lambda: mesh_compute.compute_threaded(points, pool, workers))
    print(f"threaded: {threaded_s:.3f} s  {mesh_compute.checksum(tmean, tcov)}  ({workers} workers)")

    n = min(SUBSET, len(points))
    subset = points[:n].tolist()
    _, python_s = timed(lambda: mesh_compute.python_partial(subset))
    print(f"python:   {python_s * len(points) / n:.1f} s extrapolated ({python_s:.2f} s for {n:,} vertices)")


main()
