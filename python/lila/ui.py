"""UI from Python: run actions, switch tools, and build tool panels.

Panels are plain PySide6 underneath, with a small declarative layer on top: nest Column/Row, bind widgets straight to
attributes, and decorate a function with @panel to get a dockable window.

    from lila.ui import panel, Column, Row, Label, Button, Slider, show_panel

    @panel("Wheel Rig", area="right")
    def wheel_rig():
        return Column(
            Label("Wheel"),
            Slider("Wheel.rotateX", minimum=-360, maximum=360),
            Row(Button("Key", lambda: lila.ops.setKeyframe("Wheel")), Button("Reset", reset)),
        )

    show_panel("Wheel Rig")
"""

import _lila

__all__ = [
    "run_action",
    "actions",
    "set_tool",
    "main_window",
    "panel",
    "show_panel",
    "Column",
    "Row",
    "Label",
    "Button",
    "Slider",
    "Field",
    "Check",
    "Choice",
    "Spacer",
]


def run_action(action_id):
    """Run a registered UI action, e.g. run_action("viewport.frameAll")."""
    return _lila.run_action(action_id)


def actions():
    """[(id, label, category)] for every registered action."""
    return _lila.actions()


def set_tool(name):
    """Switch the viewport tool: "select", "move", "rotate" or "scale"."""
    return _lila.run_action("tool." + name)


# ------------------------------------------------------------------ PySide layer


def _qt():
    from PySide6 import QtCore, QtWidgets

    return QtCore, QtWidgets


def main_window():
    """Lila's main window as a PySide6 QMainWindow, or None in batch mode."""
    _, QtWidgets = _qt()
    app = QtWidgets.QApplication.instance()
    if app is None:
        return None
    for w in app.topLevelWidgets():
        if isinstance(w, QtWidgets.QMainWindow):
            return w
    return None


_panels = {}
_bindings = []
_timer = None


def _ops():
    from . import ops

    return ops


def _refresh_bindings():
    """Pull bound attribute values into their widgets. Cheap, so a short poll keeps panels live without callbacks."""
    alive = []
    for b in _bindings:
        try:
            b()
            alive.append(b)
        except RuntimeError:
            pass  # widget deleted with its panel
    _bindings[:] = alive


def _ensure_timer():
    global _timer
    if _timer is None:
        QtCore, _ = _qt()
        _timer = QtCore.QTimer()
        _timer.setInterval(200)
        _timer.timeout.connect(_refresh_bindings)
        _timer.start()


class _Node:
    """Base of the declarative widgets: build() returns a QWidget."""

    def build(self):
        raise NotImplementedError


class Column(_Node):
    def __init__(self, *children, spacing=4, margin=6):
        self.children, self.spacing, self.margin = children, spacing, margin

    def _layout(self, QtWidgets):
        return QtWidgets.QVBoxLayout()

    def build(self):
        _, QtWidgets = _qt()
        w = QtWidgets.QWidget()
        lay = self._layout(QtWidgets)
        lay.setSpacing(self.spacing)
        lay.setContentsMargins(self.margin, self.margin, self.margin, self.margin)
        for c in self.children:
            lay.addWidget(c.build() if isinstance(c, _Node) else c)
        w.setLayout(lay)
        return w


class Row(Column):
    def __init__(self, *children, spacing=4, margin=0):
        super().__init__(*children, spacing=spacing, margin=margin)

    def _layout(self, QtWidgets):
        return QtWidgets.QHBoxLayout()


class Label(_Node):
    def __init__(self, text):
        self.text = text

    def build(self):
        _, QtWidgets = _qt()
        return QtWidgets.QLabel(self.text)


class Spacer(_Node):
    def build(self):
        _, QtWidgets = _qt()
        w = QtWidgets.QWidget()
        w.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
        return w


class Button(_Node):
    def __init__(self, text, on_click=None):
        self.text, self.on_click = text, on_click

    def build(self):
        _, QtWidgets = _qt()
        b = QtWidgets.QPushButton(self.text)
        if self.on_click:
            b.clicked.connect(lambda *_: self.on_click())
        return b


class _Bound(_Node):
    """A widget bound to "node.attr": edits run setAttr (undoable, echoed); scene changes flow back."""

    def __init__(self, plug=None, on_change=None, label=None):
        self.plug, self.on_change, self.label = plug, on_change, label

    def _get(self):
        return _ops().getAttr(self.plug) if self.plug else None

    def _set(self, value):
        if self.plug:
            _ops().setAttr(self.plug, value)
        if self.on_change:
            self.on_change(value)

    def _wrap(self, widget):
        if not self.label:
            return widget
        _, QtWidgets = _qt()
        row = QtWidgets.QWidget()
        lay = QtWidgets.QHBoxLayout(row)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.addWidget(QtWidgets.QLabel(self.label))
        lay.addWidget(widget, 1)
        return row


