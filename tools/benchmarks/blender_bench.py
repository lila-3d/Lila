"""Blender side of the Lila/Maya benchmark: every row of docs/performance.md, run in one GUI session.

blender --factory-startup --gpu-vsync off --window-geometry 0 0 W H --python blender_bench.py -- <out.json> [step,step,...]

Each step runs from a timer so the UI keeps drawing; "until on screen" waits for the first 3D view draw after the
command returns (a POST_PIXEL draw handler stamps every draw).
"""
import json
import math
import os
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import bpy
import gpu
import numpy as np

sys.path.insert(0, "X:/Lila/tools/benchmarks")
import mesh_compute  # noqa: E402

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = ARGS[0]
SHIPS = "X:/Tress/Art/Ship/Gen/manyships.obj"
SOUP = os.environ.get("BENCH_SOUP", "ship_soup.obj")  # make_soup.py output
FILES = os.path.join(os.path.dirname(OUT), "files").replace("\\", "/")
os.makedirs(FILES, exist_ok=True)
DEFAULT_STEPS = (
    "setup,import_obj,import_obj,import_obj,save_blend,open_blend,open_blend,open_blend,export_fbx,import_fbx,import_fbx,"
    "convert_vertices,convert_vertices,convert_vertices,weld,subdivide,python,python,python,python,python,"
    "normals,tangents,view_shaded,view_ao,view_wos,view_selected,view_vertex,done"
)
STEPS = (ARGS[1] if len(ARGS) > 1 else DEFAULT_STEPS).split(",")

res = json.load(open(OUT)) if os.path.exists(OUT) else {}
draws = []


def put(key, value, append=False):
    if append:
        res.setdefault(key, []).append(value)
    else:
        res[key] = value
    with open(OUT, "w") as f:
        json.dump(res, f, indent=1)


def on_draw():
    draws.append(time.perf_counter())


def view3d():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                return window, area
    return None, None


def redraw():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            area.tag_redraw()


def with_view(fn):
    window, area = view3d()
    region = next(r for r in area.regions if r.type == "WINDOW")
    with bpy.context.temp_override(window=window, area=area, region=region):
        return fn()


def clear():
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.images):
        for d in list(coll):
            coll.remove(d)
    bpy.data.orphans_purge(do_recursive=True)


def frame_all():
    with_view(lambda: bpy.ops.view3d.view_all(center=False))


class Timed:
    """Times fn, then waits (from timers) until the 3D view has drawn once after it."""

    def __init__(self, fn):
        self.fn = fn

    def run(self):
        t0 = time.perf_counter()
        value = self.fn()
        cmd = time.perf_counter() - t0
        mark = time.perf_counter()
        redraw()
        return t0, cmd, mark, value


def gen():
    """The steps, as a generator: yielding a float sleeps that long (UI keeps drawing), yielding 'draw' waits for a draw."""
    for step in STEPS:
        print("STEP", step, flush=True)
        try:
            yield from run_step(step)
        except Exception:
            import traceback

            put("error_" + step, traceback.format_exc())
        yield 1.0


def wait_draw(after):
    limit = time.perf_counter() + 300
    while not any(d > after for d in draws) and time.perf_counter() < limit:
        redraw()
        yield 0.0
    return next((d for d in draws if d > after), float("nan"))


def until_shown(fn):
    """(command seconds, seconds until on screen)."""
    t0 = time.perf_counter()
    fn()
    cmd = time.perf_counter() - t0
    shown = yield from wait_draw(time.perf_counter())
    return round(cmd, 3), round(shown - t0, 3)


def load_ships():
    clear()
    bpy.ops.wm.obj_import(filepath=SHIPS)
    frame_all()


def frame_ms():
    """Median frame time of the 3D view while it moves, ms: each frame turns the view a little (so the viewport draws
    the scene again instead of reusing its accumulated samples), then draws and swaps."""
    from mathutils import Quaternion

    r3d = view3d()[1].spaces.active.region_3d
    step = Quaternion((0, 0, 1), math.radians(0.05))
    out = []
    for _ in range(5):
        n = 20
        t0 = time.perf_counter()
        for _ in range(n):
            r3d.view_rotation = step @ r3d.view_rotation
            with_view(lambda: bpy.ops.wm.redraw_timer(type="DRAW_SWAP", iterations=1))
        out.append((time.perf_counter() - t0) * 1000.0 / n)
    return round(statistics.median(out), 2), [round(x, 2) for x in out]


