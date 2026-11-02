"""EXAMPLE: a Python component exporter for Lila attribute components.

Add `import lila_example_exporter` to your project's Content/Python/init_unreal.py, then in Lila set a component's
Unreal export to custom with class LilaExamplePython. Each send logs the attributes and tags a placed actor
"LilaExamplePython:<component>". Copy it into your project and give it a name of your own to start your exporter.
"""

import unreal

import lila_link_components


def apply(actor, asset, component, attrs):
    target = actor.get_actor_label() if actor else (asset.get_path_name() if asset else "nothing")
    for name, value in attrs.items():
        unreal.log("LilaExamplePython: %s %s.%s = %r" % (target, component, name, value))
    if actor:
        tag = unreal.Name("LilaExamplePython:" + component)
        tags = list(actor.get_editor_property("tags"))
        if tag not in tags:
            actor.set_editor_property("tags", tags + [tag])


lila_link_components.register("LilaExamplePython", apply)
