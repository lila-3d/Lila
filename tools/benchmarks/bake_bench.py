"""Bakes bake_scene.py's scene with the project's default maps and prints one JSON line (seconds, peak memory, samples,
per-stage timings). Run with lilabatch --batch, one bake per process so peak memory is the bake's own.

Env: BENCH_DIR (bake_bench.lila's folder, default the temp dir), BENCH_RES (1024), BENCH_SS (1), BENCH_SETUP (permesh:
bakeGroupPerMesh with aoScope all, or one: a single self group of every mesh), BENCH_MAPS (comma list, default the
project's), BENCH_BACKEND (gpu), BENCH_OUT (also write the result there).
"""
import ctypes, json, os, tempfile, time
from ctypes import wintypes

from lila import ops

DIR = os.environ.get("BENCH_DIR", tempfile.gettempdir())
RES = int(os.environ.get("BENCH_RES", "1024"))
SS = int(os.environ.get("BENCH_SS", "1"))
SETUP = os.environ.get("BENCH_SETUP", "permesh")
MAPS = [m for m in os.environ.get("BENCH_MAPS", "").split(",") if m]
BACKEND = os.environ.get("BENCH_BACKEND", "gpu")


def peak_gb():
    """Peak working set and peak commit of this process, GB (Windows)."""

    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
            (n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
                                           "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]

    c = Counters()
    c.cb = ctypes.sizeof(c)
    k32 = ctypes.WinDLL("kernel32")
    k32.GetCurrentProcess.restype = wintypes.HANDLE
    psapi = ctypes.WinDLL("psapi")
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(c), c.cb)
    return round(c.PeakWorkingSetSize / 2**30, 2), round(c.PeakPagefileUsage / 2**30, 2)


ops.openScene(os.path.join(DIR, "bake_bench.lila"), force=True)
meshes = list(ops.ls(type="mesh"))
p = ops.createBakeProject(name="Bench", folder=os.path.join(DIR, "bake_bench_%s_%d_ss%d" % (SETUP, RES, SS)))
ops.setBakeOutput(project=p, resolution=RES, supersampling=SS)
if SETUP == "permesh":
    for g in ops.bakeGroupPerMesh(*meshes, project=p, preset="none"):
        ops.bakeGroup(g, project=p, aoScope="all")
else:
    ops.bakeGroup("All", project=p, lows=meshes, self=True, aoScope="group")
t0 = time.time()
r = ops.bake(project=p, backend=BACKEND, maps=MAPS) if MAPS else ops.bake(project=p, backend=BACKEND)
wall = time.time() - t0
ws, commit = peak_gb()
groups = r["stats"]["groups"]
result = {"setup": SETUP, "res": RES, "ss": SS, "maps": MAPS, "meshes": len(meshes), "lowTriangles": sum(g["lowTriangles"] for g in groups),
          "outputs": len(r["outputs"]), "wall": round(wall, 2), "seconds": round(r["seconds"], 2), "peakWorkingSetGB": ws, "peakCommitGB": commit,
          "samples": sum(g["samples"] for g in groups), "misses": r["stats"]["misses"], "backendMode": r["backendMode"],
          "timings": {k: round(v, 3) for k, v in sorted(r["timings"].items(), key=lambda kv: -kv[1])}}
print(json.dumps(result))
if os.environ.get("BENCH_OUT"):
    with open(os.environ["BENCH_OUT"], "w") as f:
        json.dump(result, f, indent=1)
