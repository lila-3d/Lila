"""lila.bake tests: bakes a tiny plane pair in every format and reads the maps back with numpy.

Run with: lilabatch.exe --batch X:/Lila/python/tests/test_bake.py
"""

import os
import shutil
import tempfile

import numpy as np

import lila
from lila import bake, ops

DIR = os.path.join(tempfile.gettempdir(), "lila_py_bake")


def setup(fmt, bits=8):
    ops.newScene()
    low = ops.polyPlane(name="tile_low")[0]
    high = ops.polyPlane(name="tile_high")[0]
    ops.move(high, by=[0, 1, 0])
    shutil.rmtree(DIR, ignore_errors=True)
    p = bake.project("P", folder=DIR)
    ops.setBakeOutput(project=p, resolution=16, supersampling=1, format=fmt, bits=bits, name="{group}_{map}")
    ops.autoGroupBake(low, high, project=p)
    ops.bakeGroup("tile_bake", frontal=3.0, rear=3.0)
    return p


def test_formats_read_back_alike():
    maps = {}
    for fmt, bits in (("png", 8), ("png", 16), ("exr", 8), ("tga", 8)):
        p = setup(fmt, bits)
        r = bake.bake(p, maps=["normal", "height", "uvIslands"])
        assert bake.miss_rate(r) == 0.0, r["stats"]
        n = bake.load("normal", r)
        assert n.shape == (16, 16, 4) and n.dtype == np.float32
        maps[(fmt, bits)] = {m: bake.load(m, r) for m in ("normal", "height", "uvIslands")}
    ref = maps[("png", 16)]
    for key, got in maps.items():
        for m in ref:
            assert np.allclose(got[m], ref[m], atol=1.5 / 255.0), (key, m, np.abs(got[m] - ref[m]).max())
    # A flat high right above a flat low: straight-up tangent normals.
    assert np.allclose(ref["normal"][..., 2], 1.0, atol=0.01)


def test_results_and_helpers():
    p = setup("png")
    r = bake.bake(p, maps=["height"])
    assert bake.last()["outputs"][0]["file"] == r["outputs"][0]["file"]
    assert bake.output("height", group="tile_bake") is not None
    assert bake.output("height", udim=1002) is None
    assert "tile_bake" in bake.group_stats(r)
    assert "substancePainter" in bake.targets()
    assert "height" in bake.maps()
    assert bake.projects() == [p]
    try:
        bake.load("ao", r)
        assert False, "ao was not baked"
    except LookupError:
        pass


def main():
    tests = [(k, v) for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for name, fn in tests:
        fn()
        print("ok  " + name)
    shutil.rmtree(DIR, ignore_errors=True)
    print("lila.bake: %d tests passed" % len(tests))


main()
