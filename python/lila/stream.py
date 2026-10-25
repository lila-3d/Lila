"""Scene changes as deltas: one message per command or undo step, the same messages the change stream server sends.

    from lila import stream

    def changed(msg):
        if msg["type"] == "group":
            for c in msg["changes"]:
                print(msg["command"], c["kind"], c.get("path"), c.get("attr"))

    sub = stream.subscribe(changed, types=["transform"], attrs=["translate"])
    stream.unsubscribe(sub)

Messages are dicts with a "type": group, resync (ask for a snapshot), event (sceneSaved and the like), snapshot, and
with the matching options live and frame. The handler runs on the main thread after each command. Subscriptions made while an extension loads belong to it and go
when it unloads. lila.ops.changeStream serves the same stream to other processes over TCP.
"""

import json

__all__ = ["subscribe", "unsubscribe", "snapshot"]


def _filter(types, attrs, under, full, live=False, world=False, evaluated=False):
    return json.dumps({"types": list(types or ()), "attrs": list(attrs or ()), "under": list(under or ()), "full": bool(full),
                       "live": bool(live), "world": bool(world), "evaluated": bool(evaluated)})


def subscribe(fn, types=None, attrs=None, under=None, full=False, live=False, world=False, evaluated=False, owner=None):
    """Call fn(message) for each change message. Returns an id for unsubscribe.

    types: node type names (subtypes match). attrs: attribute long names. under: full paths; the node and everything
    below it. full: whole meshes and large data instead of summaries. live: "live" messages with interim values while
    a drag is still going. world: "world" changes with the evaluated world matrix of each moved transform and those
    under it. evaluated: "evaluated" changes with each output (and each connection-driven input) whose evaluated value
    changed, a mesh's outMesh say. What time moves arrives in "frame" messages, with world or evaluated.
    """
    from .extension import _owner, _registries

    return _registries().stream_subscribe(_owner(owner), lambda text: fn(json.loads(text)),
                                          _filter(types, attrs, under, full, live, world, evaluated))


def unsubscribe(id):
    """False when id is not subscribed."""
    from .extension import _registries

    return _registries().stream_unsubscribe(id)


def snapshot(types=None, attrs=None, under=None, full=False, world=False, evaluated=False):
    """The whole scene as a snapshot message: nodes with their attrs (world matrices with world, evaluated outputs as
    "outputs" with evaluated), connections, selection and time. Messages with a seq at or below its seq are already in
    it."""
    from .extension import _registries

    return json.loads(_registries().stream_snapshot(_filter(types, attrs, under, full, world=world, evaluated=evaluated)))
