"""Lila scripting API.

    import lila
    loc = lila.locator("Target")          # object API (lila.nodes)
    loc.translate_y = 10
    lila.ops.parent("Target", "Rig")     # every command, also as bare names in the script editor
"""

from contextlib import contextmanager

from _lila import CommandError

try:
    from _lila import Progress
except ImportError:  # an older lila binary running against newer python files
    Progress = None

from . import ops, debug_draw, docs, events, math, nodes, prefs, scene_data, tasks, ui
from .nodes import Node, exists, group, locator, node, selected
from .tasks import run_in_background

__all__ = ["ops", "debug_draw", "docs", "events", "math", "nodes", "prefs", "scene_data", "tasks", "ui", "CommandError", "Progress", "workspace", "undo_chunk",
           "Node", "node", "selected", "exists", "locator", "group", "run_in_background"]


def workspace():
    """Root of the open workspace folder, or None."""
    import _lila

    return _lila.workspace()


@contextmanager
def undo_chunk(name="chunk"):
    """Groups every edit made inside into one undo step. Chunks nest; only the outermost becomes a step.

        with lila.undo_chunk("Build arm"):
            ...

        @lila.undo_chunk("Build arm")
        def build_arm(): ...
    """
    ops.undoInfo(openChunk=True, chunkName=name)
    try:
        yield
    finally:
        ops.undoInfo(closeChunk=True)
