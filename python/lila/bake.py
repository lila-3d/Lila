"""Baker helpers: run bakes and read the written maps as numpy arrays.

    import lila.bake as bake
    r = bake.bake_for_target("substancePainter")
    print(bake.miss_rate(r), r["warnings"])
    n = bake.load("normal")                 # (H, W, 4) float32, row 0 is the top scanline
    h = bake.load("height", udim=1001)

Maps are read straight from the files the last bake wrote (PNG 8/16 bit, OpenEXR half, TGA), so this works on any bake
this session, from the panel, a script or an agent. Every function that changes the project runs a command, so it is
undoable and echoed like the Baker panel's edits.
"""

import os
import struct
import zlib

from . import ops

__all__ = ["project", "projects", "status", "bake", "bake_for_target", "last", "outputs", "output", "load", "read", "miss_rate",
           "group_stats", "targets", "apply_target", "save_target", "delete_target", "maps", "lint"]


def projects():
    """Names of every bake project in the scene."""
    s = ops.bakeStatus()
    if "projects" in s:
        return [p["name"] for p in s["projects"]]
    return [s["name"]]


def project(name=None, target=None, folder=None):
    """The named project, else the scene's only one; created (with target's maps and naming) when the scene has none."""
    if name is not None:
        try:
            return ops.bakeStatus(project=name)["name"]
        except Exception:
            pass
    else:
        s = ops.bakeStatus()
        if "name" in s:
            return s["name"]
        if s.get("projects"):
            raise ValueError("several bake projects; name one")
    flags = {"name": name or ""}
    if target:
        flags["target"] = target
    if folder:
        flags["folder"] = folder
    return ops.createBakeProject(**flags)


def status(project=None):
    """Settings, groups and the last result of a project (bakeStatus)."""
    return ops.bakeStatus(project=project) if project else ops.bakeStatus()


def bake(project=None, groups=None, maps=None, dry_run=False, backend="cpu", wait=True):
    """Bakes and returns the result: {outputs, stats, warnings, manifest, seconds, timings, backendMode}.

    wait=False returns at once ({async: True}) and bakes off the main thread; last() holds the result once it lands.
    """
    flags = {"dryRun": dry_run, "backend": backend}
    if not wait:
        flags["async"] = True
    if project:
        flags["project"] = project
    if groups:
        flags["groups"] = list(groups)
    if maps:
        flags["maps"] = list(maps)
    return ops.bake(**flags)


def bake_for_target(target, project=None, optional=False, dry_run=False, backend="cpu", wait=True):
    """Applies a target profile (unreal, substancePainter, ...), auto-groups when needed, bakes; returns the result (wait as bake)."""
    flags = {"target": target, "optional": optional, "dryRun": dry_run, "backend": backend}
    if not wait:
        flags["async"] = True
    if project:
        flags["project"] = project
    return ops.bakeForTarget(**flags)


def cancel():
    """Stops the running bake; True when one was running."""
    return ops.bakeCancel()


def last(project=None):
    """The last bake result of a project this session, or None."""
    return status(project).get("last")


def outputs(result=None, project=None):
    """The output files of a result (default: the project's last bake): dicts with file, map, udim, width, height, meta, ..."""
    r = result if result is not None else last(project)
    return list(r.get("outputs", [])) if r else []


def output(map, result=None, project=None, udim=None, mesh=None, group=None):
    """The first output of a map, optionally for one UDIM, mesh or group. None when there is none."""
    for o in outputs(result, project):
        if o["map"] != map:
            continue
        if udim is not None and o["udim"] != udim:
            continue
        if mesh is not None and not any(m == mesh or m.split("|")[-1] == mesh for m in o.get("meshes", [])):
            continue
        if group is not None and not any(g == group or g.split("|")[-1] == group for g in o.get("groups", [])):
            continue
        return o
    return None


def load(map, result=None, project=None, udim=None, mesh=None, group=None):
    """A baked map as an (H, W, 4) float32 array, row 0 at the top. Raises LookupError when it was not baked."""
    o = output(map, result, project, udim, mesh, group)
    if o is None:
        raise LookupError("no baked output for map %r" % map)
    return read(o["file"])


