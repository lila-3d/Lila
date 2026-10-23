"""The viewport from Python: gizmos, cursor badges, overlay icons, drawers and pickers, per node type draw overrides,
selection handlers, render passes and viewport tools.

Registrations are owned by the extension that makes them (or "python" outside one) and removed when it unloads.
Geometry callbacks (drawers, draw overrides, render passes, tool drawing) return a dict:

    {"lines": [{"a": [x, y, z], "b": [x, y, z], "color": [r, g, b, a]}],   # world space, linear colors
     "labels": [{"at": [x, y], "text": "..."}]}                            # widget pixels

Views arrive as {"view": [16], "proj": [16], "eye": [x, y, z], "width", "height", "camera", "ortho"}; project() turns
a world point into widget pixels with one.

    from lila import viewport

    def joint_under(hit):
        return ["elbow_l"] if hit["name"] == "body" and hit["face"] is not None else None

    viewport.add_selection_handler("my.joints", redirect=joint_under)
    viewport.set_selection_handler("my.joints")
"""

import json

try:
    import _lila_viewport
except ImportError:  # batch mode without the UI library, or an older lila binary
    _lila_viewport = None


def _owner(owner=None):
    from . import ui

    return ui._owner(owner) if owner is None else owner


def _wrap(fn):
    if fn is None:
        return None

    def call(text):
        return json.dumps(fn(*json.loads(text)), default=str)

    return call


def _call(op, fns=(), **args):
    if _lila_viewport is None:
        raise RuntimeError("this needs Lila's UI library (_lila_viewport), which this session does not have")
    return json.loads(_lila_viewport.call(op, json.dumps(args), tuple(_wrap(f) for f in fns)))


def available():
    """True when the viewport registries can be reached (not in batch mode)."""
    return _lila_viewport is not None


# ------------------------------------------------------------------ gizmos


def gizmos():
    """Gizmo providers as dicts: id, label, owner."""
    return _call("gizmo.list")


def set_gizmo(id):
    """Make provider id the active gizmo for every viewport; "" goes back to the pref. False for an unknown id."""
    return _call("gizmo.setActive", id=id)


def gizmo():
    """The active gizmo provider's id."""
    return _call("gizmo.active")


# ------------------------------------------------------------------ icons and badges


def add_icon(id, tabler="", file="", owner=None):
    """An overlay icon by id, from a Tabler icon name or an SVG/PNG file. Drawn as a tinted silhouette."""
    return _call("icon.add", id=id, tabler=tabler, file=file, owner=_owner(owner))


def remove_icon(id):
    return _call("icon.remove", id=id)


def icons():
    return _call("icon.list")


def add_badge(id, icon, active, message="", actions=(), key="", owner=None):
    """A cursor badge: icon (an overlay icon id, or a Tabler name) shows by the cursor while active(state) is true.
    state: {"panel", "cursor": [x, y], "tool", "navigating", "gizmo", "dragging"}."""
    return _call("badge.add", [active], id=id, icon=icon, message=message, actions=list(actions), key=key, owner=_owner(owner))


def remove_badge(id):
    return _call("badge.remove", id=id)


def badges():
    return _call("badge.list")


# ------------------------------------------------------------------ drawers, pickers and draw overrides


def add_drawer(id, draw, owner=None):
    """draw(view) runs every frame and returns lines and labels."""
    return _call("drawer.add", [draw], id=id, owner=_owner(owner))


def remove_drawer(id):
    return _call("drawer.remove", id=id)


def add_picker(id, pick, owner=None):
    """pick({"a", "b", "click", "view"}) returns [[name, pixels], ...]: the nearest hit for a click, everything
    inside for a marquee (pixels 0)."""
    return _call("picker.add", [pick], id=id, owner=_owner(owner))


def remove_picker(id):
    return _call("picker.remove", id=id)


def hooks():
    """{"drawers": [ids], "pickers": [ids]} in the order they run."""
    return _call("hooks.list")


def add_draw_override(id, node_type, draw=None, pick=None, owner=None):
    """Draw and pick every node of node_type (and types derived from it).
    draw({"node", "world": [16], "highlight": 0-3, "color": [r, g, b, a], "view"}) returns lines and labels; use
    "color" to follow Lila's selection shading. pick({"node", "world", "a", "b", "click", "view"}) returns the
    distance in pixels for a click hit, any number for a node inside a marquee, or None."""
    return _call("drawOverride.add", [draw, pick], id=id, nodeType=node_type, owner=_owner(owner))


def remove_draw_override(id):
    return _call("drawOverride.remove", id=id)


def draw_overrides():
    return _call("drawOverride.list")


# ------------------------------------------------------------------ selection handlers


def add_selection_handler(id, redirect=None, hover=None, label="", owner=None):
    """redirect(hit) returns names to select instead of what was hit, or None to leave the click alone.
    hover(hit) returns {"mesh": name, "domain": "face"|"point", "weights": [...], "color": [r, g, b, a]} or None.
    hit: {"node", "name", "face", "point", "position", "px", "click"}; face and point are None off a mesh."""
    return _call("selectionHandler.add", [redirect, hover], id=id, label=label or id, owner=_owner(owner))


def remove_selection_handler(id):
    return _call("selectionHandler.remove", id=id)


def set_selection_handler(id):
    """Make id the active handler; "" for none. False for an unknown id."""
    return _call("selectionHandler.setActive", id=id)


def selection_handler():
    return _call("selectionHandler.active")


def selection_handlers():
    return _call("selectionHandler.list")


# ------------------------------------------------------------------ render passes


def add_render_pass(id, run, stage="overlay", order=0, owner=None):
    """run({"view", "panel"}) returns lines and labels. stage: "scene" (after Lila's meshes and locators),
    "overlay" (after drawers, tools and the gizmo) or "final" (on top of everything)."""
    return _call("renderPass.add", [run], id=id, stage=stage, order=order, owner=_owner(owner))


def remove_render_pass(id):
    return _call("renderPass.remove", id=id)


def render_passes():
    return _call("renderPass.list")


# ------------------------------------------------------------------ viewport tools


def add_tool(id, event, draw=None, label="", icon="", name="", owner=None):
    """A viewport tool. event(e) gets every mouse, wheel and key event first and returns True to consume it:
    e = {"type": "press"|"move"|"release"|"wheel"|"key"|"activate"|"deactivate", "x", "y", "button", "key", "text",
    "modifiers", "view"}. draw(view) returns lines and labels while the tool is active."""
    return _call("tool.add", [event, draw], id=id, label=label or id, icon=icon, name=name or id, owner=_owner(owner))


def remove_tool(id):
    return _call("tool.remove", id=id)


def activate_tool(id):
    return _call("tool.activate", id=id)


def deactivate_tool():
    return _call("tool.deactivate")


def tool():
    """The registered id of the active viewport tool, "" when none."""
    return _call("tool.active")


def tools():
    return _call("tool.list")


def project(view, point):
    """Widget pixels [x, y] of a world point in a view dict, or None behind the camera."""
    return _call("project", view=view, point=list(point))


def remove_owned(owner):
    """Remove everything owner registered in the viewport."""
    return _call("owner.removeAll", owner=owner)
