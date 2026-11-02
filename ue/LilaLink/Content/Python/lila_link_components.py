"""Custom exporters for Lila attribute components, written in Python.

A component whose Unreal export is custom names an exporter. LilaLink runs a C++ ILilaComponentExporter of that name if
there is one, else the Python function registered here:

    import lila_link_components

    def apply(actor, asset, component, attrs):
        # actor: the placed unreal.Actor, or None for an asset import
        # asset: the imported unreal.Object (mesh, animation), or None for a placed actor
        # attrs: {attribute name: value} with numbers, bools, strings, enum labels, [x, y, z] lists
        ...

    lila_link_components.register("MyExporter", apply)

Register from your project's Content/Python/init_unreal.py so it is there when Lila connects. Raise to report a
failure; the message goes back to Lila as a warning. lila_example_exporter.py is an example.
"""

import json

import unreal

_exporters = {}


def register(name, fn):
    """Replaces any exporter of the same name."""
    _exporters[name] = fn


def unregister(name):
    return _exporters.pop(name, None) is not None


def names():
    return sorted(_exporters)


def names_text():
    """The names joined by |, for LilaLink."""
    return "|".join(names())


def _apply(name, actor_path, asset_path, component, attrs_json):
    """Called by LilaLink."""
    fn = _exporters.get(name)
    if fn is None:
        raise LookupError("no component exporter '%s' is registered in C++ or Python" % name)
    actor = unreal.find_object(None, actor_path) if actor_path else None
    asset = unreal.load_asset(asset_path) if asset_path else None
    fn(actor, asset, component, json.loads(attrs_json))
