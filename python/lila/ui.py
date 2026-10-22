"""UI from Python: run actions, switch tools, and build tool panels.

Panels are plain PySide6 underneath, with a small declarative layer on top: nest Column/Row, bind widgets straight to
attributes, and decorate a function with @panel to get a dockable window.

    from lila.ui import panel, Column, Row, Label, Button, Slider, show_panel

    @panel("Wheel Rig", area="right")
    def wheel_rig():
        return Column(
            Label("Wheel"),
            Slider("Wheel.rotateX", minimum=-360, maximum=360),
            Row(Button("Key", lambda: lila.cmds.setKeyframe("Wheel")), Button("Reset", reset)),
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


def _cmds():
    from . import cmds

    return cmds


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
        return _cmds().getAttr(self.plug) if self.plug else None

    def _set(self, value):
        if self.plug:
            _cmds().setAttr(self.plug, value)
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
