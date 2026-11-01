"""Run with: lilabatch -b python/tests/test_mesh_arrays.py"""
import threading
from concurrent.futures import ThreadPoolExecutor

import numpy as np

import _lila
import lila
from lila import ops

ops.newScene()
cube = lila.Node(ops.polyCube(width=2, height=2, depth=2)[0])

pts = cube.points_array()
assert pts.shape == (8, 3) and pts.dtype == np.float64, (pts.shape, pts.dtype)
assert not pts.flags.writeable
assert np.allclose(pts, [list(p) for p in cube.points])
assert np.allclose(np.abs(pts), 1.0)

counts, face_points = cube.face_arrays()
assert counts.dtype == np.uint32 and counts.tolist() == [4] * 6
offsets = np.cumsum(counts) - counts
assert [face_points[o:o + c].tolist() for o, c in zip(offsets, counts)] == cube.faces

normals = cube.normals_array()
assert normals.shape == (24, 3) and normals.dtype == np.float32
assert np.allclose(np.linalg.norm(normals, axis=1), 1.0, atol=1e-5)
assert "uv0" in cube.uv_sets
assert cube.uvs_array().shape == (24, 2)

slots, names = cube.material_array()
assert slots.dtype == np.uint32 and slots.tolist() == [0] * 6 and len(names) == 1, (slots, names)
red = ops.createMaterial("red")
ops.assignMaterial(red, cube.path, faces=[1, 4])
slots, names = cube.material_array()
assert [names[s] for s in slots] == [names[0], "red", names[0], names[0], "red", names[0]], (slots.tolist(), names)
ops.undo()
print("test_mesh_arrays read: ok")

cube.set_points(pts * 3.0)
assert np.allclose(cube.points_array(), pts * 3.0)
assert np.allclose(pts, cube.points_array() / 3.0)  # the old snapshot did not change
ops.undo()
assert np.allclose(cube.points_array(), pts)
print("test_mesh_arrays write: ok")

normals = cube.normals_array()
assert np.shares_memory(normals, cube.normals_array())  # cached per snapshot
moved = pts.copy()
moved[0] += 1.0
cube.set_points(moved)
assert not np.allclose(cube.normals_array(), normals)
ops.undo()
assert np.allclose(cube.normals_array(), normals)
print("test_mesh_arrays normals cache: ok")

assert cube.mesh_counts() == (8, 12, 6, 24)
buf = np.empty((8, 3), np.float32)
assert cube.points_array(out=buf) is buf and np.allclose(buf, pts)
flat = np.empty(24, np.float64)
cube.points_array(world=True, out=flat)
assert np.allclose(flat.reshape(8, 3), pts)
n32 = np.empty((24, 3), np.float64)
assert np.allclose(cube.normals_array(out=n32), normals)
uvs = np.empty((24, 2), np.float32)
assert np.array_equal(cube.uvs_array(out=uvs), cube.uvs_array())
counts64, points64 = np.empty(6, np.int64), np.empty(24, np.int32)
cube.face_arrays(out=(counts64, points64))
assert counts64.tolist() == counts.tolist() and points64.tolist() == face_points.tolist()
slots_out = np.empty(6, np.int32)
assert cube.material_array(out=slots_out)[0] is slots_out
for bad, why in [(np.empty((8, 2)), "shape"), (np.empty((8, 3), np.int32), "float32 or float64"), (np.empty((3, 8)).T, "C-contiguous"),
                 (np.empty((8, 3), np.float16), "dtype")]:
    try:
        cube.points_array(out=bad)
        raise AssertionError("accepted " + why)
    except ValueError as e:
        assert why in str(e), (why, str(e))
ro = np.empty((8, 3))
ro.flags.writeable = False
try:
    cube.points_array(out=ro)
    raise AssertionError("accepted a read-only out")
except ValueError:
    pass
print("test_mesh_arrays out: ok")

edges = cube.edge_arrays()
assert edges.shape == (12, 2) and edges.dtype == np.uint32
face_edges = {frozenset((f[i], f[(i + 1) % len(f)])) for f in cube.faces for i in range(len(f))}
assert {frozenset(e) for e in edges.tolist()} == face_edges
names = {(n, d): t for n, d, t in cube.attribute_names()}
assert names[("uv0", "corner")] == "float2", names
uv_attr, domain = cube.attribute_array("uv0")
assert domain == "corner" and np.array_equal(uv_attr, cube.uvs_array())
try:
    cube.attribute_array("nope")
    raise AssertionError("found a missing attribute")
except ValueError as e:
    assert "attribute_names" in str(e)
print("test_mesh_arrays attributes: ok")

tangents = cube.tangents_array()
assert tangents.shape == (24, 4) and tangents.dtype == np.float32
assert np.allclose(np.linalg.norm(tangents[:, :3], axis=1), 1.0, atol=1e-4) and np.allclose(np.abs(tangents[:, 3]), 1.0)
assert np.allclose(np.einsum("ij,ij->i", tangents[:, :3], normals), 0.0, atol=1e-4)
assert np.shares_memory(tangents, cube.tangents_array())
bitangents = cube.bitangents_array()
assert np.allclose(np.einsum("ij,ij->i", bitangents, normals), 0.0, atol=1e-4)
assert np.allclose(np.einsum("ij,ij->i", bitangents, tangents[:, :3]), 0.0, atol=1e-4)
print("test_mesh_arrays tangents: ok")

assert cube.morph_targets() == []
target = lila.Node(ops.polyCube(width=2, height=2, depth=2)[0])
moved = target.points_array().copy()
moved[5] += (0.0, 0.5, 0.0)
target.set_points(moved)
ops.morph(target.path, cube.path)
morphs = cube.morph_targets()
assert len(morphs) == 1, morphs
assert morphs[0]["indices"].tolist() == [5] and np.allclose(morphs[0]["deltas"], [[0.0, 0.5, 0.0]]), morphs
print("test_mesh_arrays morphs: ok")

with ThreadPoolExecutor(max_workers=4) as pool:
    sums = list(pool.map(lambda r: pts[r[0]:r[1]].sum(axis=0), [(0, 2), (2, 4), (4, 6), (6, 8)]))
assert np.allclose(np.sum(sums, axis=0), pts.sum(axis=0))

errors = []


def touch_scene():
    try:
        ops.polySphere()
    except lila.CommandError as e:
        errors.append(str(e))
    try:
        cube.points_array()
    except lila.CommandError as e:
        errors.append(str(e))


t = threading.Thread(target=touch_scene)
t.start()
t.join()
assert len(errors) == 2 and "main thread" in errors[0] and "main thread" in errors[1], errors
print("test_mesh_arrays threads: ok")

done = []


def work(progress, a):
    progress.step()
    assert not lila.tasks.is_main_thread()
    return a.mean(axis=0)


job = lila.run_in_background(work, lambda r: done.append((lila.tasks.is_main_thread(), r)), args=(pts,), title="Test", total=1)
assert job.wait(10.0)
assert done and done[0][0] and np.allclose(done[0][1], pts.mean(axis=0))
assert np.allclose(job.result(), pts.mean(axis=0))

failed = []
job = lila.run_in_background(lambda: 1 / 0, on_error=failed.append)
job.wait(10.0)
assert failed and isinstance(failed[0], ZeroDivisionError)

future = lila.tasks.call_on_main(lila.tasks.is_main_thread)
_lila.drain_main()
assert future.done() and future.result() is True
print("test_mesh_arrays tasks: ok")
