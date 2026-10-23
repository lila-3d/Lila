"""Lila's event bus: call a function when the scene is saved, the selection or time changes, a mode switches, ...

    from lila import events

    def saved(e):
        print("saved", e["path"])

    sub = events.subscribe("sceneSaved", saved)
    events.unsubscribe(sub)

The handler gets a dict {"event", "path", "name", "node", "time"}; which fields mean something depends on the event.
Subscriptions made while an extension loads belong to it and go when it unloads.
"""

from . import ui as _ui

__all__ = ["subscribe", "unsubscribe", "names"]


def subscribe(event, fn, owner=None):
    """Call fn(data) on each `event` (see names()). Returns an id for unsubscribe."""
    return _ui._call("event.subscribe", [fn], event=event, owner=_ui._owner(owner))


def unsubscribe(id):
    """False when id is not subscribed."""
    return _ui._call("event.unsubscribe", id=id)


def names():
    """Every event name."""
    return _ui._call("event.names")