def orbit_ms():
    """frame_ms at 4 camera angles around the framed view, the median of their medians."""
    window, area = view3d()
    r3d = area.spaces.active.region_3d
    base = r3d.view_rotation.copy()
    from mathutils import Quaternion

    per = []
    for k in range(4):
        r3d.view_rotation = Quaternion((0, 0, 1), math.radians(45 * k)) @ base
        per.append(frame_ms()[0])
    r3d.view_rotation = base
    return round(statistics.median(per), 2), per


def shading():
    return view3d()[1].spaces.active.shading


def overlay():
    return view3d()[1].spaces.active.overlay


def run_step(step):
    if step == "setup":
        bpy.types.SpaceView3D.draw_handler_add(on_draw, (), "WINDOW", "POST_PIXEL")
        prefs = bpy.context.preferences
        prefs.system.viewport_aa = "8"
        prefs.edit.undo_steps = 1
        window, area = view3d()
        with bpy.context.temp_override(window=window, area=area):
            bpy.ops.screen.screen_full_area(use_hide_panels=True)
        yield 2.0
        window, area = view3d()
        region = next(r for r in area.regions if r.type == "WINDOW")
        put("view_size", [region.width, region.height])
        put("backend", gpu.platform.backend_type_get())
        put("gpu", gpu.platform.renderer_get())
        put("version", bpy.app.version_string)
        clear()
    elif step == "import_obj":
        clear()
        yield 1.0
        r = yield from until_shown(lambda: bpy.ops.wm.obj_import(filepath=SHIPS))
        put(step, r, append=True)
        frame_all()
        put("tris", sum(len(p.vertices) - 2 for o in bpy.data.objects if o.type == "MESH" for p in o.data.polygons))
    elif step == "save_blend":
        load_ships()
        yield 2.0
        path = FILES + "/ships.blend"
        t0 = time.perf_counter()
        bpy.ops.wm.save_as_mainfile(filepath=path)
        put(step, [round(time.perf_counter() - t0, 3), os.path.getsize(path)])
    elif step == "open_blend":
        clear()
        yield 1.0
        r = yield from until_shown(lambda: bpy.ops.wm.open_mainfile(filepath=FILES + "/ships.blend", load_ui=False))
        put(step, r, append=True)
    elif step == "export_fbx":
        load_ships()
        yield 2.0
        path = FILES + "/ships.fbx"
        t0 = time.perf_counter()
        bpy.ops.export_scene.fbx(filepath=path)
        put(step, [round(time.perf_counter() - t0, 3), os.path.getsize(path)])
        path2 = FILES + "/ships_tangents.fbx"
        t0 = time.perf_counter()
        bpy.ops.export_scene.fbx(filepath=path2, use_tspace=True)
        put("export_fbx_tangents", [round(time.perf_counter() - t0, 3), os.path.getsize(path2)])
    elif step == "import_fbx":
        clear()
        yield 1.0
        r = yield from until_shown(lambda: bpy.ops.import_scene.fbx(filepath=FILES + "/ships.fbx"))
        put(step, r, append=True)
    elif step == "convert_vertices":
        load_ships()
        o = next(o for o in bpy.data.objects if o.type == "MESH")
        bpy.context.view_layer.objects.active = o
        o.select_set(True)
        bpy.context.tool_settings.mesh_select_mode = (True, False, False)
        for v in o.data.vertices:
            pass
        o.data.vertices.foreach_set("select", np.ones(len(o.data.vertices), dtype=bool))
        yield 3.0
        r = yield from until_shown(lambda: bpy.ops.object.mode_set(mode="EDIT"))
        put(step, r, append=True)
        bpy.ops.object.mode_set(mode="OBJECT")
    elif step == "weld":
        clear()
        bpy.ops.wm.obj_import(filepath=SOUP)
        o = next(o for o in bpy.data.objects if o.type == "MESH")
        bpy.context.view_layer.objects.active = o
        o.select_set(True)
        frame_all()
        before = len(o.data.vertices)
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        yield 3.0
        r = yield from until_shown(lambda: bpy.ops.mesh.remove_doubles(threshold=0.0001))
        bpy.ops.object.mode_set(mode="OBJECT")
        put(step, [r[0], r[1], before, len(o.data.vertices)])
    elif step == "subdivide":
        load_ships()
        o = next(o for o in bpy.data.objects if o.type == "MESH")
        bpy.context.view_layer.objects.active = o
        o.select_set(True)
        yield 3.0

        def sub():
            m = o.modifiers.new("Subdivision", "SUBSURF")
            m.levels = 1
            m.render_levels = 1
            bpy.ops.object.modifier_apply(modifier=m.name)

        r = yield from until_shown(sub)
        put(step, [r[0], r[1], len(o.data.polygons)])
    elif step == "python":
        load_ships()
        yield 2.0
        meshes = [o.data for o in bpy.data.objects if o.type == "MESH"]
        m = meshes[0]
        t0 = time.perf_counter()
        points = np.empty(len(m.vertices) * 3, dtype=np.float64)
        m.vertices.foreach_get("co", points)
        points = points.reshape(-1, 3)
        gather = time.perf_counter() - t0
        t0 = time.perf_counter()
        att = np.empty(len(m.vertices) * 3, dtype=np.float32)
        m.attributes["position"].data.foreach_get("vector", att)
        gather32 = time.perf_counter() - t0
        t0 = time.perf_counter()
        mean, cov = mesh_compute.compute(points)
        numpy_s = time.perf_counter() - t0
        workers = os.cpu_count() or 4
        with ThreadPoolExecutor(max_workers=workers) as pool:
            mesh_compute.compute_threaded(points[: workers * 1024], pool, workers)
            t0 = time.perf_counter()
            tmean, tcov = mesh_compute.compute_threaded(points, pool, workers)
            threaded_s = time.perf_counter() - t0
        n = min(100_000, len(points))
        subset = points[:n].tolist()
        t0 = time.perf_counter()
        mesh_compute.python_partial(subset)
        python_s = (time.perf_counter() - t0) * len(points) / n
        put("python_runs", {"vertices": len(points), "gather": gather, "gather_float32": gather32, "numpy": numpy_s,
                            "threaded": threaded_s, "workers": workers, "python": python_s,
                            "checksum": mesh_compute.checksum(mean, cov), "checksum_threaded": mesh_compute.checksum(tmean, tcov)},
            append=True)
    elif step == "normals":
        load_ships()
        yield 2.0
        m = next(o for o in bpy.data.objects if o.type == "MESH").data
        for k in range(2):
            t0 = time.perf_counter()
            n = np.empty(len(m.loops) * 3, dtype=np.float32)
            m.corner_normals.foreach_get("vector", n)
            put("normals", [round(time.perf_counter() - t0, 4), len(m.loops)], append=True)
            m.update()
            yield 0.5
    elif step == "tangents":
        load_ships()
        yield 2.0
        m = next(o for o in bpy.data.objects if o.type == "MESH").data
        for k in range(2):
            t0 = time.perf_counter()
            m.calc_tangents()
            t = np.empty(len(m.loops) * 3, dtype=np.float32)
            m.loops.foreach_get("tangent", t)
            s = np.empty(len(m.loops), dtype=np.float32)
            m.loops.foreach_get("bitangent_sign", s)
            put("tangents", [round(time.perf_counter() - t0, 4), len(m.loops)], append=True)
            m.free_tangents()
            yield 0.5
    elif step == "view_shaded":
        load_ships()
        yield 5.0
        sh = shading()
        sh.type = "SOLID"
        sh.show_cavity = False
        overlay().show_wireframes = False
        for o in bpy.data.objects:
            o.select_set(False)
        yield 2.0
        put("shaded", orbit_ms())
    elif step == "view_ao":
        sh = shading()
        sh.show_cavity = True
        sh.cavity_type = "WORLD"
        yield 2.0
        put("ao", orbit_ms())
        sh.cavity_type = "SCREEN"
        yield 1.0
        put("ao_screen", orbit_ms())
        sh.show_cavity = False
    elif step == "view_wos":
        ov = overlay()
        ov.show_wireframes = True
        ov.wireframe_threshold = 1.0
        yield 2.0
        put("wos", orbit_ms())
        ov.show_wireframes = False
    elif step == "view_selected":
        o = next(o for o in bpy.data.objects if o.type == "MESH")
        o.select_set(True)
        bpy.context.view_layer.objects.active = o
        yield 2.0
        put("selected", orbit_ms())
    elif step == "view_vertex":
        o = bpy.context.view_layer.objects.active
        o.data.vertices.foreach_set("select", np.ones(len(o.data.vertices), dtype=bool))
        bpy.context.tool_settings.mesh_select_mode = (True, False, False)
        bpy.ops.object.mode_set(mode="EDIT")
        yield 3.0
        put("vertex", orbit_ms())
        bpy.ops.object.mode_set(mode="OBJECT")
    elif step == "done":
        put("finished", True)
        bpy.ops.wm.quit_blender()


steps = gen()


def tick():
    try:
        delay = next(steps)
    except StopIteration:
        return None
    return max(0.0, float(delay))


bpy.app.timers.register(tick, first_interval=2.0, persistent=True)
