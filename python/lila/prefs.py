"""Read and write prefs. Changes hit disk immediately; no save step.

    lila.prefs.get("general", "/viewport/fov", 45.0)
    lila.prefs.set("general", "/viewport/fov", 50.0)
"""

import _lila


def get(file, key, default=None):
    return _lila.pref(file, key, default)


def set(file, key, value, workspace=False):
    """workspace=True writes to the open workspace's .lila/ instead of user prefs."""
    _lila.set_pref(file, key, value, workspace)


def is_locked(file, key):
    """True when a studio folder locks the pref: set() is then ignored and get() returns the studio's value."""
    return _lila.pref_locked(file, key)
