"""Debug drawing in the viewports, after Unreal's DrawDebugHelpers (the Kismet argument order, in snake_case).

    from lila import debug_draw as dd
    from lila.math import LinearColor, Rotator, Vector

    dd.draw_debug_line((0, 0, 0), (0, 100, 0), LinearColor.RED, thickness=2)          # gone after 5 s
    dd.draw_debug_box(Vector(0, 50, 0), Vector(50, 50, 50), rotation=Rotator(yaw=45))
    dd.draw_debug_string((0, 120, 0), "spawn", duration=0)                           # the next redraw only

Positions take Vectors or (x, y, z) tuples, colors LinearColors or (r, g, b[, a]) tuples (linear), rotations a
Rotator, a Quat or (pitch, yaw, roll). thickness is in pixels.

Every draw has a lifetime: duration seconds, the debugDraw/defaultDuration pref (5 s) when None, the next redraw when 0.
A forgotten call clears itself. Draws that stay need persistent=True and a channel, and clearing them is your job:

    ch = dd.channel("my_tool")
    dd.draw_debug_lines(path_points, LinearColor.GREEN, persistent=True, channel=ch)
    ...
    ch.clear()

A channel belongs to the extension that makes it and goes when it unloads. Lila clears every channel on File > New and
Open, and Show > Clear Debug Drawing (in the hotbox) clears everything.

Calls are safe from any thread. Bad input never draws: NaN, infinite or absurdly far positions are dropped with a
warning, segment counts and thickness are clamped. Past the debugDraw/maxPrimitives pref (1M lines and triangles) the
oldest draws go first, with one warning. In batch mode draws are kept and expire the same way, with nothing shown.
"""

try:
    from _lila.debug_draw import *  # noqa: F401,F403
    from _lila.debug_draw import _channel
except ImportError:  # an older lila binary running against newer python files
    _channel = None


def channel(name, owner=None):
    """The channel called name, made on first use and owned by the extension calling (or "python" outside one).

    Draws with persistent=True need one. Calling again with the same name returns the same channel.
    """
    if _channel is None:
        raise RuntimeError("debug drawing needs a newer Lila binary")
    from . import ui

    return _channel(name, ui._owner(owner))
