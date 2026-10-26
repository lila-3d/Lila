"""Live USD stages through Pixar's own Python bindings.

    from lila import usd
    from pxr import Gf

    stage = usd.stage("kitchen")                 # the pxr.Usd.Stage the usdStage node composes
    chair = stage.GetPrimAtPath("/Kitchen/Chair")
    chair.GetAttribute("xformOp:translate").Set(Gf.Vec3d(0, 0, 50))
    usd.update()                                 # Lila also takes the edit in by itself while idle

`from pxr import Usd, Sdf, UsdGeom` works anywhere in Lila's Python: the first import loads the OpenUSD extension's
libraries, the very ones its stages use. A live stage's edit target is its edit layer, so what a script authors there
is kept like Lila's own edits, and only the objects of the prims it touches are rebuilt. Use the stage on the main
thread. The handle expires when the node is deleted, the scene closes or the stage's file or edit layer changes; ask
for it again.
"""

import ctypes
import importlib.abc
import importlib.machinery
import os
import sys

import _lila
from _lila import CommandError

from . import ops

__all__ = ["stage", "update", "package_root"]

_core = None


def _library(name):
    return name + (".dll" if sys.platform == "win32" else ".dylib" if sys.platform == "darwin" else ".so")


def package_root():
    """The OpenUSD extension's folder, which holds Pixar's libraries, plugins and the pxr package (under python/)."""
    info = ops.extensionInfo("OpenUSD")
    if not info.get("loaded"):
        raise CommandError("lila.usd: the OpenUSD extension isn't loaded")
    return info["path"]


def _prepare():
    """Loads OpenUSDCore through the extension, which puts its pxr package on sys.path. Returns OpenUSDCore."""
    global _core
    if _core is not None:
        return _core
    root = package_root()
    if not os.path.isdir(os.path.join(root, "python", "pxr")):
        raise CommandError("lila.usd: this OpenUSD build has no Python bindings")
    load = ctypes.CDLL(os.path.join(root, _library("OpenUSD"))).lilaUsdLoad
    load.restype = ctypes.c_char_p
    load.argtypes = []
    error = load()
    if error:
        raise CommandError("lila.usd: " + error.decode("utf-8", "replace"))
    _core = ctypes.PyDLL(os.path.join(root, _library("OpenUSDCore")))
    return _core


def stage(node=None):
    """The pxr.Usd.Stage of a usdStage node, or of the stage an object belongs to; None picks the selected stage, or
    the only one. Edits authored through it go to the stage's edit layer and update the linked objects."""
    if not _lila.is_main_thread():
        raise CommandError("lila.usd.stage: call it on the main thread")
    core = _prepare()
    from pxr import Usd  # noqa: F401  (registers the Stage conversion)

    get = core.lilaUsdStagePython
    get.restype = ctypes.py_object
    get.argtypes = [ctypes.c_char_p]
    try:
        return get(str(node or "").encode("utf-8"))
    except LookupError as e:
        raise CommandError(str(e)) from None


def update():
    """Rebuilds the objects of the prims scripts and other tools changed, now rather than when Lila is next idle.
    Returns how many prims were rebuilt."""
    return ops.usdStageUpdate()


class _PxrLoader(importlib.abc.Loader):
    """pxr's own loader, then the extension's start-up, which imports the rest of pxr while pxr itself initializes, the
    way a package's __init__ imports its submodules."""

    def __init__(self, inner):
        self._inner = inner

    def create_module(self, spec):
        return self._inner.create_module(spec)

    def exec_module(self, module):
        self._inner.exec_module(module)
        _prepare()


class _PxrFinder:
    """Finds pxr in the OpenUSD extension and loads the extension's libraries with it, the same ones its stages use."""

    @staticmethod
    def find_spec(name, path=None, target=None):
        if name != "pxr" or _core is not None:
            return None
        try:
            py = os.path.join(package_root(), "python")
        except Exception:
            return None
        spec = importlib.machinery.PathFinder.find_spec(name, [py])
        if spec is None:
            return None
        if py not in sys.path:
            sys.path.append(py)
        spec.loader = _PxrLoader(spec.loader)
        return spec


def _install_import_hook():
    if not any(f is _PxrFinder for f in sys.meta_path):
        sys.meta_path.insert(0, _PxrFinder)
