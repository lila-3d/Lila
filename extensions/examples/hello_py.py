"""Sample Python extension.

Copy it into an extension folder (e.g. <workspace>/.lila/extensions/) or add this folder to general.json /extensions/paths, then:

    lila.cmds.loadExtension("hello_py")
    lila.cmds.helloPy("world", loud=True)
"""

import lila
from lila import extension


def hello_py(*names, loud=False):
    if not names:
        raise lila.CommandError("give someone to greet")
    text = "Hello, " + ", ".join(names)
    return text.upper() if loud else text


def initialize_extension():
    extension.register_command(
        "helloPy",
        hello_py,
        "Say hello from a Python extension.",
        positional="names",
        flags=[("loud", bool, False, "Shout.")],
    )


def uninitialize_extension():
    extension.unregister_command("helloPy")
