import os
import sys

import _lila


# Object API names put in the interactive namespace beside the commands; they don't collide with any.
API_NAMES = ("Node", "node", "selected", "exists")


class _LogWriter:
    def __init__(self, level):
        self._level = level

    def write(self, text):
        if text:
            _lila.write(self._level, text)
        return len(text)

    def flush(self):
        pass

    def isatty(self):
        return False


def _add_site_dir():
    """The user's Lila site-packages (tools installed for scripts) goes last on sys.path, behind the bundled PySide6."""
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA", "")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    site = os.path.join(base, "Lila", "site-packages")
    if os.path.isdir(site) and site not in sys.path:
        sys.path.append(site)


def boot():
    sys.stdout = _LogWriter(_lila.LOG_OUTPUT)
    sys.stderr = _LogWriter(_lila.LOG_ERROR)
    _add_site_dir()

    import lila  # noqa: F401  (registers lila in __main__ for the script editor)
    import __main__

    from . import usd

    usd._install_import_hook()

    __main__.lila = lila
    __main__.ops = lila.ops
    expose_commands()
    for name in API_NAMES:
        setattr(__main__, name, getattr(lila, name))

    _run_user_setup()
    try:
        from . import _stubgen

        _stubgen.write()
    except Exception:
        pass


def _run_user_setup():
    """userSetup.py from the user config dir, then from the workspace's .lila/."""
    from . import _runner

    roots = [os.environ.get("LILA_CONFIG_DIR") or _config_dir()]
    ws = _lila.workspace()
    if ws:
        roots.append(os.path.join(ws, ".lila"))
    for root in roots:
        path = os.path.join(root, "scripts", "userSetup.py")
        if os.path.isfile(path):
            _runner.run_file(path)


def _config_dir():
    if sys.platform == "win32":
        return os.path.join(os.environ.get("APPDATA", ""), "Lila")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/Lila")
    return os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "Lila")


def expose_commands():
    """Every command as a bare name in the interactive namespace: parent(), ls(sl=True).

    Python builtins always win, so a command that shares a builtin's name stays reachable only as ops.<name>.
    Called again after extensions add commands.
    """
    import builtins

    import __main__
    from . import ops

    for name in ops.__all__:
        if not hasattr(builtins, name):
            setattr(__main__, name, getattr(ops, name))
