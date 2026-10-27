"""Sample Python file translator: reads Blender 5.0's default scene (a cube, a camera and a point light in a collection).

Copy it into an extension folder (e.g. <workspace>/.lila/extensions/) or add this folder to general.json /extensions/paths, then:

    lila.ops.loadExtension("blend_scene_py")

File > Open and Import then take .blend files. Needs Python 3.14 for compressed .blend files (compression.zstd).
"""

import math
import re
import struct

import lila
import lila.math
from lila import extension, nodes, ops

try:
    from compression import zstd
except ImportError:
    zstd = None

CM_PER_METRE = 100.0

_PRIMITIVES = {"char": "b", "uchar": "B", "int8_t": "b", "uint8_t": "B", "short": "h", "ushort": "H",
               "int": "i", "uint": "I", "int64_t": "q", "uint64_t": "Q", "float": "f", "double": "d"}


# Reading the file


class Struct:
    """One struct in the file. Fields are read by name; pointers come back as addresses for Blend.deref."""

    def __init__(self, blend, type_name, data, offset=0):
        self._blend, self._type, self._data, self._offset = blend, type_name, data, offset

    def __getattr__(self, name):
        field = self._blend.fields(self._type).get(name)
        if field is None:
            raise AttributeError(f"{self._type} has no field {name}")
        return self._blend.read_field(self._data, self._offset, field)


class Blend:
    """A .blend: its blocks, and the struct layouts (SDNA) the file describes itself with."""

    def __init__(self, path):
        raw = open(path, "rb").read()
        if raw[:4] == b"\x28\xb5\x2f\xfd":
            if zstd is None:
                raise lila.CommandError("compressed .blend files need Python 3.14 or later")
            raw = zstd.decompress(raw)
        if not raw.startswith(b"BLENDER17-01v"):
            raise lila.CommandError("not a Blender 5.0 file")
        self.blocks = []  # (code, struct index, address, data)
        pos = 17
        while pos < len(raw):
            code, sdna, address, size, _count = struct.unpack_from("<4sIQQQ", raw, pos)
            pos += 32
            code = code.rstrip(b"\0").decode()
            if code == "ENDB":
                break
            self.blocks.append((code, sdna, address, raw[pos:pos + size]))
            pos += size
        self._by_address = {b[2]: b for b in self.blocks}
        self._read_sdna(next(b[3] for b in self.blocks if b[0] == "DNA1"))
        self._fields = {}

    def _read_sdna(self, d):
        pos = 8  # "SDNANAME"

        def strings():
            nonlocal pos
            (n,) = struct.unpack_from("<I", d, pos)
            pos += 4
            out = d[pos:].split(b"\0", n)[:n]
            pos += sum(len(s) + 1 for s in out)
            pos = (pos + 3) & ~3
            return [s.decode() for s in out]

        names = strings()
        pos += 4  # "TYPE"
        self.types = strings()
        pos += 4  # "TLEN"
        self.sizes = struct.unpack_from(f"<{len(self.types)}H", d, pos)
        pos = (pos + 2 * len(self.types) + 3) & ~3
        pos += 4  # "STRC"
        (n,) = struct.unpack_from("<I", d, pos)
        pos += 4
        self.structs = []  # (type name, [(member type, member name)])
        for _ in range(n):
            t, count = struct.unpack_from("<HH", d, pos)
            members = struct.unpack_from(f"<{2 * count}H", d, pos + 4)
            pos += 4 + 4 * count
            self.structs.append((self.types[t], [(self.types[members[i]], names[members[i + 1]]) for i in range(0, 2 * count, 2)]))
        self._struct_index = {name: i for i, (name, _) in enumerate(self.structs)}

    def fields(self, type_name):
        """{name: (offset, member type, is pointer, array length)}."""
        if type_name not in self._fields:
            out, offset = {}, 0
            for member_type, name in self.structs[self._struct_index[type_name]][1]:
                pointer = name.startswith("*") or name.startswith("(*")
                count = 1
                for dim in re.findall(r"\[(\d+)\]", name):
                    count *= int(dim)
                out[re.sub(r"[\*\(\)]|\[\d+\]", "", name)] = (offset, member_type, pointer, count)
                offset += (8 if pointer else self.sizes[self.types.index(member_type)]) * count
            self._fields[type_name] = out
        return self._fields[type_name]

    def read_field(self, data, base, field):
        offset, member_type, pointer, count = field
        offset += base
        if pointer:
            values = struct.unpack_from(f"<{count}Q", data, offset)
            return values[0] if count == 1 else list(values)
        if member_type == "char" and count > 1:
            return data[offset:offset + count].split(b"\0", 1)[0].decode("utf-8", "replace")
        if member_type in _PRIMITIVES:
            values = struct.unpack_from(f"<{count}{_PRIMITIVES[member_type]}", data, offset)
            return values[0] if count == 1 else list(values)
        return Struct(self, member_type, data, offset)

    def ids(self, code):
        """Every datablock with this block code ("OB", "ME", "SC") as a Struct."""
        return [Struct(self, self.structs[b[1]][0], b[3]) for b in self.blocks if b[0] == code]

    def deref(self, address, type_name=None):
        block = self._by_address.get(address)
        return Struct(self, type_name or self.structs[block[1]][0], block[3]) if block else None

    def raw(self, address):
        block = self._by_address.get(address)
        return block[3] if block else b""

    def items(self, listbase, type_name):
        out, address = [], listbase.first
        while address:
            item = self.deref(address, type_name)
            out.append(item)
            address = item.next
        return out


def id_name(datablock):
    """The name without Blender's two-letter type prefix ("OBCube" -> "Cube")."""
    return datablock.id.name[2:]