def miss_rate(result=None, project=None):
    """Fraction of projection rays that hit nothing (0 to 1) across every group of a result."""
    r = result if result is not None else last(project)
    s = (r or {}).get("stats", {})
    return s.get("misses", 0) / s["samples"] if s.get("samples") else 0.0


def group_stats(result=None, project=None):
    """Per group {group: {samples, hits, misses, backfaces, highTriangles, lowTriangles, missRate}}."""
    r = result if result is not None else last(project)
    out = {}
    for g in (r or {}).get("stats", {}).get("groups", []):
        g = dict(g)
        g["missRate"] = g["misses"] / g["samples"] if g.get("samples") else 0.0
        out[g["group"]] = g
    return out


def targets():
    """Target profile names: unreal, substancePainter and any user or workspace ones."""
    return [t["name"] for t in ops.bakeTargets()]


def apply_target(target, project=None, optional=False, override_groups=False):
    """Applies a target or saved preset's maps, output and group defaults without baking; returns {target, backend, skipped}.

    Group settings changed by hand keep their values unless override_groups=True.
    """
    flags = {"optional": optional, "overrideGroups": override_groups}
    if project:
        flags["project"] = project
    return ops.applyBakeTarget(target, **flags)


def save_target(name, label=None, scope="user", project=None, overwrite=False, group=None, backend=None):
    """Saves the project's bake settings (no meshes) as a preset target; scope "user" or "workspace". Returns {name, file}."""
    flags = {"scope": scope, "overwrite": overwrite}
    for key, value in (("label", label), ("project", project), ("group", group), ("backend", backend)):
        if value is not None:
            flags[key] = value
    return ops.saveBakeTarget(name, **flags)


def delete_target(name, scope=None):
    """Deletes a saved preset's file; built-in targets can't be deleted. Returns the file."""
    return ops.deleteBakeTarget(name, scope=scope) if scope else ops.deleteBakeTarget(name)


def maps():
    """Map ids the baker implements."""
    return [m["id"] for m in ops.bakeMaps()]


def lint(project=None):
    """bakeLint's findings, or None when this build has no bakeLint."""
    if not hasattr(ops, "bakeLint"):
        return None
    return ops.bakeLint(project=project) if project else ops.bakeLint()


# Readers for what the baker writes, numpy only.

def read(path):
    """An image file as an (H, W, 4) float32 array: PNG (8/16 bit, any colour type), OpenEXR (half or float, scanline,
    uncompressed/ZIPS/ZIP) or uncompressed TGA."""
    ext = os.path.splitext(path)[1].lower()
    with open(path, "rb") as f:
        data = f.read()
    if ext == ".png":
        return _read_png(data)
    if ext == ".exr":
        return _read_exr(data)
    if ext == ".tga":
        return _read_tga(data)
    raise ValueError("can't read %s images" % ext)


def _unfilter(np, raw, rows, stride, bpp):
    out = np.empty((rows, stride), dtype=np.uint8)
    prev = np.zeros(stride, dtype=np.int32)
    lines = raw.reshape(rows, stride + 1)
    for y in range(rows):
        kind = lines[y, 0]
        cur = lines[y, 1:].astype(np.int32)
        if kind == 1:
            for c in range(bpp):
                cur[c::bpp] = np.cumsum(cur[c::bpp]) & 0xFF
        elif kind == 2:
            cur = (cur + prev) & 0xFF
        elif kind in (3, 4):
            # Average and Paeth depend on the pixel just written; done pixel by pixel.
            row = cur.copy()
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                b = prev[x]
                if kind == 3:
                    row[x] = (row[x] + ((a + b) >> 1)) & 0xFF
                else:
                    c = prev[x - bpp] if x >= bpp else 0
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                    row[x] = (row[x] + pred) & 0xFF
            cur = row
        out[y] = cur
        prev = cur
    return out


