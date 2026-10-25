"""Walks the lila package for the API reference panel. Everything comes back as plain dicts, then JSON.

    {"name": "lila.math", "kind": "module", "doc": "...", "members": [
        {"name": "Vector", "qualname": "lila.math.Vector", "kind": "class", "signature": ["Vector(x=0.0, ...)"],
         "doc": "...", "bases": [...], "members": [{"kind": "method" | "property" | ..., ...}]}]}
"""

import importlib
import inspect
import json
import re

MODULES = ("lila", "lila.nodes", "lila.math", "lila.ui", "lila.prefs", "lila.extension")

_NB_DEFAULT = re.compile(r"\\(\d+)")
_SELF = re.compile(r"\((self|cls)(, |\))")
_NATIVE = re.compile(r"(?<![\w.])_lila\.")


def collect_json():
    return json.dumps(collect(), default=str)


def collect():
    out = []
    for name in MODULES:
        try:
            mod = importlib.import_module(name)
        except Exception:
            continue
        out.append(_module(mod))
    return out


def _module(mod):
    names = getattr(mod, "__all__", None)
    if names is None:
        names = [n for n in vars(mod) if not n.startswith("_")]
    members = []
    for name in names:
        if not hasattr(mod, name):
            continue
        obj = getattr(mod, name)
        if inspect.ismodule(obj) or not _owned(obj, mod.__name__):
            continue
        try:
            members.append(_member(name, obj, mod.__name__))
        except Exception as e:  # one odd object shouldn't sink the whole reference
            members.append({"name": name, "qualname": f"{mod.__name__}.{name}", "kind": "attribute", "doc": f"(could not inspect: {e})"})
    return {"name": mod.__name__, "qualname": mod.__name__, "kind": "module", "doc": _doc(mod), "members": members}


def _owned(obj, modname):
    """Defined here, or a native object this module is the public face of (_lila.math -> lila.math)."""
    if not (inspect.isclass(obj) or callable(obj)):
        return True
    owner = getattr(obj, "__module__", None) or ""
    if owner == modname:
        return True
    if owner.startswith("_lila"):
        public = "lila" + owner[len("_lila"):]
        return public == modname
    return False


def _member(name, obj, parent):
    qual = f"{parent}.{name}"
    if inspect.isclass(obj):
        return {
            "name": name,
            "qualname": qual,
            "kind": "class",
            "signature": _class_signature(name, obj),
            "doc": _doc(obj),
            "bases": [b.__name__ for b in obj.__bases__ if b is not object],
            "members": _class_members(obj, qual),
        }
    if callable(obj):
        sigs, doc = _signatures(name, obj)
        return {"name": name, "qualname": qual, "kind": "function", "signature": sigs, "doc": doc}
    return {"name": name, "qualname": qual, "kind": "attribute", "value": _repr(obj), "type": type(obj).__name__, "doc": ""}


def _class_members(cls, qual):
    out = []
    for name, raw in vars(cls).items():
        if name.startswith("_"):
            continue
        entry = {"name": name, "qualname": f"{qual}.{name}"}
        if isinstance(raw, property):
            entry["kind"] = "property"
            entry["readonly"] = raw.fset is None
            entry["doc"] = _doc(raw) or (_doc(raw.fget) if raw.fget else "")
            if raw.fget is not None:
                sigs, _ = _signatures(name, raw.fget)
                ret = sigs[0].split("->", 1)[1].strip() if sigs and "->" in sigs[0] else ""
                entry["signature"] = [f"{name}: {ret}" if ret else name]
        elif isinstance(raw, (staticmethod, classmethod)):
            entry["kind"] = "staticmethod" if isinstance(raw, staticmethod) else "classmethod"
            entry["signature"], entry["doc"] = _signatures(name, raw.__func__)
        elif callable(raw) or type(raw).__name__ in ("nb_method", "nb_func"):
            entry["kind"] = "method"
            entry["signature"], entry["doc"] = _signatures(name, raw)
        elif type(raw).__name__.endswith("property") or hasattr(raw, "__get__") and not hasattr(raw, "__call__"):
            # Native descriptors: nanobind properties, slots, getset descriptors.
            entry["kind"] = "property"
            entry["doc"] = _doc(raw)
            entry["readonly"] = not hasattr(raw, "__set__") or getattr(raw, "fset", 0) is None
        else:
            entry["kind"] = "attribute"
            entry["value"] = _repr(raw)
            entry["type"] = type(raw).__name__
            entry["doc"] = ""
        out.append(entry)
    return out


def _class_signature(name, cls):
    init = vars(cls).get("__init__")
    if init is not None and hasattr(init, "__nb_signature__"):
        sigs, _ = _signatures(name, init)
        return [name + s[len("__init__"):] if s.startswith("__init__") else s for s in sigs]
    try:
        return [name + str(inspect.signature(cls))]
    except (TypeError, ValueError):
        return [name + "(...)"]


def _signatures(name, fn):
    """Display signatures (overloads for native functions) without self, and the docstring without them."""
    nb = getattr(fn, "__nb_signature__", None)
    if nb:
        sigs, docs = [], []
        for item in nb:
            text, doc, defaults = (tuple(item) + (None, None, None))[:3]
            text = str(text)
            if text.startswith("def "):
                text = text[4:]
            if defaults:
                text = _NB_DEFAULT.sub(lambda m: _repr(defaults[int(m.group(1))]) if int(m.group(1)) < len(defaults) else m.group(0), text)
            sigs.append(_strip_self(text))
            if doc and doc not in docs:
                docs.append(inspect.cleandoc(doc))
        return sigs, "\n\n".join(docs)
    try:
        sig = inspect.signature(fn)
        params = list(sig.parameters.values())
        if params and params[0].name in ("self", "cls"):
            sig = sig.replace(parameters=params[1:])
        return [name + str(sig)], _doc(fn)
    except (TypeError, ValueError):
        pass
    doc = _doc(fn)
    lines = doc.splitlines()
    sigs = []
    while lines and lines[0].startswith(name + "("):
        sigs.append(_strip_self(lines.pop(0)))
    if sigs:
        doc = "\n".join(lines).strip()
    return sigs or [name + "(...)"], doc


def _strip_self(text):
    text = _NATIVE.sub("lila.", text)
    return _SELF.sub(lambda m: "(" if m.group(2) == ", " else "()", text, count=1)


def _doc(obj):
    try:
        if inspect.isclass(obj):
            # Only its own: getdoc falls back to a base's (RuntimeError's, Enum's), which isn't about this class.
            own = vars(obj).get("__doc__")
            return inspect.cleandoc(own) if isinstance(own, str) else ""
        doc = inspect.getdoc(obj)
        if not doc:
            return ""
        # Instances without their own docstring report their type's; that isn't about this member.
        type_doc = getattr(type(obj), "__doc__", None)
        if isinstance(type_doc, str) and doc == inspect.cleandoc(type_doc):
            return ""
        return doc
    except Exception:
        return ""


def _repr(value):
    try:
        text = repr(value)
    except Exception:
        text = "<?>"
    return text if len(text) <= 200 else text[:197] + "..."
