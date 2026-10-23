# Rig Templates, enabled by default: built as <build>/bin/<config>/extensions/RigTemplates.lilaext next to lila.exe.
lila_add_extension(lila_rig_templates_extension ID RigTemplates MANIFEST extension.json RESOURCES resources UI
    SOURCES Extension.cpp Template.cpp Ui.cpp)
