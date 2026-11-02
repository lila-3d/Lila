"""Python extensions and the commands they register.

An extension is a .py file or a package on the extension search paths that defines initialize_extension() and, optionally,
uninitialize_extension(). Load it with lila.ops.loadExtension("name"). To ship it with a version, vendor and dependencies,
put it in an <Id>.lilaext folder whose extension.json names it under "python".

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
import json
import os
import sys
import traceback

import _lila

__all__ = [
    "register_command",
    "unregister_command",
    "loaded",
    "register_file_translator",
    "unregister_file_translator",
    "file_translators",
    "register_mcp_tool",
    "unregister_mcp_tool",
    "register_link_exporter",
    "unregister_link_exporter",
    "register_unreal_component_exporter",
    "unregister_unreal_component_exporter",
    "unreal_component_exporters",
    "register_scene_ui_section",
    "unregister_scene_ui_section",
]

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
    from . import ops

    ops.refresh()


def register_command(name, fn, doc="", flags=(), positional="", undoable=True, owner=None, examples=()):
    """Register fn as the Lila command `name`.

    doc: a summary line, then optionally a blank line and a longer description. Defaults to fn's docstring.
    flags: dicts {"name", "short", "type", "default", "doc", "choices", "example", "min", "max"} or (name, type, default, doc)
    tuples, where type is bool, int, float, str, list, tuple (a vec3), None (anything) or one of those names as a
    string. choices lists a str flag's allowed values; example is a short call using the flag; min and max
    bound an int or float flag (calls are clamped, option boxes use them as the range).
    positional: description of the positional args; leave empty if the command takes none.
    owner: extension name to file it under; defaults to the extension being loaded or the one fn comes from.
    examples: Python snippets showing the command in use.

    The docs show in the API reference, in script editor completion and signature help, and in help().
    """
    if owner is None:
        owner = _loading or _owner_of(fn)
    if not doc:
        doc = (getattr(fn, "__doc__", None) or "").strip()
    # Passed only when given, so builds whose native side predates examples still register.
    extra = {"examples": [str(e) for e in examples]} if examples else {}
    _native().register_command(name, fn, doc, positional, [_flag(f) for f in flags], undoable, owner, **extra)
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


def _registries():
    native = getattr(_lila, "registries", None)
    if native is None:
        raise RuntimeError("this Lila build has no registry bindings (_lila.registries)")
    return native


def _owner(owner):
    return (_loading or "") if owner is None else owner


def _operation(op):
    """A translator operation as (command, flags json): from None, a command name or a (command, {flag: value}) pair."""
    if op is None:
        return "", ""
    if isinstance(op, str):
        return op, ""
    command, flags = op
    return command, json.dumps(flags or {})


def register_file_translator(id, extensions, label="", doc="", open=None, import_=None, export=None, priority=0, owner=None):
    """Make File > Open, Import or Export handle files with these extensions (".pts").

    open, import_ and export name the command each runs with the path as its first argument, or are
    (command, {flag: value}) pairs adding fixed flags. Leave one None when the format does not support it.
    """
    args = [_operation(op) for op in (open, import_, export)]
    _registries().register_file_translator(
        _owner(owner), id, label, doc, [e if e.startswith(".") else "." + e for e in extensions],
        args[0][0], args[0][1], args[1][0], args[1][1], args[2][0], args[2][1], priority)


def register_restriction(id, label, key, file="general", doc="", owner=None):
    """Document a pref of yours as one a studio may lock ("$locked" in a studio folder), so studioRestrictions lists it.

    Any pref can be locked without this; registering gives studio admins its label and what locking it does.
    """
    _registries().register_restriction(_owner(owner), id, label, file, key, doc)


def unregister_restriction(id):
    """Returns whether it existed."""
    return _registries().unregister_restriction(id)


def restriction_locked(id):
    """True when a studio folder locks the restriction's pref."""
    return _registries().restriction_locked(id)


def unregister_file_translator(id):
    """Returns whether it existed."""
    return _registries().unregister_file_translator(id)


def file_translators():
    """Every file translator as a dict: id, label, extensions, the open/import/export commands, owner."""
    return json.loads(_registries().file_translators())


def register_mcp_tool(name, fn, description="", schema=None, owner=None):
    """Offer fn to MCP clients (agents) as a tool. fn gets the call's arguments as keywords and returns text.

    schema: the JSON schema of the arguments; defaults to an object with no properties.
    """

    def run(args):
        result = fn(**json.loads(args))
        return result if isinstance(result, str) else json.dumps(result)

    _registries().register_mcp_tool(_owner(owner), name, description or (fn.__doc__ or "").strip(), json.dumps(schema or {}), run)


def unregister_mcp_tool(name):
    return _registries().unregister_mcp_tool(name)


def register_link_exporter(format, fn, owner=None):
    """Write nodes for the Unreal link in a format of your own. fn(node_ids, path) runs on the link's worker thread and
    returns True on success; it must not touch the scene."""
    _registries().register_link_exporter(_owner(owner), format, fn)


def unregister_link_exporter(format):
    _registries().unregister_link_exporter(format)


def register_unreal_component_exporter(name, doc="", owner=None):
    """List a component exporter by name, so a component's Unreal export can pick custom with it. The exporter itself
    runs in the Unreal editor: a C++ ILilaComponentExporter or a lila_link_components.register Python function there."""
    _registries().register_unreal_component_exporter(_owner(owner), name, doc)


def unregister_unreal_component_exporter(name):
    """Returns whether it existed."""
    return _registries().unregister_unreal_component_exporter(name)


def unreal_component_exporters():
    """The listed component exporters, {name: description}, including those the connected Unreal editor has."""
    return json.loads(_registries().unreal_component_exporters())


def register_scene_ui_section(key, save, load, owner=None):
    """Keep your own data in the scene file under "ui"/key. save() returns a JSON-able value (None leaves it out);
    load(value) gets it back after a scene opens, None when the file has none."""
    _registries().register_scene_ui_section(
        _owner(owner), key, lambda: None if (v := save()) is None else json.dumps(v), lambda text: load(json.loads(text)))


def unregister_scene_ui_section(key):
    return _registries().unregister_scene_ui_section(key)


def _remove_ui(name):
    """Actions, menus, panels, modes and event subscriptions the extension registered."""
    from . import ui

    if ui._lila_sdk is not None:
        try:
            ui.remove_owned(name)
        except Exception:
            traceback.print_exc()
    from . import viewport

    if viewport.available():
        try:
            viewport.remove_owned(name)
        except Exception:
            traceback.print_exc()


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
        _remove_ui(name)
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
    _remove_ui(name)
    _drop_modules(name)
    _refresh()
