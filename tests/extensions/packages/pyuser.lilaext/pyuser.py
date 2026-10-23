import lila
from lila import extension


def initialize_extension():
    extension.register_command("pyUserHello", lambda: "user+" + lila.cmds.pyBaseHello(), "Says user and base.")