class Slider(_Bound):
    def __init__(self, plug=None, minimum=0.0, maximum=1.0, on_change=None, label=None, steps=1000):
        super().__init__(plug, on_change, label or plug)
        self.minimum, self.maximum, self.steps = minimum, maximum, steps

    def build(self):
        QtCore, QtWidgets = _qt()
        s = QtWidgets.QSlider(QtCore.Qt.Horizontal)
        s.setRange(0, self.steps)
        span = self.maximum - self.minimum

        def to_value(i):
            return self.minimum + span * i / self.steps

        def pull():
            if s.isSliderDown():
                return
            v = self._get()
            if isinstance(v, (int, float)):
                s.blockSignals(True)
                s.setValue(int(round((v - self.minimum) / span * self.steps)) if span else 0)
                s.blockSignals(False)

        s.sliderReleased.connect(lambda: self._set(to_value(s.value())))
        s.valueChanged.connect(lambda i: None if s.isSliderDown() else self._set(to_value(i)))
        if self.plug:
            pull()
            _bindings.append(pull)
            _ensure_timer()
        return self._wrap(s)


class Field(_Bound):
    """Numeric or text field."""

    def build(self):
        _, QtWidgets = _qt()
        e = QtWidgets.QLineEdit()

        def pull():
            if not e.hasFocus():
                v = self._get()
                e.setText("" if v is None else (f"{v:g}" if isinstance(v, float) else str(v)))

        def push():
            text = e.text()
            try:
                value = float(text) if text.replace(".", "", 1).lstrip("-").isdigit() else text
            except ValueError:
                value = text
            self._set(value)

        e.editingFinished.connect(push)
        if self.plug:
            pull()
            _bindings.append(pull)
            _ensure_timer()
        return self._wrap(e)


class Check(_Bound):
    def __init__(self, plug=None, text="", on_change=None):
        super().__init__(plug, on_change)
        self.text = text

    def build(self):
        _, QtWidgets = _qt()
        c = QtWidgets.QCheckBox(self.text or (self.plug or ""))

        def pull():
            v = self._get()
            if isinstance(v, bool):
                c.blockSignals(True)
                c.setChecked(v)
                c.blockSignals(False)

        c.toggled.connect(lambda on: self._set(bool(on)))
        if self.plug:
            pull()
            _bindings.append(pull)
            _ensure_timer()
        return c


class Choice(_Bound):
    def __init__(self, items, plug=None, on_change=None, label=None):
        super().__init__(plug, on_change, label)
        self.items = list(items)

    def build(self):
        _, QtWidgets = _qt()
        box = QtWidgets.QComboBox()
        box.addItems([str(i) for i in self.items])

        def pull():
            v = self._get()
            if isinstance(v, int) and 0 <= v < len(self.items):
                box.blockSignals(True)
                box.setCurrentIndex(v)
                box.blockSignals(False)

        box.currentIndexChanged.connect(lambda i: self._set(i))
        if self.plug:
            pull()
            _bindings.append(pull)
            _ensure_timer()
        return self._wrap(box)


def panel(name, area="right"):
    """Decorator: the function returns a layout (or a QWidget); show_panel(name) docks it in the main window."""

    def wrap(fn):
        _panels[name] = (fn, area)
        return fn

    return wrap


def show_panel(name):
    """Create (or raise) the panel's dock. Returns the QDockWidget."""
    QtCore, QtWidgets = _qt()
    if name not in _panels:
        raise KeyError(f"no panel named {name!r}; decorate a function with @panel({name!r})")
    mw = main_window()
    if mw is None:
        raise RuntimeError("panels need the Lila UI (not batch mode)")
    object_name = "py_" + "".join(ch if ch.isalnum() else "_" for ch in name)
    for dock in mw.findChildren(QtWidgets.QDockWidget):
        if dock.objectName() == object_name:
            dock.show()
            dock.raise_()
            return dock
    fn, area = _panels[name]
    content = fn()
    widget = content.build() if isinstance(content, _Node) else content
    dock = QtWidgets.QDockWidget(name, mw)
    dock.setObjectName(object_name)
    dock.setWidget(widget)
    where = {
        "left": QtCore.Qt.LeftDockWidgetArea,
        "right": QtCore.Qt.RightDockWidgetArea,
        "bottom": QtCore.Qt.BottomDockWidgetArea,
        "top": QtCore.Qt.TopDockWidgetArea,
    }.get(area, QtCore.Qt.RightDockWidgetArea)
    mw.addDockWidget(where, dock)
    dock.show()
    return dock


