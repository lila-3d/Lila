"""Python extensions and the commands they register.

An extension is a .py file or a package on the extension search paths that defines initialize_extension() and, optionally,
uninitialize_extension(). Load it with lila.cmds.loadExtension("name").

    from lila import extension

    def greet(*names, loud=False):
        text = "Hello, " + ", ".join(names)
        return text.upper() if loud else text

    def initialize_extension():
        extension.register_command("greet", greet, "Say hello.", positional="names",
                                flags=[("loud", bool, False, "Shout.")])

Commands registered while an extension loads belong to it and are removed when it unloads. The function is called with
the positional args and the flags by long name; flags not passed and without a default are left out, so Python
defaults apply. Raise lila.CommandError to fail with a message.
"""

import importlib.util
import os
import sys
import traceback

import _lila

__all__ = ["register_command", "unregister_command", "loaded"]

_TYPES = {bool: "bool", int: "int", float: "float", str: "str", list: "list", tuple: "vec3", None: "any"}

_loading = None  # extension whose initialize/uninitialize is running
_modules = {}  # extension name -> module


def _native():
    native = getattr(_lila, "extension", None)
    if native is None:
        raise RuntimeError("this Lila build has no extension bindings (_lila.extension)")
    return native


def _flag(spec):
    """A flag as a dict, from a dict or a (name, type, default, doc) tuple. The short name only comes from a dict."""
    if isinstance(spec, dict):
        d = dict(spec)
    else:
        spec = tuple(spec)
        if not 1 <= len(spec) <= 4:
            raise ValueError(f"flag {spec!r}: expected (name, type, default, doc)")
        d = dict(zip(("name", "type", "default", "doc"), spec))
    t = d.get("type")
    d["type"] = _TYPES.get(t, t) if not isinstance(t, str) else t
    if not isinstance(d["type"], str):
        raise TypeError(f"flag {d.get('name')!r}: unsupported type {t!r}")
    d.setdefault("short", "")
    d.setdefault("doc", "")
    return d


def _owner_of(fn):
    module = getattr(fn, "__module__", "") or ""
    for name in _modules:
        if module == name or module.startswith(name + "."):
            return name
    return ""


def _refresh():
    from . import cmds

    cmds.refresh()


def register_command(name, fn, doc="", flags=(), positional="", undoable=True, owner=None):
    """Register fn as the Lila command `name`.

    flags: dicts {"name", "short", "type", "default", "doc"} or (name, type, default, doc) tuples, where type is
    bool, int, float, str, list, tuple (a vec3), None (anything) or one of those names as a string.
    positional: description of the positional args; leave empty if the command takes none.
    owner: extension name to file it under; defaults to the extension being loaded or the one fn comes from.
    """
    if owner is None:
        owner = _loading or _owner_of(fn)
    if not doc:
        doc = (getattr(fn, "__doc__", None) or "").strip()
    _native().register_command(name, fn, doc, positional, [_flag(f) for f in flags], undoable, owner)
    if _loading is None:
        _refresh()


def unregister_command(name):
    """Remove a command. Returns whether it existed."""
    had = _native().unregister_command(name)
    if _loading is None:
        _refresh()
    return had


def loaded():
    """Names of the Python extensions loaded now."""
    return sorted(_modules)


def _drop_modules(name):
    for mod in [m for m in sys.modules if m == name or m.startswith(name + ".")]:
        del sys.modules[mod]


def _load(name, path):
    """Called by the extension manager."""
    global _loading
    if name in _modules:
        return
    if os.path.isdir(path):
        spec = importlib.util.spec_from_file_location(
            name, os.path.join(path, "__init__.py"), submodule_search_locations=[path]
        )
    else:
        spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"can't import {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
        init = getattr(module, "initialize_extension", None)
        if not callable(init):
            raise AttributeError(f"{path} does not define initialize_extension()")
        _loading = name
        try:
            init()
        finally:
            _loading = None
    except BaseException:
        if hasattr(_lila, "extension"):
            _lila.extension.unregister_owner(name)
        _drop_modules(name)
        raise
    _modules[name] = module
    _refresh()


def _unload(name):
    """Called by the extension manager. Commands the extension owns go even if uninitialize_extension() fails."""
    global _loading
    module = _modules.pop(name, None)
    if module is None:
        return
    uninit = getattr(module, "uninitialize_extension", None)
    if callable(uninit):
        _loading = name
        try:
            uninit()
        except Exception:
            traceback.print_exc()
        finally:
            _loading = None
    if hasattr(_lila, "extension"):
        _lila.extension.unregister_owner(name)
    _drop_modules(name)
    _refresh()
