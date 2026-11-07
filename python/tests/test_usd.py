"""lila.usd tests: pxr imports inside Lila's Python, and live stages edited through Pixar's bindings.

Run with: lilabatch.exe --batch X:/Lila/python/tests/test_usd.py
"""

import math
import os
import shutil
import sys
import tempfile

import lila
from lila import ops, usd

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "usd", "fixtures")
DIR = os.path.join(tempfile.gettempdir(), "lila_py_usd")


def open_live():
    ops.newScene()
    shutil.rmtree(DIR, ignore_errors=True)
    os.makedirs(DIR)
    src = os.path.join(DIR, "live.usda")
    shutil.copyfile(os.path.join(FIXTURES, "live.usda"), src)
    return src, ops.usdStageOpen(src)


def length(v):
    return math.sqrt(sum(c * c for c in v))


def loaded_modules(basename):
    """Paths of every loaded module with this file name."""
    import ctypes
    from ctypes import wintypes

    psapi = ctypes.WinDLL("psapi")
    kernel = ctypes.WinDLL("kernel32")
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    psapi.EnumProcessModules.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetModuleFileNameW.argtypes = [wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    proc = kernel.GetCurrentProcess()
    mods = (wintypes.HMODULE * 4096)()
    need = wintypes.DWORD()
    assert psapi.EnumProcessModules(proc, mods, ctypes.sizeof(mods), ctypes.byref(need))
    out = []
    for m in mods[: need.value // ctypes.sizeof(wintypes.HMODULE)]:
        buf = ctypes.create_unicode_buffer(1024)
        kernel.GetModuleFileNameW(m, buf, 1024)
        if os.path.basename(buf.value).lower() == basename.lower():
            out.append(buf.value)
    return out


def test_pxr_imports_while_an_extension_loads_before_openusd():
    # First of the tests, so nothing has imported pxr yet.
    assert "pxr" not in sys.modules
    if ops.extensionInfo("OpenUSD").get("loaded"):
        ops.unloadExtension("OpenUSD")
    pkg = os.path.join(tempfile.gettempdir(), "lila_py_usd_pxr_probe")
    shutil.rmtree(pkg, ignore_errors=True)
    os.makedirs(os.path.join(pkg, "pxrprobe"))
    with open(os.path.join(pkg, "extension.json"), "w") as f:
        f.write('{"id": "PxrProbe", "version": "1.0", "sdk": "1.0", "python": "pxrprobe"}')
    with open(os.path.join(pkg, "pxrprobe", "__init__.py"), "w") as f:
        f.write("def initialize_extension():\n    from pxr import Sdf\n\n    Sdf.Layer.CreateAnonymous()\n")
    ops.loadExtension(pkg)
    try:
        assert ops.extensionInfo("PxrProbe").get("loaded"), ops.extensionInfo("PxrProbe").get("error")
        assert ops.extensionInfo("OpenUSD").get("loaded")
    finally:
        ops.unloadExtension("PxrProbe")


def test_pxr_imports_from_the_extension():
    from pxr import Sdf, Tf, Usd, UsdGeom

    root = os.path.normcase(os.path.abspath(usd.package_root()))
    assert os.path.normcase(os.path.abspath(Usd.__file__)).startswith(root), Usd.__file__
    s = Usd.Stage.CreateInMemory()
    UsdGeom.Xform.Define(s, "/Root")
    assert s.GetPrimAtPath("/Root").GetTypeName() == "Xform"
    assert Sdf.Path("/Root").name == "Root"
    assert Tf.Type.FindByName("UsdGeomMesh").typeName == "UsdGeomMesh"
    if sys.platform == "win32":
        # One copy of each library, shared by OpenUSDCore and the bindings: the same Tf and Sdf registries.
        for lib in ("usd_tf.dll", "usd_sdf.dll", "usd_usd.dll", "OpenUSDCore.dll"):
            paths = loaded_modules(lib)
            assert len(paths) == 1, (lib, paths)
            assert os.path.normcase(paths[0]).startswith(root), (lib, paths)


def test_pxr_schemas_from_every_feature():
    from pxr import Plug, UsdShade

    names = {p.name for p in Plug.Registry().GetAllPlugins()}
    for plugin in ("usdImaging", "hdSt", "usdMtlx", "hgiVulkan"):
        assert plugin in names, (plugin, sorted(names))
    assert UsdShade.Material


def test_live_stage_is_the_nodes_stage():
    from pxr import Usd

    src, name = open_live()
    stage = usd.stage(name)
    assert isinstance(stage, Usd.Stage)
    assert stage.GetPrimAtPath("/World/Box").IsValid()
    assert stage.GetEditTarget().GetLayer().anonymous
    assert usd.stage("Box") == stage  # an object picks its stage
    assert usd.stage() == stage  # the only stage

    # Lila's edits land in the stage the script holds.
    ops.usdStageSetProperty("Lamp", "studio:assetId", "LA_777")
    assert stage.GetPrimAtPath("/World/Lamp").GetAttribute("studio:assetId").Get() == "LA_777"


def test_script_edits_reach_the_objects():
    from pxr import Gf, Sdf

    src, name = open_live()
    before = open(src, encoding="utf-8").read()
    stage = usd.stage(name)
    assert abs(length(lila.node("Box").translate) - 10) < 1e-6

    stage.GetPrimAtPath("/World/Box").GetAttribute("xformOp:translate").Set(Gf.Vec3d(25, 0, 0))
    lamp = stage.GetPrimAtPath("/World/Lamp")
    lamp.CreateAttribute("studio:owner", Sdf.ValueTypeNames.String, custom=True).Set("py")
    assert usd.update() >= 1
    assert abs(length(lila.node("Box").translate) - 25) < 1e-6, lila.node("Box").translate
    props = {p["name"]: p.get("value") for p in ops.usdStagePrim("Lamp")["properties"]}
    assert props.get("studio:owner") == '"py"', props

    edits = ops.usdStageInfo(name, edits=True)["edits"]
    assert "over" in edits and "studio:owner" in edits, edits
    assert open(src, encoding="utf-8").read() == before

    # Saving leaves the script's edits alone.
    ops.saveScene(os.path.join(DIR, "scene.lila"))
    assert abs(length(lila.node("Box").translate) - 25) < 1e-6
    assert stage.GetPrimAtPath("/World/Box").GetAttribute("xformOp:translate").Get() == Gf.Vec3d(25, 0, 0)


def test_handle_expires_with_the_node():
    src, name = open_live()
    stage = usd.stage(name)
    assert not stage.expired
    ops.newScene()
    assert stage.expired
    try:
        usd.stage(name)
        assert False, "no stage left"
    except lila.CommandError:
        pass


def test_extension_usd_plugins_reach_plug():
    from pxr import Plug

    pkg = os.path.join(DIR, "PlugProbe.lilaext")
    shutil.rmtree(pkg, ignore_errors=True)
    os.makedirs(os.path.join(pkg, "usd", "hdPlugProbe", "resources"))
    with open(os.path.join(pkg, "extension.json"), "w") as f:
        f.write('{"id": "PlugProbe", "version": "1.0", "sdk": "1.0", "usdPlugins": ["usd"]}')
    with open(os.path.join(pkg, "usd", "hdPlugProbe", "resources", "plugInfo.json"), "w") as f:
        f.write('{"Plugins": [{"Info": {"Types": {"HdPlugProbeRendererPlugin": {"bases": ["HdRendererPlugin"], "displayName": "Plug Probe"}}},'
                ' "LibraryPath": "../../hdPlugProbe.dll", "Name": "hdPlugProbe", "ResourcePath": "resources", "Root": "..", "Type": "library"}]}')
    assert Plug.Registry().GetPluginWithName("hdPlugProbe") is None
    ops.loadExtension(pkg)
    try:
        assert Plug.Registry().GetPluginWithName("hdPlugProbe") is not None
    finally:
        ops.unloadExtension("PlugProbe")


def main():
    tests = [(k, v) for k, v in list(globals().items()) if k.startswith("test_") and callable(v)]
    for name, fn in tests:
        fn()
        print("ok  " + name)
    ops.newScene()
    shutil.rmtree(DIR, ignore_errors=True)
    print("lila.usd: %d tests passed" % len(tests))


main()
