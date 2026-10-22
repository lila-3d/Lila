"""Lila scripting API.

    import lila
    loc = lila.locator("Target")          # object API (lila.nodes)
    loc.translate_y = 10
    lila.cmds.parent("Target", "Rig")     # every command, also as bare names in the script editor
"""

from _lila import CommandError

try:
    from _lila import Progress
except ImportError:  # an older lila binary running against newer python files
    Progress = None

from . import cmds, math, nodes, prefs, ui
from .nodes import Node, exists, group, locator, node, selected

__all__ = ["cmds", "math", "nodes", "prefs", "ui", "CommandError", "Progress", "workspace", "Node", "node", "selected", "exists", "locator", "group"]


def workspace():
    """Root of the open workspace folder, or None."""
    import _lila

    return _lila.workspace()