def _read_png(data):
    import numpy as np
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    pos, idat, w = 8, [], 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, bits, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
        pos += 12 + n
    if not w or interlace:
        raise ValueError("unsupported PNG (interlaced or no header)")
    if ctype == 3 or bits < 8:
        raise ValueError("palette and low bit depth PNGs are not supported")
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    bpp = channels * bits // 8
    raw = np.frombuffer(zlib.decompress(b"".join(idat)), dtype=np.uint8)
    px = _unfilter(np, raw, h, w * bpp, bpp)
    if bits == 16:
        v = px.reshape(h, w * channels, 2).astype(np.uint16)
        px = ((v[..., 0] << 8) | v[..., 1]).astype(np.float32) / 65535.0
    else:
        px = px.astype(np.float32) / 255.0
    px = px.reshape(h, w, channels)
    out = np.ones((h, w, 4), dtype=np.float32)
    if channels in (1, 2):
        out[..., :3] = px[..., :1]
        if channels == 2:
            out[..., 3] = px[..., 1]
    else:
        out[..., :channels] = px
    return out


def _read_exr(data):
    import numpy as np
    if data[:4] != b"\x76\x2f\x31\x01":
        raise ValueError("not an OpenEXR file")
    pos, chans, comp, window = 8, [], 0, None
    while data[pos] != 0:
        end = data.index(b"\0", pos)
        name = data[pos:end].decode()
        end2 = data.index(b"\0", end + 1)
        size = struct.unpack("<i", data[end2 + 1:end2 + 5])[0]
        value = data[end2 + 5:end2 + 5 + size]
        if name == "channels":
            p = 0
            while value[p] != 0:
                e = value.index(b"\0", p)
                cname = value[p:e].decode()
                ptype = struct.unpack("<i", value[e + 1:e + 5])[0]
                chans.append((cname, ptype))
                p = e + 17
        elif name == "compression":
            comp = value[0]
        elif name == "dataWindow":
            window = struct.unpack("<iiii", value)
        pos = end2 + 5 + size
    pos += 1
    if comp not in (0, 2, 3):
        raise ValueError("only uncompressed, ZIPS and ZIP OpenEXR files are supported")
    x0, y0, x1, y1 = window
    w, h = x1 - x0 + 1, y1 - y0 + 1
    lines = 16 if comp == 3 else 1
    blocks = (h + lines - 1) // lines
    offsets = struct.unpack("<%dQ" % blocks, data[pos:pos + 8 * blocks])
    sizes = [4 if t == 2 else (2 if t == 1 else 4) for _, t in chans]
    types = [np.float32 if t == 2 else (np.float16 if t == 1 else np.uint32) for _, t in chans]
    planes = {name: np.zeros((h, w), dtype=np.float32) for name, _ in chans}
    for off in offsets:
        y, n = struct.unpack("<ii", data[off:off + 8])
        block = data[off + 8:off + 8 + n]
        count = min(lines, y1 + 1 - y)
        expected = count * w * sum(sizes)
        if comp and n < expected:
            t = np.frombuffer(zlib.decompress(block), dtype=np.uint8).astype(np.int32)
            t = np.cumsum(t - 128 * (np.arange(t.size) > 0)) & 0xFF
            t = t.astype(np.uint8)
            half = (t.size + 1) // 2
            raw = np.empty_like(t)
            raw[0::2] = t[:half]
            raw[1::2] = t[half:]
            block = raw.tobytes()
        p = 0
        for row in range(count):
            for (name, _), size, typ in zip(chans, sizes, types):
                planes[name][y - y0 + row] = np.frombuffer(block[p:p + w * size], dtype=typ).astype(np.float32)
                p += w * size
    out = np.ones((h, w, 4), dtype=np.float32)
    for i, c in enumerate("RGBA"):
        if c in planes:
            out[..., i] = planes[c]
    if "Y" in planes and "R" not in planes:
        out[..., :3] = planes["Y"][..., None]
    return out


def _read_tga(data):
    import numpy as np
    idlen, _, kind = data[0], data[1], data[2]
    if kind != 2:
        raise ValueError("only uncompressed true colour TGA files are supported")
    w, h, bpp, desc = struct.unpack("<HHBB", data[12:18])
    c = bpp // 8
    px = np.frombuffer(data[18 + idlen:18 + idlen + w * h * c], dtype=np.uint8).reshape(h, w, c).astype(np.float32) / 255.0
    if not desc & 0x20:
        px = px[::-1]
    out = np.ones((h, w, 4), dtype=np.float32)
    out[..., 0], out[..., 1], out[..., 2] = px[..., 2], px[..., 1], px[..., 0]
    if c == 4:
        out[..., 3] = px[..., 3]
    return out
