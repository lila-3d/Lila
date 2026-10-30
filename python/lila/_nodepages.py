"""Writes the node reference pages of the docs site (docs/nodes/) from the geometry node and texture graph registries,
so every node's description, sockets and properties match what the Node Editor shows.

    lila.exe -b -c "import lila._nodepages as p; p.write(r'X:/Lila-3D/docs')"

lila._apipages.write runs it too. Every .md under docs/nodes/ is generated: fix the text in the node declarations.
"""

import os

from . import ops
from ._apipages import _HEADER, _cell, _flat, _write_if_changed

FOLDER = "nodes"


def write(docs_dir):
    """Rewrites docs_dir/nodes/ and removes pages that are no longer generated. Returns the paths written."""
    out_dir = os.path.join(docs_dir, FOLDER)
    os.makedirs(out_dir, exist_ok=True)
    pages = render()
    written = []
    for name, text in pages.items():
        path = os.path.join(out_dir, name)
        _write_if_changed(path, text)
        written.append(path)
    for name in os.listdir(out_dir):
        if name.endswith(".md") and name not in pages:
            os.remove(os.path.join(out_dir, name))
    return written


def render():
    """{file name: markdown} for every page."""
    return {
        "geometry-nodes.md": _page(
            "Geometry node reference",
            "Every geometry node, by menu category. Diamond sockets in the editor take or give fields (kind *field* here). "
            "Inputs left unlinked use the default shown, or the implicit input named (Position, Normal, Index, ID).",
            ops.gnNodeTypes(detail=True),
            _gn_node,
        ),
        "texture-nodes.md": _page(
            "Texture node reference",
            "Every texture graph node, by menu category.",
            ops.texNodeTypes(detail=True),
            _tex_node,
        ),
    }


def _page(title, intro, nodes, render_node):
    groups = {}
    for n in nodes:
        groups.setdefault(n.get("owner") or "", {}).setdefault(n.get("category") or "Other", []).append(n)
    lines = [_HEADER, "", f"# {title}", "", intro, ""]
    for owner in sorted(groups, key=lambda o: (o != "", o.lower())):
        level = "##"
        if owner:
            lines += [f"## {owner} extension", "", f"Available when the {owner} extension is loaded.", ""]
            level = "###"
        for category in sorted(groups[owner], key=str.lower):
            lines += [f"{level} {category}", ""]
            for n in sorted(groups[owner][category], key=lambda n: (n.get("label") or n["type"]).lower()):
                lines += render_node(n, level + "#")
    return "\n".join(lines)


def _heading(n, level):
    label = n.get("label") or n["type"]
    lines = [f"{level} {label}", ""]
    if label.replace(" ", "") != n["type"]:
        lines += [f"Type `{n['type']}`.", ""]
    if n.get("doc"):
        lines += [_flat(n["doc"]), ""]
    return lines


def _gn_node(n, level):
    lines = _heading(n, level)
    rows = []
    for s in n.get("inputs") or []:
        kind = "input, field" if s.get("field") else "input"
        if s.get("multi"):
            kind += ", multi"
        default = _default(s.get("default"))
        if s.get("implicit"):
            default = s["implicit"].capitalize()
        rows.append((s["id"], s["type"], kind, default, _socket_doc(s)))
    for s in n.get("outputs") or []:
        rows.append((s["id"], s["type"], "output, field" if s.get("field") else "output", "", _flat(s.get("doc"))))
    lines += _socket_table(rows)
    lines += _prop_table(n.get("props") or [])
    return lines


def _tex_node(n, level):
    lines = _heading(n, level)
    rows = []
    for s in n.get("inputs") or []:
        rows.append((s["id"], s["type"], "input", _default(s.get("default")), _flat(s.get("doc"))))
    for s in n.get("outputs") or []:
        rows.append((s["id"], s["type"], "output", "", _flat(s.get("doc"))))
    lines += _socket_table(rows)
    lines += _prop_table(n.get("params") or [])
    return lines


def _socket_doc(s):
    doc = _flat(s.get("doc"))
    lo, hi = s.get("min"), s.get("max")
    if lo is not None and hi is not None:
        doc += f" Range {_number(lo)} to {_number(hi)}."
    elif lo is not None:
        doc += f" At least {_number(lo)}."
    elif hi is not None:
        doc += f" At most {_number(hi)}."
    return doc.strip()


def _socket_table(rows):
    if not rows:
        return []
    lines = ["| Socket | Type | Kind | Default | Description |", "| --- | --- | --- | --- | --- |"]
    for sid, stype, kind, default, doc in rows:
        lines.append(f"| `{sid}` | {stype} | {kind} | {default} | {_cell(doc)} |")
    return lines + [""]


def _prop_table(props):
    if not props:
        return []
    lines = ["| Property | Options | Default | Description |", "| --- | --- | --- | --- |"]
    for p in props:
        options = ", ".join(f"`{o}`" for o in p.get("options") or [])
        lines.append(f"| `{p['id']}` | {_cell(options)} | {_default(p.get('default'))} | {_cell(_flat(p.get('doc')))} |")
    return lines + [""]


def _number(v):
    return str(int(v)) if isinstance(v, float) and v.is_integer() and abs(v) < 1e15 else str(v)


def _value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return _number(v)
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(_value(x) for x in v) + "]"
    return str(v)


def _default(value):
    if value is None or value == "" or value == [] or value == {}:
        return ""
    return _cell(f"`{_value(value)}`")
