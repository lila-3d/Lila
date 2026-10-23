"""Object API over the scene: handles that survive renames, attributes as properties.

    loc = locator("Target", position=(0, 10, 0))
    loc.translate = (5, 0, 0)
    loc.rotate_y += 45
    grp = group(loc, name="Rig")
    for n in selected():
        print(n.name, n.world_position)

Every edit runs a command, so it is undoable and echoes like UI actions do.
"""

import re

import _lila

from . import ops

__all__ = ["Node", "node", "nodes", "selected", "select", "locator", "group", "mesh", "exists", "display_layers", "display_layer", "object_sets",
           "object_set"]

_CAMEL = re.compile(r"_([a-z])")


def _camel(name):
    """rotate_order -> rotateOrder; camelCase names pass through."""
    return _CAMEL.sub(lambda m: m.group(1).upper(), name)


def _vector(value):
    try:
        from .math import Vector

        return Vector(*value)
    except Exception:
        return tuple(value)


class Node:
    """A scene node. Holds the node's id, so it keeps working after a rename."""

    __slots__ = ("_id",)

    def __init__(self, name_or_id):
        if isinstance(name_or_id, Node):
            self._id = name_or_id._id
            return
        if isinstance(name_or_id, int):
            if _lila.node_name(name_or_id) is None:
                raise LookupError(f"no node with id {name_or_id}")
            self._id = name_or_id
            return
        node_id = _lila.node_id(str(name_or_id))
        if node_id is None:
            raise LookupError(f"no node named '{name_or_id}'")
        self._id = node_id

    # identity

    @property
    def id(self):
        return self._id

    @property
    def exists(self):
        return _lila.node_name(self._id) is not None

    @property
    def name(self):
        name = _lila.node_name(self._id)
        if name is None:
            raise LookupError(f"node {self._id} was deleted")
        return name

    @name.setter
    def name(self, value):
        ops.rename(self.name, value)

    @property
    def type(self):
        return _lila.node_type(self._id)

    def is_a(self, type_name):
        return _lila.node_is_a(self._id, type_name)

    def __eq__(self, other):
        return isinstance(other, Node) and other._id == self._id

    def __hash__(self):
        return hash(self._id)

    def __repr__(self):
        return f"Node('{_lila.node_name(self._id) or '<deleted>'}')"

    def __str__(self):
        return self.name

    # hierarchy

    @property
    def parent(self):
        p = _lila.node_parent(self._id)
        return Node(p) if p is not None else None

    @parent.setter
    def parent(self, value):
        if value is None:
            ops.parent(self.name, world=True)
        else:
            ops.parent(self.name, Node(value).name)

    @property
    def children(self):
        return [Node(i) for i in _lila.node_children(self._id)]

    def descendants(self):
        out = []
        stack = list(reversed(self.children))
        while stack:
            n = stack.pop()
            out.append(n)
            stack.extend(reversed(n.children))
        return out

    # attributes

    def attrs(self, keyable=None):
        """Attribute names, optionally only keyable (channel box) ones."""
        return [a["name"] for a in _lila.node_attrs(self._id) if keyable is None or a["keyable"] == keyable]

    def get(self, attr):
        value = ops.getAttr(f"{self.name}.{_camel(attr)}")
        return _vector(value) if isinstance(value, list) and len(value) == 3 else value

    def set(self, attr, value):
        if hasattr(value, "x") and hasattr(value, "y") and hasattr(value, "z"):
            value = [value.x, value.y, value.z]
        elif isinstance(value, tuple):
            value = list(value)
        ops.setAttr(f"{self.name}.{_camel(attr)}", value)

    def lock(self, *attrs, locked=True):
        """Lock attributes (translate locks all three of tx/ty/tz) so nothing can change them; locked=False unlocks."""
        for attr in attrs:
            ops.setAttr(f"{self.name}.{_camel(attr)}", lock=locked)

    def unlock(self, *attrs):
        self.lock(*attrs, locked=False)

    def is_locked(self, attr):
        return ops.getAttr(f"{self.name}.{_camel(attr)}", lock=True)

    def has_attr(self, attr):
        try:
            ops.getAttr(f"{self.name}.{_camel(attr)}")
            return True
        except Exception:
            return False

    def __getitem__(self, attr):
        return self.get(attr)

    def __setitem__(self, attr, value):
        self.set(attr, value)

    def __getattr__(self, attr):
        if attr.startswith("__"):
            raise AttributeError(attr)
        try:
            return self.get(attr)
        except Exception:
            raise AttributeError(f"{self!r} has no attribute '{attr}'") from None

    def __setattr__(self, attr, value):
        if attr in Node.__slots__ or isinstance(getattr(type(self), attr, None), property):
            object.__setattr__(self, attr, value)
        else:
            self.set(attr, value)

    def __dir__(self):
        return sorted(set(super().__dir__()) | set(self.attrs()))

    # transforms

    @property
    def world_position(self):
        """World position of the rotate pivot."""
        return _vector(ops.pivot(self.name))

    @world_position.setter
    def world_position(self, value):
        ops.move(self.name, to=list(value))

    @property
    def world_matrix(self):
        return self.get("worldMatrix")

    def move(self, x=0.0, y=0.0, z=0.0, space="world"):
        ops.move(self.name, by=[x, y, z], space=space)

    def rotate_by(self, x=0.0, y=0.0, z=0.0, space="world"):
        ops.rotate(self.name, by=[x, y, z], space=space)

    def scale_by(self, x=1.0, y=None, z=None):
        y = x if y is None else y
        z = x if z is None else z
        ops.scale(self.name, by=[x, y, z])

    def set_pivot(self, position):
        ops.setPivot(self.name, position=list(position))

    def reset_pivot(self):
        ops.resetPivot(self.name)

    # meshes

    @property
    def points(self):
        """Final (modified) mesh points in object space, as lila.math.Vector."""
        return [_vector(p) for p in ops.meshData(self.name)["points"]]

    @property
    def faces(self):
        """Faces as lists of point indices."""
        return ops.meshData(self.name)["faces"]

    def mesh_info(self):
        return ops.meshInfo(self.name)

    # vertex colors

    @property
    def color_sets(self):
        """Color set names in order; the first is the vertex color exporters write first (Unreal's)."""
        return ops.polyColorSet(self.name, query=True)["sets"]

    @property
    def current_color_set(self):
        """The set the viewport shows and paint tools edit, or "" without any."""
        return ops.polyColorSet(self.name, query=True)["current"]

    @current_color_set.setter
    def current_color_set(self, name):
        ops.polyColorSet(self.name, colorSet=name, current=True)

    def add_color_set(self, name="", fill=(1.0, 1.0, 1.0, 1.0)):
        """Adds a color set filled with a linear RGBA color; returns its name."""
        before = set(self.color_sets)
        sets = ops.polyColorSet(self.name, create=True, colorSet=name, fill=list(fill))["sets"]
        return next((s for s in sets if s not in before), name)

    def get_colors(self, color_set="", base=True):
        """(N, 4) float32 numpy array of linear RGBA, one row per face-vertex (faces in order, corners around each face)."""
        fn = getattr(_lila, "color_set_array", None)
        if fn is None:
            import numpy as np
            flat = ops.getVertexColorData(self.name, colorSet=color_set, base=base)
            return np.asarray(flat, dtype=np.float32).reshape(-1, 4)
        return fn(self.name, color_set, base)

    def set_colors(self, values, color_set=""):
        """Sets a color set from an (N, 4) array-like in get_colors' order; one undo step. Creates the set when missing."""
        import numpy as np
        arr = np.ascontiguousarray(values, dtype=np.float32).reshape(-1, 4)
        fn = getattr(_lila, "set_color_set_array", None)
        if fn is None:
            ops.setVertexColorData(self.name, colorSet=color_set, values=arr.ravel().tolist())
        else:
            fn(self.name, arr, color_set)

    # display layers

    @property
    def display_layer(self):
        """The display layer this node is in (a Node), or None."""
        for info in ops.listDisplayLayers():
            if self.name in info["members"]:
                return Node(info["name"])
        return None

    @display_layer.setter
    def display_layer(self, layer):
        """Moves the node into a layer (Node or name); None takes it out of its layer."""
        if layer is None:
            ops.removeFromDisplayLayer(self.name)
        else:
            ops.editDisplayLayerMembers(Node(layer).name, self.name)

    # object sets

    @property
    def sets(self):
        """The object sets holding this node, whole or by its components (Nodes)."""
        return [Node(s) for s in ops.listSets(object=self.name)]

    @property
    def members(self):
        """An object set's members: names and component strings like "pCube1.vtx[0:3]"."""
        return ops.setMembers(self.name)

    @property
    def member_nodes(self):
        """The nodes an object set touches (component owners included), as Nodes."""
        return [Node(n) for n in ops.setMembers(self.name, nodes=True)]

    def add_members(self, *items):
        """Adds Nodes, names or component strings to this object set."""
        ops.addToSet(self.name, *[_member(i) for i in items])

    def remove_members(self, *items):
        """Takes Nodes, names or component strings out of this object set."""
        ops.removeFromSet(self.name, *[_member(i) for i in items])

    def select_members(self, add=False):
        """Selects what this object set holds: its objects, and its components in component mode."""
        ops.selectSetMembers(self.name, add=add)

    # lifetime / selection

    def delete(self):
        ops.delete(self.name)

    def select(self, add=False):
        if add:
            ops.select(self.name, add=True)
        else:
            ops.select(self.name)