# ------------------------------------------------------------------ registries
#
# Actions, menus, panels, modes, layouts, marking menus, hotkey layers, shelves and preferences pages, owned by the
# extension that registers them (or "python" outside one) and removed when it unloads.

try:
    import _lila_sdk
except ImportError:  # batch mode without the UI library, or an older lila binary
    _lila_sdk = None


def _owner(owner=None):
    """The extension a registration belongs to: the one loading, else the one whose code is calling."""
    if owner:
        return owner
    import sys

    from . import extension

    if extension._loading:
        return extension._loading
    frame = sys._getframe(2)
    while frame is not None:
        module = frame.f_globals.get("__name__", "")
        for name in extension._modules:
            if module == name or module.startswith(name + "."):
                return name
        frame = frame.f_back
    return "python"


def _wrap(fn):
    if fn is None:
        return None
    import json

    def call(text):
        return json.dumps(fn(*json.loads(text)), default=str)

    return call


def _call(op, fns=(), **args):
    if _lila_sdk is None:
        raise RuntimeError("this needs Lila's UI library (_lila_sdk), which this session does not have")
    import json

    return json.loads(_lila_sdk.call(op, json.dumps(args), tuple(_wrap(f) for f in fns)))


def add_action(id, fn, label=None, category="Python", doc="", owner=None):
    """Register fn as the UI action `id`, so menus, hotkeys, marking menus and shelves can run it."""
    return _call("action.add", [lambda: fn()], id=id, label=label or id, category=category, doc=doc or (fn.__doc__ or "").strip(),
                 owner=_owner(owner))


def remove_action(id):
    return _call("action.remove", id=id)


def add_menu(id, title=None, area="menuBar", before="", docs="", owner=None):
    """A menu bar menu (area "menuBar") or a hotbox panel menu ("hotbox"). Items go in with add_menu_item. docs names a
    docs page ("curves", "essentials?id=snapping") whose "Open <Page> Docs" row ends the menu."""
    return _call("menu.add", id=id, title=title or id, area=area, before=before, docs=docs, owner=_owner(owner))


def add_menu_item(menu, action=None, label=None, kind=None, id="", after="", before="", owner=None):
    """Add a row to a menu path ("Mesh", "Mesh/Symmetry"). kind: "action" (default with an action), "separator",
    "section" (a separator titled by label) or "submenu" (label names it; its rows use the path "<menu>/<label>"). Returns the item's id."""
    kind = kind or ("action" if action else "submenu" if label else "separator")
    return _call("menu.addItem", menu=menu, action=action or "", label=label or "", kind=kind, id=id, after=after, before=before,
                 owner=_owner(owner))


def add_bar_item(bar="toolBox", action=None, icon="", label="", kind=None, actions=None, id="", after="", before="", owner=None):
    """Add a button to an icon bar: "toolBox" (the left edge of the window), "outliner", or a bar of your own. kind:
    "action" (a button running action), "menu" (a dropdown of the action ids in actions, "" for a separator, "#Title" for
    a section; with an action too, a click runs it and holding opens the menu) or "separator". icon is a Tabler icon name.
    Returns the item's id."""
    kind = kind or ("menu" if actions else "action" if action else "separator")
    return _call("iconBar.addItem", bar=bar, action=action or "", icon=icon, label=label, kind=kind, actions=list(actions or []), id=id,
                 after=after, before=before, owner=_owner(owner))


def remove_bar_item(id):
    return _call("iconBar.removeItem", id=id)


def bar_items(bar="toolBox"):
    """The items of an icon bar in display order: [{id, action, icon, label, owner}]."""
    return _call("iconBar.list", bar=bar)


def remove_menu(id):
    return _call("menu.remove", id=id)


def remove_menu_item(id):
    return _call("menu.removeItem", id=id)


def add_panel(id, build, title=None, area="right", icon="", open=True, docs="", owner=None):
    """A dockable panel. build() returns a QWidget or a Column/Row; it is called when the dock is made. A
    window.<id> action toggles it. docs names a docs page ("topology", "curves?id=pipes-and-hoses") that a help button
    in the panel's title bar opens."""
    QtCore, QtWidgets = _qt()
    from shiboken6 import wrapInstance

    def fill(pointer):
        holder = wrapInstance(int(pointer), QtWidgets.QWidget)
        content = build()
        holder.layout().addWidget(content.build() if isinstance(content, _Node) else content)

    return _call("panel.add", [fill], id=id, title=title or id, area=area, icon=icon, open=open, docs=docs, owner=_owner(owner))


