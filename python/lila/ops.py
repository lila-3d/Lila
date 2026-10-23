"""Every Lila command as a function: lila.ops.name(*objects, flag=value).

Flags take long or short names. Generated from the live command registry, so extension commands appear
here too after refresh().
"""

import _lila

__all__ = []


def _make(spec):
    name = spec["name"]

    def command(*args, **flags):
        call = dict(flags)
        if args:
            call["args"] = list(args)
        return _lila.execute(name, call)

    command.__name__ = name
    command.__qualname__ = name
    lines = [spec["doc"]]
    if spec["flags"]:
        lines.append("")
        for f in spec["flags"]:
            short = f" ({f['short']})" if f["short"] and f["short"] != f["name"] else ""
            lines.append(f"{f['name']}{short}: {f['type']}  {f['doc']}".rstrip())
    command.__doc__ = "\n".join(lines)
    return command


def refresh():
    """Rebuild from the registry. Called at startup and when extensions load."""
    g = globals()
    for name in list(__all__):
        g.pop(name, None)
    __all__.clear()
    for spec in _lila.commands():
        g[spec["name"]] = _make(spec)
        __all__.append(spec["name"])
    try:
        from . import _startup

        _startup.expose_commands()
    except ImportError:
        pass


refresh()