def node(name):
    """Node by name. Raises LookupError if there is none."""
    return Node(name)


def exists(name):
    return _lila.node_id(str(name)) is not None


def nodes(type=None):
    """Every node, optionally only those of (or derived from) a type."""
    out = [Node(i) for i in _lila.all_node_ids()]
    return [n for n in out if type is None or n.is_a(type)]


def selected(type=None):
    """The selection in selection order."""
    out = [Node(i) for i in _lila.selection_ids()]
    return [n for n in out if type is None or n.is_a(type)]


def select(*items, add=False):
    """Select nodes (Node handles or names). No arguments clears the selection."""
    names = [Node(i).name for i in items]
    if not names:
        ops.select(clear=True)
    elif add:
        ops.select(*names, add=True)
    else:
        ops.select(*names)


def locator(name=None, position=None):
    flags = {}
    if name:
        flags["name"] = name
    if position is not None:
        flags["position"] = list(position)
    return Node(ops.spaceLocator(**flags))


def mesh(points, faces, name=None):
    """A new mesh from points ([x, y, z] or Vector) and faces (lists of point indices)."""
    pts = [[p[0], p[1], p[2]] for p in points]
    flags = {"name": name} if name else {}
    return Node(ops.createMesh(points=pts, faces=[list(f) for f in faces], **flags))