def remove_panel(id, owner=None):
    return _call("panel.remove", id=id, owner=_owner(owner))


def add_mode(id, label=None, doc="", layout="", hotkeys=None, marking_menu_set="", shelf="", gizmo_provider="",
             selection_handler="", on_enter=None, on_exit=None, icon="", owner=None):
    """A mode: entering it applies its layout, hotkey layer, marking-menu set and shelf, then calls on_enter().
    hotkeys is shaped like the hotkeys prefs' "contexts"; naming only "press" or only "hold" for a key keeps its
    other slots, so {"viewport": {"Space": {"press": "my.action"}}} rebinds Space's tap and keeps the hotbox."""
    return _call("mode.add", [on_enter, on_exit], id=id, label=label or id, doc=doc, layout=layout, hotkeys=hotkeys or {},
                 markingMenuSet=marking_menu_set, shelf=shelf, gizmoProvider=gizmo_provider, selectionHandler=selection_handler,
                 icon=icon, owner=_owner(owner))


def remove_mode(id):
    return _call("mode.remove", id=id)


def set_mode(id):
    """Switch modes. Returns False for an unknown id."""
    return _call("mode.activate", id=id)


def mode():
    """The current mode's id."""
    return _call("mode.current")


def modes():
    """Every mode as a dict."""
    return _call("mode.list")


def add_layout(name, layout, owner=None):
    """A read-only layout (shaped like a saved one, or {"preset": {...}}), with a layout.apply.<name> action."""
    return _call("layout.add", name=name, layout=layout, owner=_owner(owner))


def remove_layout(name):
    return _call("layout.remove", name=name)


def add_marking_menu(name, definition, owner=None):
    """A marking menu ("set/name" puts it in a set), as defaults the Marking Menu Editor can still change."""
    return _call("markingMenu.add", name=name, definition=definition, owner=_owner(owner))


def remove_marking_menu(name):
    return _call("markingMenu.remove", name=name)


def add_marking_menu_set(set, menus, owner=None):
    """Every menu of a set: {name: definition}. A mode whose marking_menu_set is `set` shows these instead."""
    return _call("markingMenu.addSet", set=set, menus=menus, owner=_owner(owner))


def remove_marking_menu_set(set):
    return _call("markingMenu.removeSet", set=set)


def add_marking_menu_item(menu, item, slot="overflow", owner=None):
    """Add {"label", "action"} to an existing marking menu, in a compass slot ("N" ... "NW") or the overflow list."""
    return _call("markingMenu.addItem", menu=menu, item=item, slot=slot, owner=_owner(owner))


def remove_marking_menu_item(id):
    return _call("markingMenu.removeItem", id=id)


def add_hotkey_layer(id, contexts, mode="", owner=None):
    """Default key bindings, {"global": {"Ctrl+K": "my.action"}}; with a mode, only while it is on."""
    return _call("hotkeyLayer.add", id=id, contexts=contexts, mode=mode, owner=_owner(owner))


def remove_hotkey_layer(id):
    return _call("hotkeyLayer.remove", id=id)


def add_shelf(name, items, owner=None):
    """A shelf of buttons [{"label", "icon", "target", "tooltip"}], as defaults the user can still edit."""
    return _call("shelf.add", name=name, items=items, owner=_owner(owner))


def remove_shelf(name):
    return _call("shelf.remove", name=name)


def add_preferences_page(id, title, rows, file="general", build=None, owner=None):
    """A Preferences tab whose rows the search finds. rows: {"section": "Title"} or {"type": "toggle" | "number" |
    "choice" | "text" | "color", "label", "key", "default", ...} ("number" takes min, max, step, decimals; "choice"
    takes options [[text, value]]). build() may return one more QWidget for anything the rows can't express."""
    fill = None
    if build is not None:
        QtCore, QtWidgets = _qt()
        from shiboken6 import wrapInstance

        def fill(pointer):
            wrapInstance(int(pointer), QtWidgets.QWidget).layout().addWidget(build())

    return _call("prefPage.add", [fill], id=id, title=title, file=file, rows=rows, owner=_owner(owner))


def remove_preferences_page(id):
    return _call("prefPage.remove", id=id)


def remove_owned(owner):
    """Remove everything owner registered in the UI and on the event bus."""
    return _call("owner.removeAll", owner=owner)
