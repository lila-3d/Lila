"""Values saved in the scene file, for tools and scripts that keep data with a scene. Undoable; saved with the scene.

    lila.scene_data.set("myTool.settings", {"radius": 4})
    lila.scene_data.get("myTool.settings", {})

Prefix keys with the tool's name. Values that belong to a workspace or the user go in lila.prefs instead.
"""

import _lila


def get(key, default=None):
    return _lila.scene_data(key, default)


def set(key, value):
    """Any JSON-compatible value. None removes the key."""
    _lila.set_scene_data(key, value)


def remove(key):
    _lila.set_scene_data(key, None)


def keys():
    from . import ops

    return list(ops.sceneData().keys())
