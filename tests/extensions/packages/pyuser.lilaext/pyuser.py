import lila
from lila import extension


def initialize_extension():
    extension.register_command("pyUserHello", lambda: "user+" + lila.ops.pyBaseHello(), "Says user and base.")
