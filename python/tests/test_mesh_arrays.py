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