def display_layers():
    """Every display layer, in Display Layers panel order."""
    return [Node(info["name"]) for info in ops.listDisplayLayers()]


def display_layer(*items, name=None):
    """A new display layer holding the items (Node handles or names); none makes an empty one."""
    flags = {"name": name} if name else {}
    names = [Node(i).name for i in items]
    if not names:
        flags["empty"] = True
    return Node(ops.createDisplayLayer(*names, **flags))


def _member(item):
    return item.name if isinstance(item, Node) else str(item)


def object_sets(quick_select=None):
    """Every object set in creation order; quick_select=True only quick select sets, False only plain ones."""
    flags = {} if quick_select is None else {"quickSelect": quick_select}
    return [Node(n) for n in ops.listSets(**flags)]


def object_set(*items, name=None, quick_select=False):
    """A new object set holding the items (Nodes, names or component strings like "pCube1.f[0:3]"); none makes an empty one."""
    flags = {"quickSelect": quick_select}
    if name:
        flags["name"] = name
    members = [_member(i) for i in items]
    if not members:
        flags["empty"] = True
    return Node(ops.createSet(*members, **flags))


def group(*items, name=None):
    flags = {"name": name} if name else {}
    names = [Node(i).name for i in items]
    return Node(ops.group(*names, **flags))