def mesh_attributes(blend, mesh):
    """{name: bytes} of a Blender 5.0 mesh's attributes (position, .corner_vert, UVMap, ...)."""
    storage = mesh.attribute_storage
    table = blend.raw(storage.dna_attributes)
    size = blend.sizes[blend.types.index("Attribute")]
    out = {}
    for i in range(storage.dna_attributes_num):
        a = Struct(blend, "Attribute", table, i * size)
        array = blend.deref(a.data, "AttributeArray")
        out[blend.raw(a.name).split(b"\0", 1)[0].decode()] = blend.raw(array.data)
    return out


# Matrices: lists of rows


def matmul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(len(b))) for c in range(len(b[0]))] for r in range(len(a))]


def transposed(m):
    return [list(row) for row in zip(*m)]


def object_matrix(ob):
    """Blender's location, XYZ Euler rotation and scale as a 4x4, translation in cm."""
    cx, cy, cz = (math.cos(a) for a in ob.rot)
    sx, sy, sz = (math.sin(a) for a in ob.rot)
    rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    r = matmul(rz, matmul(ry, rx))
    m = [[r[i][j] * ob.size[j] for j in range(3)] + [ob.loc[i] * CM_PER_METRE] for i in range(3)]
    return m + [[0, 0, 0, 1]]


def to_lila_axes():
    """Blender is Z up. A Y-up Lila scene turns it -90 degrees about X: (x, y, z) -> (x, z, -y)."""
    if lila.math.up_axis() == "z":
        return [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    return [[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]]


def set_matrix(node, m):
    ops.xform(node.path, matrix=[v for column in transposed(m) for v in column])


# Building the Lila scene


def make_mesh(blend, ob, axes):
    me = blend.deref(ob.data)
    attrs = mesh_attributes(blend, me)
    flat = struct.unpack(f"<{3 * me.totvert}f", attrs["position"])
    points = [matmul(axes, [[flat[i] * CM_PER_METRE], [flat[i + 1] * CM_PER_METRE], [flat[i + 2] * CM_PER_METRE], [1]])
              for i in range(0, len(flat), 3)]
    offsets = struct.unpack(f"<{me.totpoly + 1}i", blend.raw(me.poly_offset_indices))
    corners = struct.unpack(f"<{me.totloop}i", attrs[".corner_vert"])
    faces = [list(corners[offsets[f]:offsets[f + 1]]) for f in range(me.totpoly)]
    uvs = None
    if "UVMap" in attrs:
        uv = struct.unpack(f"<{2 * me.totloop}f", attrs["UVMap"])
        uvs = [[uv[i], uv[i + 1]] for i in range(0, len(uv), 2)]
    node = nodes.Node(ops.createMesh(points=[[p[0][0], p[1][0], p[2][0]] for p in points], faces=faces, uvs=uvs, name=id_name(ob)))
    # Points were turned onto Lila's axes, so the object's matrix turns with them.
    set_matrix(node, matmul(axes, matmul(object_matrix(ob), transposed(axes))))
    return node


def make_camera(blend, ob, axes):
    ca = blend.deref(ob.data)
    node = nodes.Node(ops.createNode("camera", name=id_name(ob), skipSelect=True))
    node.set("focalLength", ca.lens)
    node.set("horizontalFilmAperture", ca.sensor_x / 25.4)
    node.set("verticalFilmAperture", ca.sensor_y / 25.4)
    node.set("filmFit", "vertical" if ca.sensor_fit == 2 else "horizontal")
    node.set("nearClipPlane", ca.clipsta * CM_PER_METRE)
    node.set("farClipPlane", ca.clipend * CM_PER_METRE)
    # Both look down their local -Z, so only the world axes turn.
    set_matrix(node, matmul(axes, object_matrix(ob)))
    return node


def make_light(blend, ob, axes):
    la = blend.deref(ob.data)
    if la.type != 0:
        return None  # only point lights
    node = nodes.Node(ops.light(type="point", name=id_name(ob), color=(la.r, la.g, la.b), intensity=la.energy,
                                units="watts", skipSelect=True))
    node.set("sourceRadius", la.radius * CM_PER_METRE)
    set_matrix(node, matmul(axes, object_matrix(ob)))
    return node


MAKERS = {1: make_mesh, 10: make_light, 11: make_camera}  # Blender's OB_MESH, OB_LAMP, OB_CAMERA


def import_blend(path):
    """Import a Blender 5.0 scene: each collection becomes a group holding its meshes, cameras and point lights."""
    blend = Blend(path)
    axes = to_lila_axes()
    scene = blend.ids("SC")[0]
    made = []

    def add_collection(collection, parent):
        for child in blend.items(collection.children, "CollectionChild"):
            sub = blend.deref(child.collection)
            group = nodes.Node(ops.group(empty=True, name=id_name(sub)))
            if parent:
                group.parent = parent
            made.append(group.path)
            add_collection(sub, group)
        for entry in blend.items(collection.gobject, "CollectionObject"):
            ob = blend.deref(entry.ob)
            make = MAKERS.get(ob.type)
            node = make(blend, ob, axes) if make else None
            if node is None:
                print(f"{path}: skipped {id_name(ob)}")
                continue
            if parent:
                node.parent = parent
            made.append(node.path)

    add_collection(blend.deref(scene.master_collection), None)
    return made


def initialize_extension():
    extension.register_command("importBlendPy", import_blend, positional="path")
    extension.register_file_translator("blendPy", [".blend"], label="Blender scenes",
                                       doc="Blender 5.0 scenes: meshes, cameras and point lights, by collection.",
                                       import_="importBlendPy")


def uninitialize_extension():
    extension.unregister_file_translator("blendPy")
    extension.unregister_command("importBlendPy")
