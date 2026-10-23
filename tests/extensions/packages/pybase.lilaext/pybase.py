from lila import extension


def initialize_extension():
    extension.register_command("pyBaseHello", lambda: "base", "Says base.")
