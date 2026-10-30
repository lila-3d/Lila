"""Compare the vertex positions of two meshes, index by index, and report every vertex that deviates by more than a
threshold, down to the last bit if THRESHOLD is 0. Both meshes need the same point order (a copy, an export round
trip, a reimport); a vertex count mismatch is reported and the shared range compared.

In the Script Editor, select two meshes and run it. Headless:

    set LILA_COMPARE_A=meshA
    set LILA_COMPARE_B=meshB
    lilabatch -b python/examples/compare_mesh_points.py

LILA_COMPARE_FILE opens a scene first. LILA_COMPARE_THRESHOLD overrides THRESHOLD.
"""

import os

import numpy as np

import lila
from lila import ops

THRESHOLD = 1e-8  # |d| above this is reported; nonzero differences below it are only counted
WORLD = False     # True compares world space, so transform differences count too
BASE = False      # True reads the base mesh under modifiers


def compare(a, b, threshold=THRESHOLD, world=WORLD, base=BASE):
    """Prints each vertex of b that moved more than threshold from a, then a summary. Returns the deviating indices
    and the (N, 3) deltas."""
    pa = a.points_array(base=base, world=world)
    pb = b.points_array(base=base, world=world)
    print(f"{a.path}: {len(pa)} verts   {b.path}: {len(pb)} verts   space={'world' if world else 'object'}")
    if len(pa) != len(pb):
        print(f"VERTEX COUNT MISMATCH: {len(pa)} vs {len(pb)}; comparing the first {min(len(pa), len(pb))}")
    n = min(len(pa), len(pb))
    d = pb[:n] - pa[:n]
    dist = np.linalg.norm(d, axis=1)
    bad = np.nonzero(dist > threshold)[0]
    below = int(np.count_nonzero(np.any(d != 0.0, axis=1) & (dist <= threshold)))

    for i in bad:
        print(f"vtx[{i}]  A={pa[i].tolist()!r}  B={pb[i].tolist()!r}  "
              f"d=({d[i, 0]:.17g}, {d[i, 1]:.17g}, {d[i, 2]:.17g})  |d|={dist[i]:.17g}")

    print(f"\n{len(bad)} of {n} vertices deviate by more than {threshold:g}")
    if below:
        print(f"{below} more differ by a nonzero amount at or below the threshold")
    if len(bad):
        worst = bad[np.argmax(dist[bad])]
        print(f"max |d| = {dist[worst]:.17g} at vtx[{worst}]")
        print(f"mean |d| (deviating only) = {dist[bad].mean():.17g}")
        print(f"max per axis = {np.abs(d).max(axis=0).tolist()!r}")
    return bad, d


def main():
    if os.environ.get("LILA_COMPARE_FILE"):
        ops.openFile(os.environ["LILA_COMPARE_FILE"])
    threshold = float(os.environ.get("LILA_COMPARE_THRESHOLD", THRESHOLD))
    names = [os.environ.get("LILA_COMPARE_A"), os.environ.get("LILA_COMPARE_B")]
    meshes = [lila.Node(n) for n in names] if all(names) else lila.selected("mesh")[:2]
    if len(meshes) < 2:
        raise RuntimeError("Select two meshes, or set LILA_COMPARE_A and LILA_COMPARE_B")
    compare(meshes[0], meshes[1], threshold)


main()
