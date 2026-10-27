"""lila.debug_draw tests. Run with: lilabatch.exe --batch -c "exec(open(r'python/tests/test_debug_draw.py').read())" """

import math
import threading

import lila
from lila import debug_draw as dd
from lila.math import Color, LinearColor, Plane, Quat, Rotator, Transform, Vector


def fresh():
    dd.flush_debug_draw()
    return dd.stats()


def test_types_and_tuples():
    fresh()
    dd.draw_debug_line((0, 0, 0), Vector(1, 0, 0), (1, 0, 0))
    dd.draw_debug_line([0, 0, 0], [0, 1, 0], LinearColor.RED, 2.0, 3.0, on_top=True)
    dd.draw_debug_point((0, 0, 0), 6, Color(255, 0, 0))
    dd.draw_debug_box((0, 0, 0), (1, 2, 3), rotation=(0, 45, 0))
    dd.draw_debug_box((0, 0, 0), (1, 2, 3), rotation=Rotator(yaw=45))
    dd.draw_debug_box((0, 0, 0), (1, 2, 3), rotation=Quat())
    dd.draw_debug_solid_box((0, 0, 0), (1, 1, 1), (0, 1, 0, 0.5))
    dd.draw_debug_sphere((0, 0, 0), 10, 8)
    dd.draw_debug_capsule((0, 0, 0), 50, 10, Rotator(pitch=90))
    dd.draw_debug_cylinder((0, 0, 0), (0, 100, 0), 10)
    dd.draw_debug_circle((0, 0, 0), 10, 16, normal=(0, 0, 1))
    dd.draw_debug_circle((0, 0, 0), 10, 16, y_axis=(1, 0, 0), z_axis=(0, 1, 0))
    dd.draw_debug_cone((0, 0, 0), (1, 0, 0), 100, 20, 10, 8)
    dd.draw_debug_directional_arrow((0, 0, 0), (100, 0, 0), 10)
    dd.draw_debug_arrow((0, 0, 0), (0, 100, 0))
    dd.draw_debug_coordinate_system((0, 0, 0), Rotator(), 20)
    dd.draw_debug_frustum(Transform(), 60, 1.5, 1, 100)
    dd.draw_debug_plane(Plane(0, 1, 0, 0), (0, 5, 0), 50)
    dd.draw_debug_string((0, 0, 0), "héllo")
    dd.draw_debug_lines([(0, 0, 0), (1, 0, 0), (1, 1, 0)], closed=True)
    dd.draw_debug_segments([(0, 0, 0), (1, 0, 0), (2, 0, 0), (3, 0, 0)])
    s = dd.stats()
    assert s["points"] == 1 and s["texts"] == 1
    assert s["triangles"] == 12 + 2
    assert s["lines"] > 100


def test_numpy_points():
    try:
        import numpy as np
    except ImportError:
        return
    fresh()
    pts = np.zeros((1000, 3))
    pts[:, 0] = np.arange(1000)
    dd.draw_debug_lines(pts)
    assert dd.stats()["lines"] == 999
    try:
        dd.draw_debug_lines(np.zeros((10, 2)))
    except ValueError:
        pass
    else:
        raise AssertionError("an (N, 2) array must be refused")


def test_persistent_needs_a_channel():
    fresh()
    try:
        dd.draw_debug_line((0, 0, 0), (1, 0, 0), persistent=True)
    except ValueError as e:
        assert "channel" in str(e)
    else:
        raise AssertionError("persistent without a channel must raise")
    ch = dd.channel("test_tool")
    assert ch.owner == "python" and ch.name == "test_tool" and ch.valid
    assert dd.channel("test_tool") == ch
    dd.draw_debug_box((0, 0, 0), (1, 1, 1), persistent=True, channel=ch)
    dd.draw_debug_string((0, 0, 0), "x", persistent=True, channel=ch)
    assert ch.primitive_count == 13
    ch.visible = False
    assert not ch.visible
    ch.clear()
    assert ch.primitive_count == 0 and ch.valid
    ch.remove()
    assert not ch.valid
    assert "removed" in repr(ch)


def test_scene_new_clears_channels():
    fresh()
    ch = dd.channel("scene_test")
    dd.draw_debug_line((0, 0, 0), (1, 0, 0), persistent=True, channel=ch)
    lila.ops.newScene()
    assert dd.stats()["lines"] == 0
    assert ch.valid
    ch.remove()


def test_invalid_input_is_dropped_not_raised():
    fresh()
    before = dd.stats()["rejected"]
    dd.draw_debug_line((math.nan, 0, 0), (1, 0, 0))
    dd.draw_debug_sphere((0, 0, 0), math.inf)
    dd.draw_debug_box((1e15, 0, 0), (1, 1, 1))
    dd.draw_debug_string((0, math.nan, 0), "x")
    s = dd.stats()
    assert s["lines"] == 0 and s["texts"] == 0
    assert s["rejected"] - before == 4
    dd.draw_debug_sphere((0, 0, 0), 10, 10_000_000)  # clamped, not a hang
    assert dd.stats()["lines"] == 127 * 256 + 256 * 128
    dd.draw_debug_string((0, 0, 0), "x" * 100_000)
    try:
        dd.draw_debug_box((0, 0, 0), (1, 1, 1), rotation="sideways")
    except TypeError:
        pass
    else:
        raise AssertionError("a bad rotation type must raise TypeError")


def test_threads():
    fresh()

    def work(k):
        for i in range(2000):
            dd.draw_debug_line((i, k, 0), (i, k + 1, 0), duration=600)

    threads = [threading.Thread(target=work, args=(k,)) for k in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert dd.stats()["lines"] == 8000


def test_limits():
    lim = dd.limits()
    assert lim["max_primitives"] >= 1000
    assert lim["default_duration"] > 0


def run():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print("ok", t.__name__)
    dd.flush_debug_draw()
    print(f"{len(tests)} debug_draw tests passed")


run()
